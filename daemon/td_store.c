/* td_store.c: runs the sources and keeps what they emit. Finds or creates the instance a
   value belongs to, turns running counts into rates, and holds one minute of slots and ten
   minutes of seconds per instance. A read that overruns its budget is slowed down. */
#include <math.h>

#include "td_source.h"

#define TD_STORE_BUDGET_NS        (TD_NS_PER_MS)      /* a source's mean read, all scopes... */
#define TD_STORE_BUDGET_DIV       (100UL)             /* ...or this share of its period, if more */
#define TD_STORE_WORST_DIV        (1000UL)            /* its costliest read: a tick or this share */
#define TD_STORE_SETTLE_NS        (TD_NS_PER_SECOND)  /* a thread's totals start this late */
#define TD_STORE_PERIOD_MAX_TICKS (1000)              /* a slowed source still reads every 10 s */
#define TD_STORE_PACE_READS       (10)                /* reads between two looks at a period */
#define TD_STORE_STALE_PERIODS    (3)                 /* not emitted this long: dropped */

struct td_out {
  int      source_idx;
  int      last_idx;      /* the instance of the emit before; -1: the read's first */
  int      thread_idx;
  int      resolution_ms;
  int      own_ms;        /* the resolution the source stated with td_resolution; 0: none */
  int      cpu;           /* whose counts are being emitted; -1: not a CPU's */
  uint64_t now_ns;
  int      is_changed;
};

char              td_scratch[ TD_SCRATCH_BYTES ];
td_source_state_t td_source_state[ TD_SOURCE_MAX ];
int               td_source_cnt;
td_instance_t     td_instance[ TD_INSTANCE_MAX ];
int               td_instance_end;
td_unavailable_t  td_unavailable_list[ TD_UNAVAILABLE_MAX ];
int               td_unavailable_cnt;

/* Whether the source's probe passed, per scope object; the last index is the host. */
static uint8_t td_store_usable[ TD_SOURCE_MAX ][ TD_THREAD_MAX + 1 ];
static int     td_store_first_idx[ TD_SOURCE_MAX ];  /* the instance each source emitted first */

static uint64_t         td_store_found_ns[ TD_THREAD_MAX ];   /* of the thread each slot held */
static td_unavailable_t td_store_before[ TD_UNAVAILABLE_MAX ]; /* the list at the last probe */
static int              td_store_full_source = -1;  /* a source whose values found no room */

static int td_store_base_ticks( td_source_t const * source ) {
  int period_ticks = source->period_ms / TD_TICK_MS;
  return period_ticks > 0 ? period_ticks : 1;
}

void td_store_init( td_source_t const * const * source, int source_cnt ) {
  td_source_cnt = source_cnt < TD_SOURCE_MAX ? source_cnt : TD_SOURCE_MAX;
  if( source_cnt > TD_SOURCE_MAX )
    fprintf( stderr, "tracerd: %d sources, the first %d are run\n", source_cnt, TD_SOURCE_MAX );
  for( int i = 0; i < td_source_cnt; i++ ) {
    td_source_state_t * state = &td_source_state[ i ];
    memset( state, 0, sizeof( *state ) );
    state->source       = source[ i ];
    state->is_on        = !source[ i ]->is_unproven;
    state->period_ticks = td_store_base_ticks( source[ i ] );
    if( source[ i ]->metric_cnt > TD_METRIC_MAX )
      fprintf( stderr, "tracerd: %s has %d metrics, thresholds are kept for the first %d\n",
               source[ i ]->name, source[ i ]->metric_cnt, TD_METRIC_MAX );
    for( int metric_idx = 0; metric_idx < TD_METRIC_MAX; metric_idx++ ) {
      if( metric_idx >= source[ i ]->metric_cnt ) break;
      state->amber[ metric_idx ] = source[ i ]->metric[ metric_idx ].amber;
      state->red  [ metric_idx ] = source[ i ]->metric[ metric_idx ].red;
    }
  }
}

