/* src_faults.c: page faults of each traced thread. A sanitized thread takes none once it is
   warm, so a minor fault is worth a mark and a major one is an incident.
   From the thread's own /proc stat file, which td_ring_find.c reads at most once per tick:
   the read that finds where the thread runs serves here too. A thread in user space never
   waits for it; docs/reading-the-page.md says what it can cost one in the kernel. */
#include "td_source.h"

enum { SRC_FAULTS_MINOR, SRC_FAULTS_MAJOR, SRC_FAULTS_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_faults_metric[ SRC_FAULTS_METRIC_CNT ] = {
  { "minor", "minor faults", "/s", TD_LANE,  TD_RATE,  1, 100 },
  { "major", "major faults", "",   TD_CHECK, TD_TOTAL, 0, 1 },
};

static int src_faults_get( td_ctx_t const * ctx, unsigned long * minor, unsigned long * major ) {
  td_task_stat_t stat;
  int            err = td_ring_task_stat( ctx->thread_idx, ctx->now_ns, &stat );
  if( err ) return err;
  *minor = stat.minor_cnt;
  *major = stat.major_cnt;
  return 0;
}

static int src_faults_probe( td_ctx_t const * ctx, td_out_t * out ) {
  unsigned long minor, major;
  if( !src_faults_get( ctx, &minor, &major ) ) return 0;
  td_unavailable( out, -1, "cannot read the thread's /proc stat file" );
  return -ENOENT;
}

static void src_faults_read( td_ctx_t const * ctx, td_out_t * out ) {
  unsigned long minor, major;
  if( src_faults_get( ctx, &minor, &major ) ) return;
  td_emit( out, SRC_FAULTS_MINOR, "", (double)minor );
  td_emit( out, SRC_FAULTS_MAJOR, "", (double)major );
}

td_source_t const td_src_faults = {
  .name = "faults", .scope = TD_SCOPE_THREAD, .period_ms = 100,
  .metric = src_faults_metric, .metric_cnt = SRC_FAULTS_METRIC_CNT,
  .probe = src_faults_probe, .read = src_faults_read,
};
