/* td_ring.c: drains the rings of traced threads and turns events into spans, traces and
   latency statistics. Reads shared memory only: the traced thread never waits for it and
   never knows whether it runs. td_ring_find.c finds the rings. */
#include "td_ring.h"

td_thread_t td_thread[ TD_THREAD_MAX ];

static double        td_ring_ns_per_tsc;
/* Counts to nanoseconds in whole numbers: ns = ( ( cnt << shift ) * mult ) >> 64, where mult
   is ns_per_tsc * 2^( 64 - shift ) and shift the least that keeps it under 2^64: 0 whenever a
   count is under a nanosecond. On x86 a multiply where the same in floating point is two
   conversions and two compares; one way or the other, the nanoseconds are the same. */
static uint64_t      td_ring_mult;
static int           td_ring_shift;
static uint64_t      td_ring_tsc_max  = UINT64_MAX;    /* more counts are INT64_MAX ns */
static uint64_t      td_ring_idle_tsc = UINT64_MAX;    /* TD_IDLE_CLOSE_NS in timestamp counts */
static trace_event_t td_ring_batch[ TD_DRAIN_BATCH_EVENTS ];  /* a copy: the ring moves */
static uint64_t      td_ring_batch_wall_ms;            /* the clocks, read once per batch */
static uint64_t      td_ring_batch_tsc;
/* The slots that may be a thread's, one bit each: a hint, checked against the slot. All are
   set until td_ring_follow is first called; from then on a slot found free loses its bit. */
static uint32_t      td_ring_slot_mask = ( 1U << TD_THREAD_MAX ) - 1U;
static int           td_ring_is_told;

_Static_assert( TD_THREAD_MAX < 32, "td_ring_slot_mask has a bit per slot" );

void td_ring_init( double ns_per_tsc ) {
  double   count_limit;
  uint64_t last_place;
  td_ring_ns_per_tsc = ns_per_tsc;
  if( !( ns_per_tsc > 0.0 ) ) return;
  td_ring_idle_tsc = (uint64_t)( (double)TD_IDLE_CLOSE_NS / ns_per_tsc );
  td_ring_shift    = 0;
  while( td_ring_shift < 63 && ldexp( ns_per_tsc, -td_ring_shift ) >= 1.0 ) td_ring_shift++;
  td_ring_mult = (uint64_t)ldexp( ns_per_tsc, 64 - td_ring_shift );
  /* One unit of the double's last place up. The floating product was rounded to nearest
     before it was cut to whole nanoseconds, and a length that is a whole number of them
     came out whole; cut only, it would come out one short. */
  last_place = td_ring_mult >> 52 ? 1UL << ( 11 - __builtin_clzl( td_ring_mult ) ) : 1UL;
  if( td_ring_mult && td_ring_mult <= UINT64_MAX - last_place ) td_ring_mult += last_place;
  /* Past this many counts the product is no time in nanoseconds, or the shift loses bits.
     The limit is taken a little short, for the unit added above. */
  count_limit     = (double)INT64_MAX / ns_per_tsc;
  td_ring_tsc_max = count_limit < 0x1p64 ? (uint64_t)count_limit : UINT64_MAX;
  if( count_limit < 0x1p64 ) td_ring_tsc_max -= td_ring_tsc_max >> 40;
  if( td_ring_shift && td_ring_tsc_max >> ( 64 - td_ring_shift ) )
    td_ring_tsc_max = ( 1UL << ( 64 - td_ring_shift ) ) - 1UL;
}

void td_ring_follow( td_thread_t const * thread ) {
  td_ring_is_told    = 1;
  td_ring_slot_mask |= 1U << ( thread - td_thread );
}

/* The first live slot from `idx` on, or TD_THREAD_MAX. td_thread is 20 MB: a slot is looked
   at only if it may be a thread's, because every look at one is a cache and a TLB miss. */