static int td_store_source_find( char const * name, size_t name_bytes ) {
  for( int i = 0; i < td_source_cnt; i++ ) {
    char const * candidate = td_source_state[ i ].source->name;
    if( strlen( candidate ) == name_bytes && !strncmp( candidate, name, name_bytes ) ) return i;
  }
  return -1;
}

int td_store_select( char const * list, int is_on ) {
  uint8_t is_named[ TD_SOURCE_MAX ] = { 0 };
  if( !*list ) return -ENOENT;
  while( *list ) {  /* every name is checked before anything changes */
    size_t name_bytes = strcspn( list, "," );
    int    source_idx = td_store_source_find( list, name_bytes );
    if( source_idx < 0 ) return -ENOENT;
    is_named[ source_idx ] = 1;
    list += name_bytes;
    if( *list == ',' && !*++list ) return -ENOENT;  /* a comma with nothing after it */
  }
  for( int i = 0; i < td_source_cnt; i++ ) {
    if( is_on < 0 )          td_source_state[ i ].is_on = is_named[ i ];
    else if( is_named[ i ] ) td_source_state[ i ].is_on = is_on;
  }
  return 0;
}

/* Is the text from `start` up to `end` exactly `word`? */
static int td_store_is_word( char const * start, char const * end, char const * word ) {
  size_t word_bytes = strlen( word );
  return (size_t)( end - start ) == word_bytes && !strncmp( start, word, word_bytes );
}

int td_store_set( char const * assignment ) {
  char const *        metric_dot = strchr( assignment, '.' );
  char const *        level_dot  = metric_dot ? strchr( metric_dot + 1, '.' ) : NULL;
  char const *        equals     = level_dot  ? strchr( level_dot + 1, '=' )  : NULL;
  td_source_state_t * state;
  double              level, * threshold;
  int                 source_idx;

  if( !equals ) return -EINVAL;
  source_idx = td_store_source_find( assignment, (size_t)( metric_dot - assignment ) );
  if( source_idx < 0 ) return -EINVAL;
  state = &td_source_state[ source_idx ];
  for( int metric_idx = 0; metric_idx < TD_METRIC_MAX; metric_idx++ ) {
    char const * id;
    size_t       id_bytes = (size_t)( level_dot - metric_dot - 1 );
    if( metric_idx >= state->source->metric_cnt ) break;
    id = state->source->metric[ metric_idx ].id;
    if( strlen( id ) != id_bytes || strncmp( id, metric_dot + 1, id_bytes ) ) continue;
    if( td_parse_double( equals + 1, &level ) || level < 0.0 ) return -EINVAL;
    if(      td_store_is_word( level_dot + 1, equals, "amber" ) ) threshold = state->amber;
    else if( td_store_is_word( level_dot + 1, equals, "red"   ) ) threshold = state->red;
    else return -EINVAL;
    threshold[ metric_idx ] = level;
    return 0;
  }
  return -EINVAL;
}

/* ---- what sources call ----------------------------------------------------------------- */

