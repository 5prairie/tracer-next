/* td_frame.c: builds the JSON frames of the stream from the ring module's threads and the
   store's instances. Every frame is built from stored history, so the live path and the
   replay for a new client are the same code. Frames carry their index: order is free. */
#define _GNU_SOURCE
#include <math.h>
#include <sys/utsname.h>

#include "td_frame.h"
#include "td_http.h"
#include "td_source.h"

#define TD_FRAME_BYTES               (1UL << 20)  /* the largest: hello with every instance */
#define TD_FRAME_STRING_BYTES        (256)        /* no string of a frame is longer */
#define TD_FRAME_REPLAY_BATCH_FRAMES (100)        /* history frames a new client gets per tick */
#define TD_FRAME_REPLAY_ROOM_BYTES   ( 4UL * TD_FRAME_BYTES )  /* a replay waits for this much */
/* Half a tick: what the replays may take of one together, past each page's first frame. */
#define TD_FRAME_REPLAY_BUDGET_NS    ( (uint64_t)TD_TICK_MS * TD_NS_PER_MS / 2UL )
#define TD_FRAME_WHOLE_MAX           (1e9)        /* below it a whole value is written whole */
#define TD_FRAME_PLAIN_MIN           (1e-5)       /* below it a float is left to printf */
#define TD_FRAME_DIGITS_MIN          (6)          /* significant digits tried first for a float */
#define TD_FRAME_DIGITS_MAX          (9)          /* and last: a float's all */
#define TD_FRAME_FLOAT_BYTES         (24)         /* sign, "0.", five zeroes, nine digits: 17 */

/* td_frame_replay_step's "none while the client's buffer is half full" holds while this does. */
_Static_assert( TD_FRAME_REPLAY_ROOM_BYTES <= TD_HTTP_OUT_BYTES / 2UL,
                "a replay waits while half of a client's buffer is taken" );

static char td_frame_data[ TD_FRAME_BYTES ];

/* The half-open ranges of slots and seconds that have been closed, hence can be replayed. */
static uint64_t td_frame_slot_first, td_frame_slot_end;
static uint64_t td_frame_second_first, td_frame_second_end;

/* Which of them ended while the daemon was stopped, by history index: sent to no one. */
static uint8_t td_frame_slot_is_gap  [ TD_FAST_CNT ];
static uint8_t td_frame_second_is_gap[ TD_SLOW_CNT ];

/* Where each client's replay stands: the next second and slot to send, up to the end as it
   was when the client connected. Live frames cover what closes after that. */
typedef struct {
  int      is_active;
  uint64_t second, second_end;
  uint64_t slot, slot_end;
} td_frame_replay_t;

static td_frame_replay_t td_frame_replay[ TD_HTTP_CLIENT_MAX ];

static char const * const td_frame_widget[] = {
  [ TD_LANE ] = "lane", [ TD_TABLE ] = "table", [ TD_CHECK ] = "check",
};
/* To the page a count since the source began is a total like any other: it only grows. */
static char const * const td_frame_kind[] = {
  [ TD_LEVEL ] = "level", [ TD_RATE ] = "rate", [ TD_TOTAL ] = "total", [ TD_SINCE ] = "total",
};
static char const * const td_frame_scope[] = {
  [ TD_SCOPE_HOST ] = "host", [ TD_SCOPE_PROCESS ] = "process", [ TD_SCOPE_THREAD ] = "thread",
  [ TD_SCOPE_CORE ] = "core",
};

/* A float in the fewest of 6 to 9 significant digits that read back as the same float,
   which is all the page keeps of a value. No format is parsed: %.9g was most of the time a
   frame took. The digits are tried on a double: the float times a power of ten, and the
   candidate over that power, are each rounded once, as the reader of the text rounds once.
   Outside [1e-5, 1e9), or should no candidate read back, printf writes it. */
