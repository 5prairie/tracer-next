/* src_ring.c: two sources fed by the rings themselves. "ring" is the tracer's own queue:
   backlog, lost events, event and trace rates. "gauge" is whatever level the program
   publishes with trace_gauge, one instance per site that carried one. */
#include "td_source.h"

enum { SRC_RING_BACKLOG, SRC_RING_LOST, SRC_RING_EVENTS, SRC_RING_TRACES, SRC_RING_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_ring_metric[ SRC_RING_METRIC_CNT ] = {
  { "backlog", "ring backlog", "%",  TD_LANE,  TD_LEVEL, 25, 75 },
  { "lost",    "events lost",  "",   TD_CHECK, TD_TOTAL, 0,  1  },
  { "events",  "events",       "/s", TD_TABLE, TD_RATE,  0,  0  },
  { "traces",  "traces",       "/s", TD_TABLE, TD_RATE,  0,  0  },
};

/* Always usable. Says what a reader of the page should know about the rings themselves. */
static int src_ring_probe( td_ctx_t const * ctx, td_out_t * out ) {
  char why[ TD_WHY_BYTES ];
  if( !td_thread[ ctx->thread_idx ].is_locked )
    td_unavailable( out, -1, "the ring is not locked in memory (mlock failed): it can fault" );
  if( td_ring_unfollowed_cnt ) {
    snprintf( why, sizeof( why ), "more than %d traced threads: not followed, %d",
              TD_THREAD_MAX, td_ring_unfollowed_cnt );
    td_unavailable( out, -1, why );
  }
  return 0;
}

static void src_ring_read( td_ctx_t const * ctx, td_out_t * out ) {
  td_thread_t * thread    = &td_thread[ ctx->thread_idx ];
  double        event_cnt = (double)( thread->mask + 1UL );
  td_emit( out, SRC_RING_BACKLOG, "", 100.0 * (double)thread->backlog_max_cnt / event_cnt );
  td_emit( out, SRC_RING_LOST,    "", (double)thread->lost_cnt );
  td_emit( out, SRC_RING_EVENTS,  "", (double)thread->seen_cnt );
  td_emit( out, SRC_RING_TRACES,  "", (double)thread->trace_cnt );
  thread->backlog_max_cnt = 0UL;
}

td_source_t const td_src_ring = {
  .name = "ring", .scope = TD_SCOPE_THREAD, .period_ms = 100,
  .metric = src_ring_metric, .metric_cnt = SRC_RING_METRIC_CNT,
  .probe = src_ring_probe, .read = src_ring_read,
};

enum { SRC_GAUGE_LEVEL, SRC_GAUGE_METRIC_CNT };

static td_metric_t const src_gauge_metric[ SRC_GAUGE_METRIC_CNT ] = {
  { "level", "", "", TD_LANE, TD_LEVEL, 0, 0 },
};

/* The largest level since the last read, so that a short peak between two reads shows. */
static void src_gauge_read( td_ctx_t const * ctx, td_out_t * out ) {
  td_thread_t * thread = &td_thread[ ctx->thread_idx ];
  for( int site = 0; site < thread->site_cnt; site++ ) {
    trace_site_name_t const * named = &thread->site[ site ];
    char                      name[ TD_KEY_BYTES ];
    if( !thread->is_gauge[ site ] ) continue;
    snprintf( name, sizeof( name ), "%.*s", (int)sizeof( named->name ), named->name );
    td_emit( out, SRC_GAUGE_LEVEL, name, (double)thread->gauge_max[ site ] );
    thread->gauge_max[ site ] = thread->gauge_last[ site ];
  }
}

td_source_t const td_src_gauge = {
  .name = "gauge", .scope = TD_SCOPE_THREAD, .period_ms = TD_TICK_MS * TD_SLOT_TICKS,
  .metric = src_gauge_metric, .metric_cnt = SRC_GAUGE_METRIC_CNT,
  .read = src_gauge_read,
};