static td_instance_t * td_store_instance( td_out_t * out, int metric_idx, char const * key ) {
  td_instance_t * free_slot = NULL;
  /* A source emits in the same order read after read: the scan starts where the emit that
     followed the last one was found a read ago, and most often ends there. */
  int *           next      = out->last_idx < 0 ? &td_store_first_idx[ out->source_idx ]
                                                : &td_instance[ out->last_idx ].next_idx;
  int             end       = td_instance_end;  /* no slot at or past it is in use */
  int             first     = *next < end ? *next : 0;
  for( int n = 0; n < end; n++ ) {
    int             i        = first + n < end ? first + n : first + n - end;
    td_instance_t * instance = &td_instance[ i ];
    if( !instance->is_live ) {
      if( !free_slot || instance < free_slot ) free_slot = instance;
      continue;
    }
    if( instance->source_idx == out->source_idx && instance->metric_idx == metric_idx &&
        instance->thread_idx == out->thread_idx &&
        !strncmp( instance->key, key, TD_KEY_BYTES - 1UL ) ) {  /* as it was cut */
      *next = out->last_idx = i;
      return instance;
    }
  }
  if( !free_slot && end < TD_INSTANCE_MAX ) free_slot = &td_instance[ end ];
  if( !free_slot ) {
    TD_LOG_ONCE( "tracerd: more than %d metric instances, the rest dropped\n", TD_INSTANCE_MAX );
    td_store_full_source = out->source_idx;  /* said on the page at the next probe */
    return NULL;
  }
  memset( free_slot, 0, sizeof( *free_slot ) );
  free_slot->is_live    = 1;
  free_slot->source_idx = out->source_idx;
  free_slot->metric_idx = metric_idx;
  free_slot->thread_idx = out->thread_idx;
  snprintf( free_slot->key, sizeof( free_slot->key ), "%s", key );
  for( int i = 0; i < TD_FAST_CNT; i++ ) free_slot->fast[ i ] = NAN;
  for( int i = 0; i < TD_SLOW_CNT; i++ ) free_slot->slow[ i ] = NAN;
  free_slot->value = free_slot->second_max = NAN;
  *next = out->last_idx = (int)( free_slot - td_instance );
  if( out->last_idx >= td_instance_end ) td_instance_end = out->last_idx + 1;
  out->is_changed = 1;
  return free_slot;
}

void td_emit( td_out_t * out, int metric_idx, char const * key, double value ) {
  td_source_t const * source = td_source_state[ out->source_idx ].source;
  td_instance_t *     instance;
  double              shown = value;
  int                 is_other_cpu;  /* the count of another CPU than last time: no sequel */
  if( metric_idx < 0 || metric_idx >= source->metric_cnt || isinf( value ) ) return;
  instance = td_store_instance( out, metric_idx, key );
  if( !instance ) return;
  if( instance->resolution_ms != out->resolution_ms ) out->is_changed = 1;  /* the page prints it */
  instance->resolution_ms = out->resolution_ms;
  instance->own_ms        = out->own_ms;
  instance->seen_ns       = out->now_ns;
  if( isnan( value ) ) {  /* there, with no value just now: what it counted so far is kept */
    instance->value = NAN;
    return;
  }

  is_other_cpu = instance->has_raw && instance->cpu != out->cpu;
  if( source->metric[ metric_idx ].kind == TD_RATE ) {
    double seconds  = (double)( out->now_ns - instance->raw_ns ) / (double)TD_NS_PER_SECOND;
    int    has_rate = instance->has_raw && !is_other_cpu && seconds > 0.0 &&
                      value >= instance->raw_last;
    shown = has_rate ? ( value - instance->raw_last ) / seconds : NAN;
  } else if( source->metric[ metric_idx ].kind == TD_TOTAL ) {
    /* Attaching costs a thread faults and switches of its own making: not counted. */
    int is_settling = out->thread_idx >= 0 &&
                      out->now_ns - td_thread[ out->thread_idx ].found_ns < TD_STORE_SETTLE_NS;
    if( !instance->has_raw || is_settling ) instance->raw_first = value;
    else if( is_other_cpu || value < instance->raw_last )  /* it starts again: keep the total */
      instance->raw_first -= instance->raw_last - value;
    shown = value - instance->raw_first;
  }
  instance->cpu      = out->cpu;
  instance->raw_last = value;
  instance->raw_ns   = out->now_ns;
  instance->has_raw  = 1;
  instance->value    = (float)shown;
  if( !isnan( shown ) && !( instance->second_max >= shown ) ) instance->second_max = (float)shown;
}

void td_resolution( td_out_t * out, int resolution_ms ) {
  int period_ms = td_source_state[ out->source_idx ].period_ticks * TD_TICK_MS;
  out->own_ms        = resolution_ms;
  out->resolution_ms = resolution_ms > period_ms ? resolution_ms : period_ms;
}

void td_of_cpu( td_out_t * out, int cpu ) { out->cpu = cpu; }