static void td_frame_float( td_text_t * text, float value ) {
  static double const power[] = { 1e0, 1e1, 1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                  1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15 };
  char     byte[ TD_FRAME_FLOAT_BYTES ], digit[ TD_FRAME_DIGITS_MAX ];
  double   size     = fabs( (double)value );
  uint64_t mantissa = 0UL;
  int      exponent = 0, digit_cnt = TD_FRAME_DIGITS_MAX + 1, whole_cnt, byte_cnt = 0;

  if( size >= TD_FRAME_PLAIN_MIN && size < TD_FRAME_WHOLE_MAX ) {
    /* 10^exponent <= size < 10^(exponent+1): -5 to 8, or -6 when a product rounded down. */
    if( size < 1.0 ) {
      exponent = -1;
      while( size * power[ -exponent ] < 1.0 ) exponent--;
    } else while( size >= power[ exponent + 1 ] ) exponent++;
    for( digit_cnt = TD_FRAME_DIGITS_MIN; digit_cnt <= TD_FRAME_DIGITS_MAX; digit_cnt++ ) {
      int    shift = digit_cnt - 1 - exponent;  /* mantissa = size x 10^shift: -3 to 14 */
      double back;
      mantissa = (uint64_t)( ( shift >= 0 ? size * power[ shift ]
                                          : size / power[ -shift ] ) + 0.5 );
      back     = shift >= 0 ? (double)mantissa / power[ shift ]
                            : (double)mantissa * power[ -shift ];
      if( (double)mantissa < power[ digit_cnt ] && (float)back == (float)size ) break;
    }
  }
  if( digit_cnt > TD_FRAME_DIGITS_MAX ) {
    td_text_printf( text, "%.9g", (double)value );
    return;
  }
  for( int i = digit_cnt - 1; i >= 0; i-- ) {
    digit[ i ] = (char)( '0' + mantissa % 10UL );
    mantissa  /= 10UL;
  }
  whole_cnt = exponent + 1;  /* digits before the point; 0 or less: zeroes after it first */
  while( digit_cnt > 1 && digit_cnt > whole_cnt && digit[ digit_cnt - 1 ] == '0' )
    digit_cnt--;             /* zeroes that end a fraction say nothing */
  if( value < 0.0f ) byte[ byte_cnt++ ] = '-';
  if( whole_cnt <= 0 ) {
    byte[ byte_cnt++ ] = '0';
    byte[ byte_cnt++ ] = '.';
    for( int i = whole_cnt; i < 0; i++ ) byte[ byte_cnt++ ] = '0';
  }
  for( int i = 0; i < digit_cnt; i++ ) {
    if( i && i == whole_cnt ) byte[ byte_cnt++ ] = '.';
    byte[ byte_cnt++ ] = digit[ i ];
  }
  for( int i = digit_cnt; i < whole_cnt; i++ ) byte[ byte_cnt++ ] = '0';  /* whole digits dropped */
  td_text_put( text, byte, (size_t)byte_cnt );
}

/* A number, or null: JSON has no NaN. Written as a float: the store's values are floats. */
static void td_frame_number( td_text_t * text, double value ) {
  if( isnan( value ) || isinf( value ) ) { TD_TEXT_LITERAL( text, "null" ); return; }
  /* A whole value, as most are, is its digits. */
  if( value >= 0.0 && value < TD_FRAME_WHOLE_MAX && value == (double)(uint64_t)value )
    td_text_uint( text, (uint64_t)value );
  else if( fabs( value ) >= TD_FRAME_PLAIN_MIN && fabs( value ) < TD_FRAME_WHOLE_MAX )
    td_frame_float( text, (float)value );
  else
    td_text_printf( text, "%.9g", value );  /* a threshold may be more or less than a float holds */
}

static void td_frame_stat( td_text_t * text, td_stat_t const * stat ) {
  td_text_put ( text, "[", 1UL );
  td_text_uint( text, stat->p50_ns );
  td_text_put ( text, ",", 1UL );
  td_text_uint( text, stat->p99_ns );
  td_text_put ( text, ",", 1UL );
  td_text_uint( text, stat->max_ns );
  td_text_put ( text, ",", 1UL );
  td_text_uint( text, stat->cnt );
  td_text_put ( text, "]", 1UL );
}

static void td_frame_send( td_text_t const * text, int client_idx ) {
  if( text->is_overflow ) {
    TD_LOG_ONCE( "tracerd: a frame did not fit and was not sent\n" );
    return;
  }
  if( client_idx < 0 ) td_http_broadcast( text->data, text->byte_cnt );
  else                 td_http_send( client_idx, text->data, text->byte_cnt );
}

/* ---- hello ----------------------------------------------------------------------------- */

