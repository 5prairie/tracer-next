/* src_numa.c: memory that came from the wrong node. Per node, the kernel's counts of pages
   it wanted there and had to take elsewhere. On a host with one node nothing can be
   remote, and the source says so. NOT RUN on a multi-node host yet; off by default. */
#include <glob.h>

#include "td_source.h"

enum { SRC_NUMA_FOREIGN, SRC_NUMA_MISS, SRC_NUMA_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_numa_metric[ SRC_NUMA_METRIC_CNT ] = {
  { "numa_foreign", "wanted here, got elsewhere", "pages", TD_TABLE, TD_TOTAL, 0, 1 },
  { "numa_miss",    "got here, wanted elsewhere", "pages", TD_TABLE, TD_TOTAL, 0, 1 },
};

#define SRC_NUMA_GLOB     "/sys/devices/system/node/node[0-9]*/numastat"
#define SRC_NUMA_NODE_DIR "/node/"  /* in a path of the glob, what precedes "node<N>" */
#define SRC_NUMA_NODE_MAX (64)

static char src_numa_path[ SRC_NUMA_NODE_MAX ][ TD_PATH_BYTES ];  /* filled once, by the probe */
static int  src_numa_node_cnt = -1;
static int  src_numa_is_full;  /* the host has more nodes than are read */

static int src_numa_probe( td_ctx_t const * ctx, td_out_t * out ) {
  glob_t found;
  (void)ctx;
  if( src_numa_node_cnt < 0 && !glob( SRC_NUMA_GLOB, 0, NULL, &found ) ) {
    src_numa_node_cnt = 0;
    for( size_t i = 0UL; i < found.gl_pathc && src_numa_node_cnt < SRC_NUMA_NODE_MAX; i++ )
      snprintf( src_numa_path[ src_numa_node_cnt++ ], TD_PATH_BYTES, "%s", found.gl_pathv[ i ] );
    src_numa_is_full = found.gl_pathc > SRC_NUMA_NODE_MAX;
    globfree( &found );
  }
  if( src_numa_is_full ) td_unavailable( out, SRC_NUMA_FOREIGN, "more nodes than are read" );
  if( src_numa_node_cnt > 1 ) return 0;
  td_unavailable( out, -1, src_numa_node_cnt < 0 ? "no node directory in sysfs"
                                                  : "one NUMA node: no memory is remote" );
  return -ENOENT;
}

static void src_numa_read( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  for( int i = 0; i < src_numa_node_cnt; i++ ) {
    char const * node = strstr( src_numa_path[ i ], SRC_NUMA_NODE_DIR );  /* ".../node/node3/..." */
    char         key[ TD_KEY_BYTES ];
    long         foreign_cnt, miss_cnt;
    if( !node || td_file_read( src_numa_path[ i ], td_scratch, TD_SCRATCH_BYTES ) <= 0L ) continue;
    node += sizeof( SRC_NUMA_NODE_DIR ) - 1UL;
    snprintf( key, sizeof( key ), "%.*s", (int)strcspn( node, "/" ), node );
    foreign_cnt = td_file_field( td_scratch, "numa_foreign" );
    miss_cnt    = td_file_field( td_scratch, "numa_miss" );
    if( foreign_cnt >= 0L ) td_emit( out, SRC_NUMA_FOREIGN, key, (double)foreign_cnt );
    if( miss_cnt    >= 0L ) td_emit( out, SRC_NUMA_MISS,    key, (double)miss_cnt );
  }
}

td_source_t const td_src_numa = {
  .name = "numa", .scope = TD_SCOPE_HOST, .period_ms = 1000, .is_unproven = 1,
  .metric = src_numa_metric, .metric_cnt = SRC_NUMA_METRIC_CNT,
  .probe = src_numa_probe, .read = src_numa_read,
};
