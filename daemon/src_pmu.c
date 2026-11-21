/* src_pmu.c: the hardware counters, as the traced thread sampled them into its ring. Turns
   counts into counts per trace (per millisecond for a thread without traces), and the two
   stall counters into the price of a miss. The reading itself is in src/trace_pmu.h. */
#include <math.h>

#include "td_source.h"

enum {
  SRC_PMU_DTLB, SRC_PMU_LLC, SRC_PMU_MEM_STALL, SRC_PMU_FRONT_STALL, SRC_PMU_L2, SRC_PMU_BACK_STALL,
  SRC_PMU_L1D, SRC_PMU_BRANCH, SRC_PMU_REMOTE, SRC_PMU_MISS_COST,
  SRC_PMU_DTLB_MS, SRC_PMU_LLC_MS, SRC_PMU_L2_MS, SRC_PMU_L1D_MS, SRC_PMU_BRANCH_MS,
  SRC_PMU_REMOTE_MS, SRC_PMU_METRIC_CNT
};

#define SRC_PMU_US_PER_MS     (1000U)
#define SRC_PMU_PERIOD_MAX_US (1000000U)  /* the sampler's longest period: the ring may say more */
#define SRC_PMU_IDLE_NOTE     "; has not counted since the thread attached"

/* One row per metric, in the order of the enum above. The ids of the counted ones are the
   names of src/trace_events.h. Misses are shown per trace: a spin loop is taken to make
   none (not measured), so they belong to the traces. Stalls (cycles; pipeline slots for x86
   front and back) accrue while spinning too, so they are shown per millisecond of the thread,
   spin included. The last six are the misses of a thread that has no root site, hence no
   trace to divide by: per millisecond as well. */