static void td_frame_hello_thread( td_text_t * text, int thread_idx ) {
  td_thread_t const * thread = &td_thread[ thread_idx ];

  td_text_printf( text, "{\"i\":%d,\"name\":", thread_idx );
  td_text_json( text, thread->name, sizeof( thread->name ) );
  td_text_printf( text, ",\"pid\":%u,\"tid\":%u,\"cpu\":%d,\"gone\":%d", thread->pid, thread->tid,
                  thread->cpu, thread->is_gone );
  td_text_printf( text, ",\"sites\":[" );
  for( int site = 0; site < thread->site_cnt; site++ ) {
    td_text_printf( text, "%s{\"name\":", site ? "," : "" );
    td_text_json( text, thread->site[ site ].name, sizeof( thread->site[ site ].name ) );
    td_text_printf( text, ",\"root\":%u,\"gauge\":%u}", thread->site[ site ].flags & TRACE_ROOT,
                    thread->is_gauge[ site ] );
  }
  td_text_printf( text, "]}" );
}

static void td_frame_hello_source( td_text_t * text, int source_idx ) {
  td_source_state_t const * state  = &td_source_state[ source_idx ];
  td_source_t const *       source = state->source;
  td_text_printf( text, "{\"name\":" );
  td_text_json( text, source->name, TD_FRAME_STRING_BYTES );
  td_text_printf( text, ",\"scope\":\"%s\",\"on\":%d,\"unproven\":%d,\"metrics\":[",
                  td_frame_scope[ source->scope ], state->is_on, source->is_unproven );
  for( int metric_idx = 0; metric_idx < source->metric_cnt; metric_idx++ ) {
    td_metric_t const * metric   = &source->metric[ metric_idx ];
    int                 is_state = metric_idx < TD_METRIC_MAX;  /* the store keeps that many */
    td_text_printf( text, "%s{\"id\":", metric_idx ? "," : "" );
    td_text_json( text, metric->id, TD_FRAME_STRING_BYTES );
    td_text_printf( text, ",\"label\":" );
    td_text_json( text, metric->label, TD_FRAME_STRING_BYTES );
    td_text_printf( text, ",\"unit\":" );
    td_text_json( text, metric->unit, TD_FRAME_STRING_BYTES );
    td_text_printf( text, ",\"widget\":\"%s\",\"kind\":\"%s\",\"amber\":",
                    td_frame_widget[ metric->widget ], td_frame_kind[ metric->kind ] );
    td_frame_number( text, is_state ? state->amber[ metric_idx ] : metric->amber );
    td_text_printf( text, ",\"red\":" );
    td_frame_number( text, is_state ? state->red[ metric_idx ] : metric->red );
    td_text_printf( text, "}" );
  }
  td_text_printf( text, "]}" );
}

static void td_frame_hello_lists( td_text_t * text ) {
  int is_first = 1;
  td_text_printf( text, ",\"instances\":[" );
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t const * instance = &td_instance[ i ];
    if( !instance->is_live ) continue;
    td_text_printf( text, "%s{\"i\":%d,\"source\":%d,\"metric\":%d,\"thread\":%d",
                    is_first ? "" : ",", i, instance->source_idx, instance->metric_idx,
                    instance->thread_idx );
    td_text_printf( text, ",\"resolution_ms\":%d,\"key\":", instance->resolution_ms );
    td_text_json( text, instance->key, TD_FRAME_STRING_BYTES );
    td_text_printf( text, "}" );
    is_first = 0;
  }
  td_text_printf( text, "],\"unavailable\":[" );
  for( int i = 0; i < td_unavailable_cnt; i++ ) {
    td_unavailable_t const * entry  = &td_unavailable_list[ i ];
    td_source_t const *      source = td_source_state[ entry->source_idx ].source;
    td_text_printf( text, "%s{\"source\":", i ? "," : "" );
    td_text_json( text, source->name, TD_FRAME_STRING_BYTES );
    td_text_printf( text, ",\"metric\":" );
    td_text_json( text, entry->metric_idx < 0 ? "" : source->metric[ entry->metric_idx ].label,
                  TD_FRAME_STRING_BYTES );
    td_text_printf( text, ",\"scope\":\"%s\",\"thread\":%d,\"why\":",
                    td_frame_scope[ source->scope ], entry->thread_idx );
    td_text_json( text, entry->why, TD_FRAME_STRING_BYTES );
    td_text_printf( text, "}" );
  }
  td_text_printf( text, "]" );
}