void td_unavailable( td_out_t * out, int metric_idx, char const * why ) {
  td_unavailable_t * entry;
  if( td_unavailable_cnt >= TD_UNAVAILABLE_MAX ) return;
  entry             = &td_unavailable_list[ td_unavailable_cnt++ ];
  entry->source_idx = out->source_idx;
  entry->metric_idx = metric_idx;
  entry->thread_idx = out->thread_idx;
  snprintf( entry->why, sizeof( entry->why ), "%s", why );
}

/* ---- scopes ---------------------------------------------------------------------------- */

/* Is thread_idx the first live thread of its scope object? A process-scope source reads a
   process once, a core-scope source a CPU once, however many traced threads share it. */
static int td_store_is_first( int scope, int thread_idx ) {
  td_thread_t const * thread = &td_thread[ thread_idx ];
  for( int i = 0; i < thread_idx; i++ ) {
    td_thread_t const * other = &td_thread[ i ];
    if( !other->is_live || other->is_gone ) continue;
    if( scope == TD_SCOPE_PROCESS && other->pid       == thread->pid       ) return 0;
    if( scope == TD_SCOPE_CORE    && other->cpu       == thread->cpu       ) return 0;
  }
  return 1;
}

/* Probe (is_probe) or read the source once per object of its scope. */
static void td_store_each_scope( int source_idx, uint64_t now_ns, int is_probe, td_out_t * out ) {
  td_source_t const * source    = td_source_state[ source_idx ].source;
  td_ctx_t            ctx       = { .thread_idx = -1, .cpu = -1, .now_ns = now_ns };
  int                 period_ms = td_source_state[ source_idx ].period_ticks * TD_TICK_MS;
  int                 is_host   = source->scope == TD_SCOPE_HOST;
  int                 first     = is_host ? TD_THREAD_MAX : 0;
  int                 last      = is_host ? TD_THREAD_MAX : TD_THREAD_MAX - 1;

  out->source_idx = source_idx;
  out->last_idx   = -1;
  out->now_ns     = now_ns;
  ctx.period_ms   = period_ms;
  for( int usable_idx = first; usable_idx <= last; usable_idx++ ) {  /* TD_THREAD_MAX: the host */
    uint8_t * is_usable = &td_store_usable[ source_idx ][ usable_idx ];
    out->thread_idx    = -1;
    out->cpu           = -1;
    out->resolution_ms = period_ms;
    out->own_ms        = 0;
    if( usable_idx < TD_THREAD_MAX ) {
      td_thread_t const * thread = &td_thread[ usable_idx ];
      if( !thread->is_live || thread->is_gone ) continue;
      if( !td_store_is_first( source->scope, usable_idx ) ) continue;
      ctx.thread_idx  = usable_idx;
      ctx.pid         = thread->pid;
      ctx.tid         = thread->tid;
      ctx.cpu         = thread->cpu;
      out->thread_idx = usable_idx;
      if( source->scope == TD_SCOPE_CORE ) out->cpu = thread->cpu;  /* all it emits is the CPU's */
    }
    if( is_probe )        *is_usable = !source->probe || !source->probe( &ctx, out );
    else if( *is_usable ) source->read( &ctx, out );
  }
}

/* A slot of td_thread that holds another thread than before starts afresh: the instances
   of the thread that was there are not this one's history. And a thread that is gone, or
   no longer the first of its process or core, gives up what it held for them. */
static void td_store_forget_reused( void ) {
  uint64_t * found_ns = td_store_found_ns;
  for( int thread_idx = 0; thread_idx < TD_THREAD_MAX; thread_idx++ ) {
    td_thread_t const * thread   = &td_thread[ thread_idx ];
    int                 is_other = thread->is_live && thread->found_ns != found_ns[ thread_idx ];
    if( is_other ) found_ns[ thread_idx ] = thread->found_ns;
    if( !thread->is_live ) continue;
    for( int i = 0; i < td_instance_end; i++ ) {
      td_instance_t * instance = &td_instance[ i ];
      int             scope;
      if( !instance->is_live || instance->thread_idx != thread_idx ) continue;
      scope = td_source_state[ instance->source_idx ].source->scope;
      /* What a thread held for its process or core is read under another thread once it
         is gone or no longer the first of them: left to go stale, it would show twice. */
      if( is_other ) instance->is_live = 0;
      else if( scope != TD_SCOPE_PROCESS && scope != TD_SCOPE_CORE ) continue;
      else if( thread->is_gone || !td_store_is_first( scope, thread_idx ) ) instance->is_live = 0;
    }
  }
}