static int td_ring_next_live( int idx ) {
  for( ; idx < TD_THREAD_MAX; idx++ ) {
    if( !( ( td_ring_slot_mask >> idx ) & 1U ) ) continue;
    if( td_thread[ idx ].is_live ) return idx;
    if( td_ring_is_told ) td_ring_slot_mask &= ~( 1U << idx );
  }
  return TD_THREAD_MAX;
}

double td_ring_tsc_to_ms( uint64_t tsc_cnt ) {
  return (double)tsc_cnt * td_ring_ns_per_tsc / (double)TD_NS_PER_MS;
}

double td_ring_tsc_ns( void ) { return td_ring_ns_per_tsc; }

/* Timestamps come from the ring, which is not trusted: a difference too large to be a
   time in nanoseconds is the largest that is one. */
TRACE_INLINE uint64_t td_ring_tsc_to_ns( uint64_t tsc_cnt ) {
  unsigned __int128 product = (unsigned __int128)( tsc_cnt << td_ring_shift ) * td_ring_mult;
  return tsc_cnt > td_ring_tsc_max ? (uint64_t)INT64_MAX : (uint64_t)( product >> 64 );
}

static uint32_t td_ring_clamp( uint64_t value ) {
  return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* ---- events into spans and traces ------------------------------------------------------ */

static void td_ring_trace_close( td_thread_t * thread ) {
  uint64_t length_ns;
  if( !thread->in_trace ) return;
  thread->in_trace = 0;
  if( thread->last_end_tsc <= thread->root_tsc ) return;  /* a root with nothing after it */
  length_ns = td_ring_tsc_to_ns( thread->last_end_tsc - thread->root_tsc );
  td_hist_add( &thread->slot_hist[ TD_SITE_MAX ], length_ns );  /* the second's: at slot close */
  thread->trace_cnt++;
  thread->open_trace.length_ns = length_ns;
  if( length_ns >= thread->slowest_open.length_ns ) thread->slowest_open = thread->open_trace;
}

/* This and the three after the counter are inlined into the drain's loop, whatever the
   compiler thinks: they run once or more per event. */
TRACE_INLINE void td_ring_span_keep( td_thread_t * thread, uint64_t site, uint64_t kind,
                                     uint64_t start_tsc, uint64_t length_ns ) {
  td_trace_t * trace = &thread->open_trace;
  td_span_t *  span;
  if( !thread->in_trace || trace->span_cnt >= TD_SPAN_MAX || start_tsc < thread->root_tsc ) return;
  span            = &trace->span[ trace->span_cnt++ ];
  span->site      = (uint16_t)site;
  span->kind      = (uint16_t)kind;
  span->start_ns  = td_ring_clamp( td_ring_tsc_to_ns( start_tsc - thread->root_tsc ) );
  span->length_ns = td_ring_clamp( length_ns );
}

static void td_ring_counter( td_thread_t * thread, uint64_t counter_idx, uint64_t tsc,
                             uint64_t value ) {
  td_counter_t * counter;
  if( counter_idx >= TRACE_COUNTER_MAX ) return;
  counter = &thread->counter[ counter_idx ];
  if( counter->has_last ) {
    counter->delta_sum += ( value - counter->last_value ) & TRACE_VALUE_MASK;
    counter->trace_sum += thread->trace_cnt - counter->last_trace_cnt;
    counter->tsc_sum   += tsc - counter->last_tsc;
  }
  counter->last_value     = value;
  counter->last_trace_cnt = thread->trace_cnt;
  counter->last_tsc       = tsc;
  counter->has_last       = 1;
}

/* How long ago an event was written: it is read up to a tick later, more when replayed. */
static uint64_t td_ring_age_ms( uint64_t event_tsc ) {
  uint64_t now_tsc = td_ring_batch_tsc;
  return now_tsc > event_tsc ? (uint64_t)td_ring_tsc_to_ms( now_tsc - event_tsc ) : 0UL;
}

TRACE_INLINE void td_ring_begin( td_thread_t * thread, uint64_t site, uint64_t tsc,
                                 uint64_t value ) {
  if( thread->site[ site ].flags & TRACE_ROOT ) {
    td_ring_trace_close( thread );
    thread->in_trace            = 1;
    thread->root_tsc            = tsc;
    thread->last_end_tsc        = 0UL;
    thread->open_trace.wall_ms  = td_ring_batch_wall_ms - td_ring_age_ms( tsc );
    thread->open_trace.value    = value;
    thread->open_trace.span_cnt = 0U;
  }
  thread->begin_tsc[ site ] = tsc;
}

TRACE_INLINE void td_ring_end( td_thread_t * thread, uint64_t site, uint64_t tsc ) {
  uint64_t begin_tsc = thread->begin_tsc[ site ], length_ns;
  if( !begin_tsc ) return;  /* an end whose begin was lost */
  thread->begin_tsc[ site ] = 0UL;
  /* An end stamped before its begin (a clock that went back) has no length to count; but
     the span is closed: left open, it would keep its trace from ever closing on silence. */
  if( tsc < begin_tsc ) return;
  length_ns = td_ring_tsc_to_ns( tsc - begin_tsc );
  td_hist_add( &thread->slot_hist[ site ], length_ns );
  td_ring_span_keep( thread, site, TRACE_KIND_END, begin_tsc, length_ns );
  thread->last_end_tsc = tsc;
}

static int td_ring_has_open_span( td_thread_t const * thread ) {
  for( int site = 0; site < thread->site_cnt; site++ ) if( thread->begin_tsc[ site ] ) return 1;
  return 0;
}

/* Returns 1 for an event a trace is made of (begin, end, mark), 0 for the others: a counter
   sample or a gauge, which an idle thread goes on writing, must not keep a trace open. */
TRACE_INLINE int td_ring_event( td_thread_t * thread, trace_event_t const * event ) {
  uint64_t kind  = event->word >> TRACE_KIND_SHIFT;
  uint64_t site  = ( event->word >> TRACE_SITE_SHIFT ) & TRACE_SITE_MASK;
  uint64_t value = event->word & TRACE_VALUE_MASK;

  if( kind == TRACE_KIND_COUNTER ) { td_ring_counter( thread, site, event->tsc, value ); return 0; }
  if( site >= (uint64_t)thread->site_cnt || kind > TRACE_KIND_GAUGE ) return 0;
  if( kind == TRACE_KIND_GAUGE ) {
    thread->is_gauge  [ site ] = 1;
    thread->gauge_last[ site ] = value;
    if( value > thread->gauge_max[ site ] ) thread->gauge_max[ site ] = value;
    return 0;
  }
  /* Silence by the events' own clock too: what a ring held for long is read in one batch,
     and the daemon's clock then sees no silence between a trace and what came much later.
     One compare says there was none; a clock that went back gives a difference as large as
     a silence, and is told from one only then. */
  if( __builtin_expect( event->tsc - thread->last_trace_tsc > td_ring_idle_tsc, 0 ) &&
      thread->in_trace && event->tsc > thread->last_trace_tsc && !td_ring_has_open_span( thread ) )
    td_ring_trace_close( thread );
  thread->last_trace_tsc = event->tsc;
  if(      kind == TRACE_KIND_BEGIN ) td_ring_begin( thread, site, event->tsc, value );
  else if( kind == TRACE_KIND_END   ) td_ring_end  ( thread, site, event->tsc );
  else {  /* a mark */
    td_hist_add( &thread->slot_hist[ site ], 0UL );
    td_ring_span_keep( thread, site, TRACE_KIND_MARK, event->tsc, 0UL );
  }
  return 1;
}

/* ---- drain ----------------------------------------------------------------------------- */

/* Events were lost: the spans and the trace that were open have lost their other half,
   and a counter's next sample no longer follows its last one. */
static void td_ring_lose( td_thread_t * thread, uint64_t lost_cnt ) {
  thread->lost_cnt += lost_cnt;
  thread->seen_cnt += lost_cnt;
  thread->in_trace  = 0;
  memset( thread->begin_tsc, 0, sizeof( thread->begin_tsc ) );
  for( int i = 0; i < TRACE_COUNTER_MAX; i++ ) thread->counter[ i ].has_last = 0;
}

/* Drain up to the cursor as it was on entry. Before every batch the cursor is read again:
   the reader never copies within a quarter ring (safe_cnt) of where the writer may be,
   because a writer that laps a reader's copy stalls. A batch is trusted
   only once the cursor, read after the copy, proves that it was not overwritten meanwhile;
   the slack is for a writer whose next event's stores are seen before its cursor's
   (aarch64 orders the cursor after the event it counts, not before the next one). A dead
   writer (is_dead) moves nothing: all its ring holds is read. */
static void td_ring_drain_one( td_thread_t * thread, uint64_t now_ns, int is_dead ) {
  trace_event_t *      batch       = td_ring_batch;
  trace_ring_t const * ring        = thread->ring;
  uint64_t             event_cnt   = thread->mask + 1UL;
  uint64_t             margin_cnt  = is_dead ? 0UL : event_cnt / TD_DRAIN_MARGIN_DIV;
  uint64_t             safe_cnt    = event_cnt - margin_cnt;
  uint64_t             target_cnt  = __atomic_load_n( &ring->cursor, __ATOMIC_ACQUIRE );
  uint64_t             backlog_cnt = target_cnt - thread->seen_cnt;

  if( thread->is_cut ) return;  /* nothing to read but the zeroes that took the ring's place */
  if( target_cnt < thread->seen_cnt ) return;  /* a cursor that went back: no backlog either */
  /* What a ring held before it was found is history to read, not a reader running late.
     Beyond a full ring the rest is lost, and counted there. */
  if( backlog_cnt > event_cnt ) backlog_cnt = event_cnt;
  if( !thread->is_new && backlog_cnt > thread->backlog_max_cnt )
    thread->backlog_max_cnt = backlog_cnt;
  thread->is_new = 0;
  while( thread->seen_cnt < target_cnt ) {
    uint64_t cursor    = __atomic_load_n( &ring->cursor, __ATOMIC_ACQUIRE );
    uint64_t copy_cnt  = target_cnt - thread->seen_cnt;
    uint64_t first_idx = thread->seen_cnt & thread->mask, head_cnt = event_cnt - first_idx;
    int      has_sound = 0;
    if( thread->is_cut ) return;  /* the file was cut short: the cursor just read is a zero */
    if( cursor < thread->seen_cnt ) return;  /* a cursor that went back: nothing to count */
    if( cursor - thread->seen_cnt > safe_cnt ) {
      td_ring_lose( thread, cursor - thread->seen_cnt - safe_cnt );
      continue;
    }
    if( copy_cnt > TD_DRAIN_BATCH_EVENTS ) copy_cnt = TD_DRAIN_BATCH_EVENTS;
    /* Two runs: up to the ring's end, then from its start. No more than a ring is copied,
       whatever the cursor said (it is not trusted), so that the second run stays within it. */
    if( copy_cnt > event_cnt ) copy_cnt = event_cnt;
    if( head_cnt > copy_cnt ) head_cnt = copy_cnt;
    memcpy( batch, thread->event + first_idx, head_cnt * sizeof( trace_event_t ) );
    memcpy( batch + head_cnt, thread->event, ( copy_cnt - head_cnt ) * sizeof( trace_event_t ) );
    /* The copy's loads must be done before the cursor is read again: on aarch64 that takes
       a barrier (none is emitted on x86). */
    __atomic_thread_fence( __ATOMIC_ACQUIRE );
    cursor = __atomic_load_n( &ring->cursor, __ATOMIC_ACQUIRE );
    if( thread->is_cut ) return;
    if( !is_dead && cursor - thread->seen_cnt + TD_DRAIN_SLACK_EVENTS > event_cnt ) {
      td_ring_lose( thread, copy_cnt );
      continue;
    }
    td_ring_batch_wall_ms = td_wall_ms();
    td_ring_batch_tsc     = trace_tsc();
    for( uint64_t i = 0UL; i < copy_cnt; i++ ) has_sound |= td_ring_event( thread, &batch[ i ] );
    if( has_sound ) thread->last_event_ns = now_ns;
    thread->seen_cnt += copy_cnt;
  }
  /* A trace ends at the next root. An idle thread has no next root: close on silence
     (counter samples and gauges are not a sound), unless a span is still open, which is a
     slow trace and not an idle one. */
  if( thread->in_trace && now_ns - thread->last_event_ns > TD_IDLE_CLOSE_NS &&
      !td_ring_has_open_span( thread ) ) td_ring_trace_close( thread );
}

void td_ring_drain( uint64_t now_ns ) {
  for( int i = td_ring_next_live( 0 ); i < TD_THREAD_MAX; i = td_ring_next_live( i + 1 ) )
    if( !td_thread[ i ].is_gone ) td_ring_drain_one( &td_thread[ i ], now_ns, 0 );
}

void td_ring_finish( td_thread_t * thread, int is_dead ) {
  td_ring_drain_one( thread, td_clock_ns(), is_dead );
  td_ring_trace_close( thread );
}

/* ---- slots ----------------------------------------------------------------------------- */

/* A histogram into one cell of history. `second`, when given, is where a slot's samples go
   on: a sample is added to its slot's histogram only, and the second's is the sum of its
   slots'. The same counts as adding each sample twice, for half the work per event. */
static void td_ring_stat( td_hist_t * hist, td_stat_t * cell, td_hist_t * second ) {
  cell->p50_ns = td_ring_clamp( td_hist_percentile( hist, 0.50 ) );
  cell->p99_ns = td_ring_clamp( td_hist_percentile( hist, 0.99 ) );
  cell->max_ns = td_ring_clamp( hist->max_ns );
  cell->cnt    = td_ring_clamp( hist->cnt );
  if( !hist->cnt ) return;
  if( second ) {
    for( int i = 0; i < TD_HIST_BUCKET_CNT; i++ ) second->bucket[ i ] += hist->bucket[ i ];
    second->cnt += hist->cnt;
    if( hist->max_ns > second->max_ns ) second->max_ns = hist->max_ns;
  }
  td_hist_reset( hist );
}

/* Turn a thread's working histograms into one row of history: every site, then the trace. */
static void td_ring_close( td_thread_t const * thread, td_hist_t * hist, td_stat_t * row,
                           td_hist_t * second ) {
  for( int site = 0; site < thread->site_cnt; site++ )
    td_ring_stat( &hist[ site ], &row[ site ], second ? &second[ site ] : NULL );
  td_ring_stat( &hist[ TD_SITE_MAX ], &row[ TD_SITE_MAX ], second ? &second[ TD_SITE_MAX ] : NULL );
}

void td_ring_close_slot( uint64_t slot ) {
  for( int i = td_ring_next_live( 0 ); i < TD_THREAD_MAX; i = td_ring_next_live( i + 1 ) ) {
    td_thread_t * thread = &td_thread[ i ];
    td_ring_close( thread, thread->slot_hist, thread->fast[ slot % TD_FAST_CNT ],
                   thread->second_hist );
  }
}

void td_ring_close_second( uint64_t second ) {
  for( int i = td_ring_next_live( 0 ); i < TD_THREAD_MAX; i = td_ring_next_live( i + 1 ) ) {
    td_thread_t * thread = &td_thread[ i ];
    td_ring_close( thread, thread->second_hist, thread->slow[ second % TD_SLOW_CNT ], NULL );
    thread->slowest[ second % TD_SLOW_CNT ] = thread->slowest_open;
    memset( &thread->slowest_open, 0, sizeof( thread->slowest_open ) );
  }
}