static void td_frame_hello( td_text_t * text ) {
  struct utsname kernel;
  int            is_first = 1;
  uint64_t       slot_ns  = (uint64_t)( TD_TICK_MS * TD_SLOT_TICKS ) * TD_NS_PER_MS;
  /* The slot being filled, by the clock: after a long stop the last one closed is old, and
     the page reads the time of every column from this pair of slot and wall time. */
  uint64_t       slot     = td_clock_ns() / slot_ns;
  uname( &kernel );
  td_text_init( text, td_frame_data, sizeof( td_frame_data ) );
  td_text_printf( text, "{\"type\":\"hello\",\"host\":" );
  td_text_json( text, kernel.nodename, TD_FRAME_STRING_BYTES );
  td_text_printf( text, ",\"arch\":\"%s\",\"kernel\":", TRACE_ARCH_NAME );
  td_text_json( text, kernel.release, TD_FRAME_STRING_BYTES );
  td_text_printf( text, ",\"slot_ms\":%d,\"fast_cnt\":%d,\"slow_cnt\":%d,\"tsc_ns\":%.3f",
                  TD_TICK_MS * TD_SLOT_TICKS, TD_FAST_CNT, TD_SLOW_CNT, td_ring_tsc_ns() );
  td_text_printf( text, ",\"mark_kind\":%d", (int)TRACE_KIND_MARK );  /* which spans are instants */
  /* What time it is at the slot being filled: the page's time axis starts from this. */
  td_text_printf( text, ",\"slot\":%lu,\"wall_ms\":%lu", (unsigned long)slot,
                  (unsigned long)td_wall_ms() );
  td_text_printf( text, ",\"threads\":[" );
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    if( !td_thread[ i ].is_live ) continue;
    td_text_printf( text, "%s", is_first ? "" : "," );
    td_frame_hello_thread( text, i );
    is_first = 0;
  }
  td_text_printf( text, "],\"sources\":[" );
  for( int i = 0; i < td_source_cnt; i++ ) {
    td_text_printf( text, "%s", i ? "," : "" );
    td_frame_hello_source( text, i );
  }
  td_text_printf( text, "]" );
  td_frame_hello_lists( text );
  td_text_printf( text, "}" );
}

/* ---- tick and second -------------------------------------------------------------------- */

static void td_frame_slowest( td_text_t * text, td_trace_t const * trace ) {
  if( !trace->length_ns ) return;
  td_text_printf( text, ",\"slowest\":{\"wall_ms\":%lu,\"ns\":%lu,\"value\":%lu,\"spans\":[",
                  (unsigned long)trace->wall_ms, (unsigned long)trace->length_ns,
                  (unsigned long)trace->value );
  for( uint32_t i = 0U; i < trace->span_cnt; i++ ) {
    td_text_put ( text, &",["[ !i ], i ? 2UL : 1UL );  /* the comma: not before the first */
    td_text_uint( text, trace->span[ i ].site );
    td_text_put ( text, ",", 1UL );
    td_text_uint( text, trace->span[ i ].kind );
    td_text_put ( text, ",", 1UL );
    td_text_uint( text, trace->span[ i ].start_ns );
    td_text_put ( text, ",", 1UL );
    td_text_uint( text, trace->span[ i ].length_ns );
    td_text_put ( text, "]", 1UL );
  }
  TD_TEXT_LITERAL( text, "]}" );
}

/* A tick and a second name no instance, and a tick no thread: their numbers are in the order
   of the lists of the hello that describes the same tables. This says which tables a frame
   was built from: the live threads and instances, in that order. The page computes the same
   number from a hello (main.js tablesOf) and holds a frame that comes ahead of its hello
   until that hello is there, so a frame is never read against other tables than its own. */
static uint32_t td_frame_tables( void ) {
  uint32_t sum = 0U;
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t const * thread = &td_thread[ i ];
    if( !thread->is_live ) continue;
    sum = sum * 31U + (uint32_t)i;
    sum = sum * 31U + thread->tid;
    sum = sum * 31U + (uint32_t)thread->site_cnt;
  }
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t const * instance = &td_instance[ i ];
    if( !instance->is_live ) continue;
    sum = sum * 31U + (uint32_t)i;
    sum = sum * 31U + (uint32_t)instance->source_idx;
    sum = sum * 31U + (uint32_t)instance->metric_idx;
    sum = sum * 31U + (uint32_t)instance->thread_idx;
  }
  return sum;
}

/* 1 when the thread completed nothing in the slot or second of `stat`: every count is 0. */
static int td_frame_is_idle( td_thread_t const * thread, td_stat_t const * stat ) {
  for( int site = 0; site < thread->site_cnt; site++ )
    if( stat[ site ].cnt ) return 0;
  return !stat[ TD_SITE_MAX ].cnt;
}