int td_store_probe( uint64_t now_ns ) {
  td_unavailable_t * before     = td_store_before;
  td_out_t           out        = { 0 };
  int                before_cnt = td_unavailable_cnt;

  memcpy( before, td_unavailable_list, sizeof( td_store_before ) );
  memset( td_unavailable_list, 0, sizeof( td_unavailable_list ) );
  memset( td_store_usable, 0, sizeof( td_store_usable ) );
  td_unavailable_cnt = 0;
  td_store_forget_reused();
  for( int i = 0; i < td_source_cnt; i++ )
    if( td_source_state[ i ].is_on ) td_store_each_scope( i, now_ns, 1, &out );
  if( td_store_full_source >= 0 ) {
    out.source_idx = td_store_full_source;
    out.thread_idx = -1;
    td_unavailable( &out, -1, "more values than tracerd keeps: some are not shown" );
    td_store_full_source = -1;  /* said again only if it happens again */
  }
  if( before_cnt != td_unavailable_cnt ) return 1;
  return !!memcmp( before, td_unavailable_list, sizeof( td_store_before ) );
}

/* Would reads of this cost be over budget at this period? Two budgets, each a fixed time
   or a fraction of the period when that is more, so that a slow source settles at a period
   it can afford. One for the mean of the reads, what the source costs the daemon: 1 ms, or
   1 % of the period. The two meet at the 100 ms most sources read at, so a source may
   cost up to about 1 % of a CPU at any period, and one that costs a little more than a
   millisecond reads half as often, not sixteen times less. One for the costliest read: a
   whole tick, or a thousandth of the period: a read that long puts off a drain. */
static int td_store_is_over( td_source_state_t const * state, int period_ticks ) {
  uint64_t period_ns = (uint64_t)period_ticks * TD_TICK_MS * TD_NS_PER_MS;
  uint64_t mean_ns   = state->pace_sum_ns / TD_STORE_PACE_READS;
  uint64_t tick_ns   = (uint64_t)TD_TICK_MS * TD_NS_PER_MS;
  return ( mean_ns > TD_STORE_BUDGET_NS && mean_ns > period_ns / TD_STORE_BUDGET_DIV ) ||
         ( state->pace_worst_ns > tick_ns &&
           state->pace_worst_ns > period_ns / TD_STORE_WORST_DIV );
}

/* Reconsider a source's period every few reads. Over budget, the period doubles; a slowed
   source comes back a step toward its own period once it could afford that step. Returns
   1 when the period changed: the page prints it. */
static int td_store_pace( td_source_state_t * state, uint64_t cost_ns ) {
  int base_ticks = td_store_base_ticks( state->source ), old_ticks = state->period_ticks;
  state->pace_sum_ns += cost_ns;
  if( cost_ns > state->pace_worst_ns ) state->pace_worst_ns = cost_ns;
  if( ++state->pace_read_cnt < TD_STORE_PACE_READS ) return 0;
  if( td_store_is_over( state, state->period_ticks ) ) state->period_ticks *= 2;
  else if( !td_store_is_over( state, state->period_ticks / 2 ) ) state->period_ticks /= 2;
  if( state->period_ticks > TD_STORE_PERIOD_MAX_TICKS )
    state->period_ticks = TD_STORE_PERIOD_MAX_TICKS;
  if( state->period_ticks < base_ticks ) state->period_ticks = base_ticks;
  state->cost_us       = (uint32_t)( state->pace_worst_ns / TD_NS_PER_US );
  state->pace_read_cnt = 0;
  state->pace_sum_ns   = state->pace_worst_ns = 0UL;
  return state->period_ticks != old_ticks;
}