static td_metric_t const src_pmu_metric[ SRC_PMU_METRIC_CNT ] = {
  { "dtlb_walk",      "dTLB walks",        "/trace", TD_LANE, TD_LEVEL, 0, 0 },
  { "llc_miss",       "last-level misses", "/trace", TD_LANE, TD_LEVEL, 0, 0 },
  { "mem_stall",      "memory stall",      "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "front_stall",    "frontend stall",    "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "l2_miss",        "L2 misses",         "/trace", TD_LANE, TD_LEVEL, 0, 0 },
  { "back_stall",     "backend stall",     "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "l1d_miss",       "L1D misses",        "/trace", TD_LANE, TD_LEVEL, 0, 0 },
  { "branch_miss",    "mispredicts",       "/trace", TD_LANE, TD_LEVEL, 0, 0 },
  { "remote_load",    "remote loads",      "/trace", TD_LANE, TD_LEVEL, 0, 1 },
  { "miss_cost",      "stall per miss",    "",       TD_LANE, TD_LEVEL, 0, 0 },
  { "dtlb_walk_ms",   "dTLB walks",        "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "llc_miss_ms",    "last-level misses", "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "l2_miss_ms",     "L2 misses",         "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "l1d_miss_ms",    "L1D misses",        "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "branch_miss_ms", "mispredicts",       "/ms",    TD_LANE, TD_LEVEL, 0, 0 },
  { "remote_load_ms", "remote loads",      "/ms",    TD_LANE, TD_LEVEL, 0, 1 },
};

/* The per-millisecond row of each counted metric: its own for a stall, the twin of a miss. */
static int const src_pmu_per_ms[ SRC_PMU_MISS_COST ] = {
  [ SRC_PMU_DTLB ]      = SRC_PMU_DTLB_MS,   [ SRC_PMU_LLC ]         = SRC_PMU_LLC_MS,
  [ SRC_PMU_MEM_STALL ] = SRC_PMU_MEM_STALL, [ SRC_PMU_FRONT_STALL ] = SRC_PMU_FRONT_STALL,
  [ SRC_PMU_L2 ]        = SRC_PMU_L2_MS,     [ SRC_PMU_BACK_STALL ]  = SRC_PMU_BACK_STALL,
  [ SRC_PMU_L1D ]       = SRC_PMU_L1D_MS,    [ SRC_PMU_BRANCH ]      = SRC_PMU_BRANCH_MS,
  [ SRC_PMU_REMOTE ]    = SRC_PMU_REMOTE_MS,
};

/* What is known for "stall per miss" of one thread: the last rate of each of its two
   counters and when it was taken, for they are sampled in turn and seldom in one read; and
   whether the miss rate spans a wait for a trace, which makes it no match for a stall rate. */
typedef struct {
  uint64_t found_ns;  /* of the thread these are of: a slot can come to hold another */
  double   stall_rate, miss_rate;
  uint64_t stall_ns, miss_ns;
  int      is_gap;
} src_pmu_cost_t;

static src_pmu_cost_t src_pmu_cost[ TD_THREAD_MAX ];

/* The metric a counter of the ring feeds, by name; -1 for a name this tracerd lacks. */
static int src_pmu_metric_of( char const * counter_name ) {
  for( int i = 0; i < SRC_PMU_MISS_COST; i++ )
    if( !strncmp( src_pmu_metric[ i ].id, counter_name, TRACE_NAME_BYTES ) ) return i;
  return -1;
}

/* Does the thread have a root site? Its misses are then per trace; else per millisecond. */
static int src_pmu_has_root( td_thread_t const * thread ) {
  for( int site = 0; site < thread->site_cnt; site++ )
    if( thread->site[ site ].flags & TRACE_ROOT ) return 1;
  return 0;
}

/* Usable when the thread has counters. Then says what they cost it, and for each metric
   which event was taken, or that the host gave none: 8 events and 7 lanes is a riddle. */
static int src_pmu_probe( td_ctx_t const * ctx, td_out_t * out ) {
  td_thread_t const *  thread      = &td_thread[ ctx->thread_idx ];
  trace_ring_t const * ring        = thread->ring;
  uint32_t             counter_cnt = __atomic_load_n( &ring->counter_cnt, __ATOMIC_ACQUIRE );
  uint32_t             period_us   = ring->counter_period_us;
  uint8_t              has_event[ SRC_PMU_MISS_COST ] = { 0 };
  char                 why[ TD_WHY_BYTES ];
  if( !counter_cnt ) {
    int status_bytes = (int)sizeof( ring->counter_status );
    snprintf( why, sizeof( why ), "%.*s", status_bytes,
              ring->counter_status[ 0 ] ? ring->counter_status : "no counters" );
    td_unavailable( out, -1, why );
    return -ENOENT;
  }
  if( counter_cnt > TRACE_COUNTER_MAX ) counter_cnt = TRACE_COUNTER_MAX;  /* not trusted */
  if( period_us > SRC_PMU_PERIOD_MAX_US ) period_us = SRC_PMU_PERIOD_MAX_US;
  snprintf( why, sizeof( why ), "%u events; a read costs the thread %u ns, one is read every %u ms",
            counter_cnt, ring->counter_read_ns, period_us / SRC_PMU_US_PER_MS );
  td_unavailable( out, -1, why );
  for( uint32_t i = 0U; i < counter_cnt && i < TRACE_COUNTER_MAX; i++ ) {
    int metric_idx = src_pmu_metric_of( ring->counter[ i ].name );
    if( metric_idx < 0 ) continue;
    has_event[ metric_idx ] = 1;
    snprintf( why, sizeof( why ), "event: %.*s%s", (int)sizeof( ring->counter[ i ].chosen ),
              ring->counter[ i ].chosen,
              thread->counter[ i ].has_counted ? "" : SRC_PMU_IDLE_NOTE );
    td_unavailable( out, metric_idx, why );
  }
  for( int metric_idx = 0; metric_idx < SRC_PMU_MISS_COST; metric_idx++ )
    if( !has_event[ metric_idx ] )
      td_unavailable( out, metric_idx, "no usable event here: tracerd --probe --pmu says why" );
  if( !has_event[ SRC_PMU_MEM_STALL ] || !has_event[ SRC_PMU_LLC ] )
    td_unavailable( out, SRC_PMU_MISS_COST, "needs both memory stall and last-level misses" );
  return 0;
}

/* Emit what one counter gathered since the last read, then start it over. A counter not
   sampled twice since then emits nothing: its lane holds. A miss count of a thread with a
   root, while no trace has completed, waits for one: its lane shows a gap meanwhile. */
static void src_pmu_emit( td_thread_t const * thread, td_counter_t * counter, int metric_idx,
                          src_pmu_cost_t * cost, uint64_t now_ns, td_out_t * out ) {
  double count      = (double)counter->delta_sum;
  int    per_ms_idx = src_pmu_per_ms[ metric_idx ];
  int    is_per_ms  = per_ms_idx == metric_idx || !src_pmu_has_root( thread );
  if( !counter->tsc_sum ) return;
  if( !counter->delta_sum && !counter->has_counted ) {  /* never seen counting: no lane, */
    counter->trace_sum = counter->tsc_sum = 0UL;        /* and no average since attach later */
    return;
  }
  counter->has_counted = 1;
  if( !is_per_ms && !counter->trace_sum ) {
    td_emit( out, metric_idx, "", NAN );
    counter->has_waited = 1;
    if( metric_idx == SRC_PMU_LLC ) { cost->is_gap = 1; cost->miss_ns = now_ns; }
    return;
  }
  if( is_per_ms ) td_emit( out, per_ms_idx, "", count / td_ring_tsc_to_ms( counter->tsc_sum ) );
  else            td_emit( out, metric_idx, "", count / (double)counter->trace_sum );
  if( metric_idx == SRC_PMU_MEM_STALL ) {
    cost->stall_rate = count / (double)counter->tsc_sum;
    cost->stall_ns   = now_ns;
  }
  if( metric_idx == SRC_PMU_LLC ) {
    cost->miss_rate = count / (double)counter->tsc_sum;
    cost->miss_ns   = now_ns;
    cost->is_gap    = counter->has_waited;
  }
  counter->delta_sum  = counter->trace_sum = counter->tsc_sum = 0UL;
  counter->has_waited = 0;
}

/* The price of a miss, from the last rate of each counter when one of them is new and the
   other no older than two rounds of the sampler. A gap when there was no miss to price,
   or the misses span a wait for a trace; with nothing new, nothing: the lane holds. */
static void src_pmu_emit_cost( src_pmu_cost_t const * cost, uint64_t now_ns, uint64_t round_ns,
                               td_out_t * out ) {
  int is_new   = cost->stall_ns == now_ns || cost->miss_ns == now_ns;
  int is_known = cost->stall_ns && cost->miss_ns && now_ns - cost->stall_ns <= 2UL * round_ns &&
                 now_ns - cost->miss_ns <= 2UL * round_ns;
  if( !is_new || !is_known ) return;
  if( cost->is_gap || cost->miss_rate <= 0.0 || cost->stall_rate <= 0.0 )
    td_emit( out, SRC_PMU_MISS_COST, "", NAN );
  else td_emit( out, SRC_PMU_MISS_COST, "", cost->stall_rate / cost->miss_rate );
}

static void src_pmu_read( td_ctx_t const * ctx, td_out_t * out ) {
  td_thread_t *    thread      = &td_thread[ ctx->thread_idx ];
  src_pmu_cost_t * cost        = &src_pmu_cost[ ctx->thread_idx ];
  uint32_t         counter_cnt = thread->ring->counter_cnt;
  uint32_t         period_us   = thread->ring->counter_period_us;
  int              round_ms;

  /* What the ring says is not trusted: it is bounded before it is used. */
  if( counter_cnt > TRACE_COUNTER_MAX )   counter_cnt = TRACE_COUNTER_MAX;
  if( period_us > SRC_PMU_PERIOD_MAX_US ) period_us   = SRC_PMU_PERIOD_MAX_US;
  if( cost->found_ns != thread->found_ns ) {  /* another thread holds the slot: start afresh */
    memset( cost, 0, sizeof( *cost ) );
    cost->found_ns = thread->found_ns;
  }
  /* A counter is read once every counter_cnt periods of the thread's sampler: a round. */
  round_ms = (int)( period_us / SRC_PMU_US_PER_MS * counter_cnt );
  if( round_ms < ctx->period_ms ) round_ms = ctx->period_ms;
  td_resolution( out, round_ms );
  for( uint32_t i = 0U; i < counter_cnt; i++ ) {
    int metric_idx = src_pmu_metric_of( thread->ring->counter[ i ].name );
    if( metric_idx < 0 ) continue;
    src_pmu_emit( thread, &thread->counter[ i ], metric_idx, cost, ctx->now_ns, out );
  }
  src_pmu_emit_cost( cost, ctx->now_ns, (uint64_t)round_ms * TD_NS_PER_MS, out );
}

td_source_t const td_src_pmu = {
  .name = "pmu", .scope = TD_SCOPE_THREAD, .period_ms = 500,
  .metric = src_pmu_metric, .metric_cnt = SRC_PMU_METRIC_CNT,
  .probe = src_pmu_probe, .read = src_pmu_read,
};