/* The latency of every thread at history index `idx` of `fast` (is_fast) or `slow`.
   A tick: "t", per thread in hello's order, the whole trace's [p50, p99, max, count]: the
   page draws nothing else of a slot. A second: "threads", each with its index, one such
   stat per site then the whole trace's, and its slowest trace.
   A thread that did nothing in this slot and in the one before (one that is gone, one that
   waits) is null in "t" and not listed in "threads": the page reads that as an empty
   sample. The first idle slot after a busy one is still sent, as zeros. */
static void td_frame_threads( td_text_t * text, uint64_t idx, int is_fast ) {
  uint64_t idx_cnt  = is_fast ? TD_FAST_CNT : TD_SLOW_CNT;
  uint64_t before   = ( idx + idx_cnt - 1UL ) % idx_cnt;
  int      is_first = 1;
  TD_TEXT_LITERAL( text, ",\"g\":" );
  td_text_uint( text, td_frame_tables() );
  if( is_fast ) TD_TEXT_LITERAL( text, ",\"t\":[" );
  else          TD_TEXT_LITERAL( text, ",\"threads\":[" );
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t const * thread  = &td_thread[ i ];
    td_stat_t const *   stat    = is_fast ? thread->fast[ idx ]    : thread->slow[ idx ];
    td_stat_t const *   last    = is_fast ? thread->fast[ before ] : thread->slow[ before ];
    int                 is_idle;
    if( !thread->is_live ) continue;
    is_idle = td_frame_is_idle( thread, stat ) && td_frame_is_idle( thread, last );
    if( is_fast ) {
      if( !is_first ) TD_TEXT_LITERAL( text, "," );
      if( is_idle ) TD_TEXT_LITERAL( text, "null" );
      else          td_frame_stat( text, &stat[ TD_SITE_MAX ] );
      is_first = 0;
      continue;
    }
    if( is_idle ) continue;
    if( is_first ) TD_TEXT_LITERAL( text, "{\"i\":" );
    else           TD_TEXT_LITERAL( text, ",{\"i\":" );
    td_text_uint( text, (uint64_t)i );
    TD_TEXT_LITERAL( text, ",\"lat\":[" );
    for( int site = 0; site < thread->site_cnt; site++ ) {
      td_frame_stat( text, &stat[ site ] );
      TD_TEXT_LITERAL( text, "," );
    }
    td_frame_stat( text, &stat[ TD_SITE_MAX ] );
    TD_TEXT_LITERAL( text, "]" );
    td_frame_slowest( text, &thread->slowest[ idx ] );
    TD_TEXT_LITERAL( text, "}" );
    is_first = 0;
  }
  TD_TEXT_LITERAL( text, "]" );
}

/* "v": one value per instance in hello's order, null where it has none in this slot or
   second. Most values are 0: no index to carry makes a frame less than half as long. */
static void td_frame_values( td_text_t * text, uint64_t idx, int is_fast ) {
  int is_first = 1;
  TD_TEXT_LITERAL( text, ",\"v\":[" );
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t const * instance = &td_instance[ i ];
    if( !instance->is_live ) continue;
    if( !is_first ) TD_TEXT_LITERAL( text, "," );
    td_frame_number( text, is_fast ? instance->fast[ idx ] : instance->slow[ idx ] );  /* or null */
    is_first = 0;
  }
  TD_TEXT_LITERAL( text, "]" );
}

/* What the sources cost right now: [cost_us, period_ms] each. Only live seconds carry it. */
static void td_frame_costs( td_text_t * text ) {
  TD_TEXT_LITERAL( text, ",\"sources\":[" );
  for( int i = 0; i < td_source_cnt; i++ )
    td_text_printf( text, "%s[%u,%d]", i ? "," : "", td_source_state[ i ].cost_us,
                    td_source_state[ i ].period_ticks * TD_TICK_MS );
  TD_TEXT_LITERAL( text, "]" );
}

static void td_frame_build( td_text_t * text, uint64_t number, int is_fast, int is_live ) {
  uint64_t idx = number % ( is_fast ? TD_FAST_CNT : TD_SLOW_CNT );
  td_text_init( text, td_frame_data, sizeof( td_frame_data ) );
  if( is_fast ) TD_TEXT_LITERAL( text, "{\"type\":\"tick\",\"index\":" );
  else          TD_TEXT_LITERAL( text, "{\"type\":\"second\",\"index\":" );
  td_text_uint( text, number );
  td_frame_threads( text, idx, is_fast );
  td_frame_values ( text, idx, is_fast );
  if( !is_fast && is_live ) td_frame_costs( text );
  TD_TEXT_LITERAL( text, "}" );
}

