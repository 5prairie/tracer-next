/* src_sched.c: the checks of a sanitized thread. Pinned to one isolated core, never moved,
   never preempted, never waiting for a CPU, its hyperthread sibling idle, no time stolen by
   a hypervisor. Two are states, five are counts since the thread settled: all stay at zero.
   Each read takes the thread's schedstat file, which costs the thread nothing; its status
   file, whose read takes locks of the thread's, only once the thread has been switched in
   again or a second has passed; /proc/stat once a second. */
#include "td_source.h"

enum {
  SRC_SCHED_UNPINNED, SRC_SCHED_SHARED, SRC_SCHED_MOVES, SRC_SCHED_PREEMPTED, SRC_SCHED_WAIT,
  SRC_SCHED_SIBLING, SRC_SCHED_STEAL, SRC_SCHED_METRIC_CNT
};

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_sched_metric[ SRC_SCHED_METRIC_CNT ] = {
  { "unpinned",  "several CPUs allowed", "",      TD_CHECK, TD_LEVEL, 0, 1   },
  { "shared",    "core not isolated",    "",      TD_CHECK, TD_LEVEL, 0, 1   },
  { "moves",     "core changes",         "",      TD_CHECK, TD_TOTAL, 0, 1   },
  { "preempted", "involuntary switches", "",      TD_CHECK, TD_TOTAL, 0, 1   },
  { "wait",      "wait for a CPU",       "ns",    TD_CHECK, TD_TOTAL, 0, 1   },
  { "sibling",   "sibling thread busy",  "ticks", TD_CHECK, TD_TOTAL, 1, 100 },
  { "steal",     "stolen time",          "ticks", TD_CHECK, TD_TOTAL, 1, 100 },
};

#define SRC_SCHED_SIBLING_FILE "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list"
#define SRC_SCHED_STAT_FILE    "/proc/stat"
#define SRC_SCHED_ALLOWED_KEY  "Cpus_allowed_list:"
#define SRC_SCHED_PREEMPT_KEY  "nonvoluntary_ctxt_switches"
#define SRC_SCHED_STAT_ROW     "\ncpu%d "  /* where a CPU's row of /proc/stat begins */
#define SRC_SCHED_ROW_BYTES    (16)
#define SRC_SCHED_STAT_BYTES   (4UL << 20)  /* /proc/stat of 1024 CPUs, its interrupt line too */
#define SRC_SCHED_STAT_FIELDS  (8)          /* of a CPU's row: user to steal */
#define SRC_SCHED_STAT_READS   (10)         /* /proc/stat is read once in this many reads: it counts
                                               clock ticks, ten or four milliseconds each */
#define SRC_SCHED_STATUS_READS (10)         /* status is read once in this many at the least */

/* /proc/stat as last read, and at which tick: every thread of one tick is served from one
   read, and so are the ticks until the next read. */
static char      src_sched_stat_text[ SRC_SCHED_STAT_BYTES ];
static long      src_sched_stat_bytes;
static uint64_t  src_sched_stat_ns = UINT64_MAX;  /* no tick yet */
static uint64_t  src_sched_stat_tick_cnt;         /* ticks this source has read at */
static td_kept_t src_sched_stat_kept;

/* What is so for as long as a thread stays on its CPU, per thread slot: read by the
   probe, every few seconds and when a thread has changed CPU, not by each read. */
typedef struct {
  int cpu;        /* the CPU these are of */
  int sibling;    /* the other hardware thread of its core, or -1 */
  int is_shared;  /* it is not in the kernel's isolated list; -1: the list did not say */
} src_sched_place_t;

static src_sched_place_t src_sched_place[ TD_THREAD_MAX ];

/* What a thread's status file said when it was last read, per thread slot, and the
   thread's count of time slices then (the third number of schedstat). That count moves
   each time the thread is switched in: while it stands, the thread has not been switched,
   neither switch count of status has moved, and status is not read. It is read anyway once
   in SRC_SCHED_STATUS_READS: the allowed CPUs can change without a switch, and a thread
   switched out for good is never switched in again. */
typedef struct {
  uint64_t      found_ns;     /* of the thread these are of: a slot can come to hold another */
  unsigned long slice_cnt;
  long          preempt_cnt;
  int           is_unpinned;  /* -1: status did not say */
  int           has_status;
  int           skip_cnt;     /* reads since status was last read */
} src_sched_last_t;