static void td_store_forgive( int source_idx, uint64_t late_ns ) {
  for( int i = 0; i < td_instance_end; i++ )
    if( td_instance[ i ].is_live && td_instance[ i ].source_idx == source_idx )
      td_instance[ i ].seen_ns += late_ns;
}

/* The source's period changed. Each instance's resolution follows, down to what its source
   stated for it, at once: the hello that tells the page is built before the next emit. */
static void td_store_repace( int source_idx, int period_ms ) {
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t * instance = &td_instance[ i ];
    if( !instance->is_live || instance->source_idx != source_idx ) continue;
    instance->resolution_ms = instance->own_ms > period_ms ? instance->own_ms : period_ms;
  }
}

int td_store_tick( uint64_t tick, uint64_t now_ns ) {
  td_out_t out = { 0 };
  for( int i = 0; i < td_source_cnt; i++ ) {
    td_source_state_t * state = &td_source_state[ i ];
    uint64_t            start_ns, cost_ns, period_ns;
    uint32_t            cost_us;
    if( !state->is_on || tick < state->due_tick ) continue;
    /* A read that comes late (the daemon was stopped) is no silence of the instances: the
       time they were last emitted moves on by the lateness, so that none goes stale for it. */
    if( state->due_tick && tick > state->due_tick )
      td_store_forgive( i, ( tick - state->due_tick ) * TD_TICK_MS * TD_NS_PER_MS );
    state->due_tick = tick + (uint64_t)state->period_ticks;
    start_ns = td_clock_ns();
    /* Values are dated by the tick, not by the read: a tick's reads are well under a
       millisecond apart, the shortest period is a hundred. */
    td_store_each_scope( i, now_ns, 0, &out );
    cost_ns        = td_clock_ns() - start_ns;
    /* The card shows the costliest read of the pacing window before, or of this one. */
    cost_us = (uint32_t)( cost_ns / TD_NS_PER_US );
    if( cost_us > state->cost_us ) state->cost_us = cost_us;
    if( td_store_pace( state, cost_ns ) ) {
      td_store_repace( i, state->period_ticks * TD_TICK_MS );
      out.is_changed = 1;
    }

    /* Stale after a few periods of the source, or of the instance when its values come
       less often than the source reads: a counter sampled every few seconds must not flap. */
    period_ns = (uint64_t)state->period_ticks * TD_TICK_MS * TD_NS_PER_MS;
    for( int instance_idx = 0; instance_idx < td_instance_end; instance_idx++ ) {
      td_instance_t * instance = &td_instance[ instance_idx ];
      uint64_t        own_ns;
      if( !instance->is_live || instance->source_idx != i ) continue;
      own_ns = (uint64_t)( instance->resolution_ms > 0 ? instance->resolution_ms : 0 ) *
               TD_NS_PER_MS;
      if( own_ns < period_ns ) own_ns = period_ns;
      if( now_ns - instance->seen_ns <= own_ns * TD_STORE_STALE_PERIODS ) continue;
      instance->is_live = 0;
      out.is_changed    = 1;
    }
  }
  return out.is_changed;
}

void td_store_close_slot( uint64_t slot, int is_known ) {
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t * instance = &td_instance[ i ];
    if( !instance->is_live ) continue;
    instance->fast[ slot % TD_FAST_CNT ] = is_known ? instance->value : NAN;
  }
}

void td_store_close_second( uint64_t second, int is_known ) {
  for( int i = 0; i < td_instance_end; i++ ) {
    td_instance_t * instance = &td_instance[ i ];
    if( !instance->is_live ) continue;
    /* A second in which the source did not read holds the value that still stands. */
    instance->slow[ second % TD_SLOW_CNT ] = !is_known ? NAN : isnan( instance->second_max )
                                             ? instance->value : instance->second_max;
    instance->second_max                   = NAN;
  }
  while( td_instance_end > 0 && !td_instance[ td_instance_end - 1 ].is_live ) td_instance_end--;
}