void td_frame_hello_all( void ) {
  td_text_t text;
  if( !td_http_has_stream() ) return;
  td_frame_hello( &text );
  td_frame_send( &text, -1 );
}

/* A slot that is not known is remembered as a gap and built for no one: after a long stop
   there are up to TD_FAST_CNT of them in one tick, and the page shows a gap for an index
   it was not sent. */
void td_frame_tick( uint64_t slot, int is_known ) {
  td_text_t text;
  if( !td_frame_slot_end ) td_frame_slot_first = slot;
  td_frame_slot_end = slot + 1UL;
  if( td_frame_slot_end - td_frame_slot_first > TD_FAST_CNT )
    td_frame_slot_first = td_frame_slot_end - TD_FAST_CNT;
  td_frame_slot_is_gap[ slot % TD_FAST_CNT ] = !is_known;
  if( !is_known || !td_http_has_stream() ) return;  /* the past is built when someone asks for it */
  td_frame_build( &text, slot, 1, 1 );
  td_frame_send( &text, -1 );
}

void td_frame_second( uint64_t second, int is_known ) {
  td_text_t text;
  if( !td_frame_second_end ) td_frame_second_first = second;
  td_frame_second_end = second + 1UL;
  if( td_frame_second_end - td_frame_second_first > TD_SLOW_CNT )
    td_frame_second_first = td_frame_second_end - TD_SLOW_CNT;
  td_frame_second_is_gap[ second % TD_SLOW_CNT ] = !is_known;
  if( !is_known || !td_http_has_stream() ) return;
  td_frame_build( &text, second, 0, 1 );
  td_frame_send( &text, -1 );
}

void td_frame_replay_start( int client_idx ) {
  td_text_t text;
  td_frame_hello( &text );
  td_frame_send( &text, client_idx );
  td_frame_replay[ client_idx ].is_active  = 1;
  td_frame_replay[ client_idx ].second     = td_frame_second_first;
  td_frame_replay[ client_idx ].second_end = td_frame_second_end;
  td_frame_replay[ client_idx ].slot       = td_frame_slot_first;
  td_frame_replay[ client_idx ].slot_end   = td_frame_slot_end;
}

/* A bounded number of frames and a bounded time per tick, and none while the client's
   buffer is half full: a replay must neither stall the loop nor overrun the client. A
   page's first frame of a tick is outside the time, so a replay always ends. History that has been
   overwritten since the client connected is skipped, and so are the gaps: slots and seconds
   that were not known when they closed, which no page was sent. The frames are queued:
   the caller's td_http_flush_all sends each page's batch at once. */
void td_frame_replay_step( void ) {
  td_text_t text;
  uint64_t  start_ns = 0UL;
  for( int client_idx = 0; client_idx < TD_HTTP_CLIENT_MAX; client_idx++ ) {
    td_frame_replay_t * replay = &td_frame_replay[ client_idx ];
    size_t              room_bytes;
    if( !replay->is_active ) continue;
    if( !start_ns ) start_ns = td_clock_ns();
    if( replay->second < td_frame_second_first ) replay->second = td_frame_second_first;
    if( replay->slot   < td_frame_slot_first   ) replay->slot   = td_frame_slot_first;
    for( int i = 0; i < TD_FRAME_REPLAY_BATCH_FRAMES && replay->is_active; i++ ) {
      int is_stream = td_http_is_stream( client_idx, &room_bytes );
      if( !is_stream ) { replay->is_active = 0; break; }
      if( room_bytes < TD_FRAME_REPLAY_ROOM_BYTES ) break;
      if( i && td_clock_ns() - start_ns > TD_FRAME_REPLAY_BUDGET_NS ) break;
      while( replay->second < replay->second_end &&
             td_frame_second_is_gap[ replay->second % TD_SLOW_CNT ] ) replay->second++;
      while( replay->slot < replay->slot_end &&
             td_frame_slot_is_gap[ replay->slot % TD_FAST_CNT ] ) replay->slot++;
      if( replay->second < replay->second_end )  td_frame_build( &text, replay->second++, 0, 0 );
      else if( replay->slot < replay->slot_end ) td_frame_build( &text, replay->slot++, 1, 0 );
      else { replay->is_active = 0; break; }
      td_frame_send( &text, client_idx );
    }
  }
}