static src_sched_last_t src_sched_last[ TD_THREAD_MAX ];

static long src_sched_task_file( td_ctx_t const * ctx, int leaf_idx ) {
  return td_ring_task_read( ctx->thread_idx, leaf_idx, td_scratch, TD_SCRATCH_BYTES );
}

/* The decimal number at *cursor, past the blanks before it. Moves *cursor past it and
   returns 1; or returns 0 when no digit is there. */
static int src_sched_number( char const ** cursor, unsigned long * value ) {
  char const * digit = *cursor;
  char const * first;
  *value = 0UL;
  while( *digit == ' ' ) digit++;
  for( first = digit; *digit >= '0' && *digit <= '9'; digit++ )
    *value = *value * 10UL + (unsigned long)( *digit - '0' );
  *cursor = digit;
  return digit != first;
}

/* The busy and the stolen clock ticks of one CPU since boot, from the text of /proc/stat. */
static int src_sched_cpu_ticks( char const * stat, int cpu, double * busy_ticks,
                                double * steal_ticks ) {
  /* user, nice, system, idle, iowait, irq, softirq, steal */
  unsigned long field[ SRC_SCHED_STAT_FIELDS ];
  char          key[ SRC_SCHED_ROW_BYTES ];
  char const *  line;
  snprintf( key, sizeof( key ), SRC_SCHED_STAT_ROW, cpu );
  line = strstr( stat, key );
  if( !line ) return -EINVAL;
  line += strlen( key );
  for( int i = 0; i < SRC_SCHED_STAT_FIELDS; i++ )
    if( !src_sched_number( &line, &field[ i ] ) ) return -EINVAL;
  *busy_ticks  = (double)( field[ 0 ] + field[ 1 ] + field[ 2 ] + field[ 5 ] + field[ 6 ] );
  *steal_ticks = (double)field[ 7 ];
  return 0;
}

/* The other hardware thread of the core `cpu` is on, or -1 when it has none. */
static int src_sched_sibling( int cpu ) {
  char path[ TD_PATH_BYTES ], text[ TD_CPU_LIST_BYTES ];
  long sibling;
  snprintf( path, sizeof( path ), SRC_SCHED_SIBLING_FILE, cpu );
  if( td_file_read( path, text, sizeof( text ) ) <= 0L ) return -1;
  for( long nth = 0L; ( sibling = td_list_nth( text, nth ) ) >= 0L; nth++ )
    if( sibling != cpu ) return (int)sibling;  /* "1,5" or "2-3": the first that is not cpu */
  return -1;
}

static void src_sched_place_read( src_sched_place_t * place, int cpu ) {
  char isolated[ TD_CPU_LIST_BYTES ] = "";
  long isolated_err = td_file_read( TD_ISOLATED_FILE, isolated, sizeof( isolated ) );
  place->cpu     = cpu;
  place->sibling = cpu < 0 ? -1 : src_sched_sibling( cpu );
  /* No such file: no core is isolated. Any other failure says nothing: no verdict. */
  if( isolated_err < 0L && isolated_err != -ENOENT ) place->is_shared = -1;
  else place->is_shared = !td_list_has( isolated, cpu );
}

static int src_sched_probe( td_ctx_t const * ctx, td_out_t * out ) {
  if( src_sched_task_file( ctx, TD_TASK_LEAF_STATUS ) <= 0L ) {
    td_unavailable( out, -1, "cannot read the thread's /proc files" );
    return -ENOENT;
  }
  if( src_sched_task_file( ctx, TD_TASK_LEAF_SCHEDSTAT ) <= 0L )
    td_unavailable( out, SRC_SCHED_WAIT, "no schedstat file: a kernel without CONFIG_SCHED_INFO" );
  src_sched_place_read( &src_sched_place[ ctx->thread_idx ], ctx->cpu );
  return 0;
}

/* Read the thread's status file into `last`: its involuntary switches, and whether it may
   run on several CPUs. */
static void src_sched_status_read( td_ctx_t const * ctx, src_sched_last_t * last ) {
  char const * allowed, * allowed_end;
  last->has_status = src_sched_task_file( ctx, TD_TASK_LEAF_STATUS ) > 0L;
  last->skip_cnt   = 0;
  if( !last->has_status ) return;
  allowed     = strstr( td_scratch, SRC_SCHED_ALLOWED_KEY );
  allowed_end = allowed ? strpbrk( allowed, ",-\n" ) : NULL;
  /* A list of one CPU has neither a comma nor a dash before the line ends. */
  last->is_unpinned = allowed_end ? *allowed_end != '\n' : -1;
  last->preempt_cnt = td_file_field( td_scratch, SRC_SCHED_PREEMPT_KEY );
}

static void src_sched_read( td_ctx_t const * ctx, td_out_t * out ) {
  src_sched_place_t * place = &src_sched_place[ ctx->thread_idx ];
  src_sched_last_t *  last  = &src_sched_last[ ctx->thread_idx ];
  char const *        cursor = td_scratch;
  double              busy_ticks, steal_ticks;
  unsigned long       run_ns, wait_ns = 0UL, slice_cnt = 0UL;
  int                 has_wait, has_slices;

  if( ctx->cpu < 0 ) return;  /* where the thread runs is not known yet */
  if( place->cpu != ctx->cpu ) src_sched_place_read( place, ctx->cpu );  /* it has moved */
  if( last->found_ns != td_thread[ ctx->thread_idx ].found_ns ) {  /* another thread's slot now */
    memset( last, 0, sizeof( *last ) );
    last->found_ns = td_thread[ ctx->thread_idx ].found_ns;
  }
  td_emit( out, SRC_SCHED_MOVES, "", (double)td_thread[ ctx->thread_idx ].move_cnt );
  if( place->is_shared >= 0 ) td_emit( out, SRC_SCHED_SHARED, "", (double)place->is_shared );

  /* schedstat: time on CPU, time waiting for one, time slices. Without the file (a kernel
     without CONFIG_SCHED_INFO) nothing says the thread was not switched: status every read. */
  has_wait   = src_sched_task_file( ctx, TD_TASK_LEAF_SCHEDSTAT ) > 0L &&
               src_sched_number( &cursor, &run_ns ) && src_sched_number( &cursor, &wait_ns );
  has_slices = has_wait && src_sched_number( &cursor, &slice_cnt );
  if( !has_slices || !last->has_status || slice_cnt != last->slice_cnt ||
      ++last->skip_cnt >= SRC_SCHED_STATUS_READS ) {
    src_sched_status_read( ctx, last );
    last->slice_cnt = slice_cnt;
  }
  if( last->has_status ) {
    td_emit( out, SRC_SCHED_PREEMPTED, "", (double)last->preempt_cnt );
    if( last->is_unpinned >= 0 ) td_emit( out, SRC_SCHED_UNPINNED, "", (double)last->is_unpinned );
  }
  if( has_wait ) td_emit( out, SRC_SCHED_WAIT, "", (double)wait_ns );
  /* On a tickless core steal is accounted when the thread next enters the kernel: it lags. */
  if( ctx->now_ns != src_sched_stat_ns ) {  /* the first thread of this tick */
    if( !( src_sched_stat_tick_cnt++ % SRC_SCHED_STAT_READS ) )
      src_sched_stat_bytes = td_kept_read( &src_sched_stat_kept, SRC_SCHED_STAT_FILE,
                                           src_sched_stat_text, sizeof( src_sched_stat_text ), 1 );
    src_sched_stat_ns = ctx->now_ns;
  }
  if( src_sched_stat_bytes <= 0L ) return;
  td_resolution( out, ctx->period_ms * SRC_SCHED_STAT_READS );  /* how often /proc/stat is read */
  td_of_cpu( out, ctx->cpu );  /* the two below are the core's counts, not the thread's */
  if( !src_sched_cpu_ticks( src_sched_stat_text, ctx->cpu, &busy_ticks, &steal_ticks ) )
    td_emit( out, SRC_SCHED_STEAL, "", steal_ticks );
  if( place->sibling >= 0 &&
      !src_sched_cpu_ticks( src_sched_stat_text, place->sibling, &busy_ticks, &steal_ticks ) )
    td_emit( out, SRC_SCHED_SIBLING, "", busy_ticks );
}

td_source_t const td_src_sched = {
  .name = "sched", .scope = TD_SCOPE_THREAD, .period_ms = 100,
  .metric = src_sched_metric, .metric_cnt = SRC_SCHED_METRIC_CNT,
  .probe = src_sched_probe, .read = src_sched_read,
};
