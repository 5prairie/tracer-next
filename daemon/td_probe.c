/* td_probe.c: what this host exposes, measured. The host and its clock; and, when asked,
   the hardware counters: which events count, what a read costs, when they overflow, and
   whether opening them sent IPIs to other CPUs. Run it on a housekeeping CPU. */
#define _GNU_SOURCE
#include <sched.h>
#include <sys/utsname.h>

#include "../src/trace_pmu.h"
#include "td_probe.h"
#include "td_source.h"

#define TD_PROBE_READ_CNT    (2001)     /* reads timed for a median */
#define TD_PROBE_BUFFER_MIB  (128UL)
#define TD_PROBE_TOUCH_CNT   (2000000UL)
#define TD_PROBE_IDLE_US     (300000)
#define TD_PROBE_UNPATCH_US  (1500000)  /* the kernel unpatches 1 s after the last close */
#define TD_PROBE_IPI_ROW     "Function call interrupts"
#define TD_PROBE_LINE_BYTES  (256)
#define TD_PROBE_NO_LIST     "(null)"   /* how some kernels print an empty CPU list */

/* What a read() of a perf event returns with the read_format trace_pmu.h asks for. */
typedef struct {
  uint64_t value;
  uint64_t enabled_ns;
  uint64_t running_ns;
} td_probe_view_t;

/* A file of /proc or /sys that says something of the host, and what to call it. */
typedef struct {
  char const * label;
  char const * path;
} td_probe_file_t;

static td_probe_file_t const td_probe_host_file[] = {
  { "cpus online",                TD_ONLINE_FILE },
  { "cpus isolated",              TD_ISOLATED_FILE },
  { "cpus nohz_full",             TD_NOHZ_FULL_FILE },
  { "numa nodes",                 "/sys/devices/system/node/online" },
  { "free 2 MiB huge pages",      "/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages" },
  { "perf_event_paranoid",        TRACE_PMU_PARANOID_FILE },
  { "perf_user_access (aarch64)", "/proc/sys/kernel/perf_user_access" },
  { "rdpmc (x86)",                "/sys/bus/event_source/devices/cpu/rdpmc" },
};

/* Working memory of the two reports: too large for a stack, and one thread uses it. */
static uint64_t            td_probe_cost_tsc[ TD_PROBE_READ_CNT ];
static uint64_t            td_probe_ipi_cnt[ TD_CPU_MAX ], td_probe_ipi_after[ TD_CPU_MAX ];
static trace_pmu_counter_t td_probe_counter[ TRACE_COUNTER_MAX ];

uint64_t td_probe_host( void ) {
  uint64_t *      cost_tsc = td_probe_cost_tsc;
  struct utsname  kernel;
  char            text[ TD_PROBE_LINE_BYTES ];
  uint64_t        tsc_hz = trace_tsc_hz(), cost_median_tsc;

  uname( &kernel );
  printf( "== host\n" );
  printf( "%-28s %s  %s  %s\n", "arch, kernel", TRACE_ARCH_NAME, kernel.release, kernel.nodename );
  printf( "%-28s %ld bytes\n", "page size", sysconf( _SC_PAGESIZE ) );
  printf( "%-28s %d\n", "running on cpu", sched_getcpu() );
  for( int i = 0; i < TD_COUNT_OF( td_probe_host_file ); i++ ) {
    long   byte_cnt = td_file_read( td_probe_host_file[ i ].path, text, sizeof( text ) );
    char * value;
    if( byte_cnt < 0L ) snprintf( text, sizeof( text ), "(%s)", strerror( (int)-byte_cnt ) );
    value = text + strspn( text, " " );
    value[ strcspn( value, "\n" ) ] = '\0';
    if( !value[ 0 ] || !strcmp( value, TD_PROBE_NO_LIST ) ) value = "(none)";
    printf( "%-28s %s\n", td_probe_host_file[ i ].label, value );
  }

  for( int i = 0; i < TD_PROBE_READ_CNT; i++ ) {
    uint64_t start_tsc = trace_tsc();
    cost_tsc[ i ] = trace_tsc() - start_tsc;
  }
  cost_median_tsc = trace_pmu_median( cost_tsc, TD_PROBE_READ_CNT );
  printf( "%-28s %.3f MHz, invariant %d, resolution %.2f ns\n", "timestamp counter",
          (double)tsc_hz / 1e6, trace_tsc_is_invariant(),
          (double)TD_NS_PER_SECOND / (double)tsc_hz );
  printf( "%-28s %.1f ns median\n", "timestamp read cost",
          (double)cost_median_tsc * (double)TD_NS_PER_SECOND / (double)tsc_hz );
  return tsc_hz;
}

/* Per-CPU count of function-call IPIs: the kind the kernel uses to reach every core. One
   value per column of /proc/interrupts, which has one per online CPU. */
static int td_probe_ipi_read( uint64_t * ipi_cnt ) {
  td_rows_t rows;
  char *    row, * cursor;
  int       cpu_cnt = 0;
  if( td_file_read( TD_INTERRUPTS_FILE, td_scratch, TD_SCRATCH_BYTES ) < 0L ) {
    TD_LOG_ONCE( "tracerd: cannot read " TD_INTERRUPTS_FILE ": no IPI counts\n" );
    return 0;
  }
  row = strstr( td_scratch, TD_PROBE_IPI_ROW );  /* its description, at the end of the row */
  while( row && row > td_scratch && row[ -1 ] != '\n' ) row--;
  cursor = row ? strchr( row, ':' ) : NULL;
  if( !cursor || td_rows_begin( &rows, td_scratch, 0 ) ) return 0;
  for( cursor++; cpu_cnt < rows.column_cnt && cpu_cnt < TD_CPU_MAX; cpu_cnt++ )
    ipi_cnt[ cpu_cnt ] = strtoull( cursor, &cursor, 10 );
  return cpu_cnt;
}

static void td_probe_ipi_delta( char const * label, uint64_t * before ) {
  uint64_t * after   = td_probe_ipi_after;
  int        cpu_cnt = td_probe_ipi_read( after );
  printf( "%-28s", label );
  for( int i = 0; i < cpu_cnt; i++ )
    printf( " %llu", (unsigned long long)( after[ i ] - before[ i ] ) );
  printf( "\n" );
  memcpy( before, after, sizeof( td_probe_ipi_after ) );
}

static void td_probe_counter_report( char const * name, trace_pmu_counter_t * counter,
                                     uint64_t count_before, uint64_t tsc_hz ) {
  td_probe_view_t view         = { 0UL, 0UL, 0UL };  /* the kernel's own account of the event */
  int             is_live      = 0;
  uint64_t        count_after  = trace_pmu_read( counter->page, &is_live );
  uint64_t        overflow_cnt = trace_pmu_overflow_cnt( counter->page );
  uint64_t        read_ns      = trace_pmu_read_cost_ns( counter->page, tsc_hz );
  uint64_t        counted;
  double          scheduled_pct;

  if( read( counter->fd, &view, sizeof( view ) ) < 0 ) view.enabled_ns = 0UL;
  scheduled_pct = view.enabled_ns ? 100.0 * (double)view.running_ns / (double)view.enabled_ns : 0.0;
  counted   = is_live ? count_after - count_before : view.value;
  printf( "%-12s %-34s user-read %d  width %2u  overflow 2^%d  counted %10llu  scheduled %3.0f%%"
          "  read %5lu ns\n", name, counter->chosen, is_live, (unsigned)counter->page->pmc_width,
          overflow_cnt > 1UL ? 64 - __builtin_clzl( overflow_cnt - 1UL ) : 0,
          (unsigned long long)counted,
          scheduled_pct, (unsigned long)read_ns );
}

/* The first candidate of a metric that the kernel accepts. */
static void td_probe_counter_open( trace_metric_t const * metric, trace_pmu_host_t const * host,
                                   trace_pmu_counter_t * counter ) {
  trace_pmu_counter_t tried;
  counter->fd = -1;
  snprintf( counter->chosen, TRACE_CHOSEN_BYTES, TRACE_PMU_NO_EVENT );
  for( int i = 0; i < TRACE_CANDIDATE_MAX && counter->fd < 0; i++ ) {
    trace_candidate_t const * candidate = &metric->candidate[ i ];
    if( !candidate->sysfs_name && !candidate->type && !candidate->config ) break;
    trace_pmu_open( metric, i, host, &tried );
    /* Another vendor's candidate says nothing: it must not hide why this one's failed. */
    if( tried.fd >= 0 || strcmp( tried.chosen, TRACE_PMU_NO_EVENT ) ) *counter = tried;
  }
}

void td_probe_counters( uint64_t tsc_hz, uint64_t buffer_mib, char const * only ) {
  trace_pmu_counter_t * counter      = td_probe_counter;
  uint64_t *            ipi_cnt      = td_probe_ipi_cnt;
  uint64_t              buffer_bytes = ( buffer_mib ? buffer_mib : TD_PROBE_BUFFER_MIB ) << 20;
  uint64_t              count_before[ TRACE_COUNTER_MAX ];
  trace_pmu_host_t      host;
  uint8_t *             buffer;
  int                   is_live, metric_cnt = TRACE_METRIC_CNT;

  if( metric_cnt > TRACE_COUNTER_MAX ) metric_cnt = TRACE_COUNTER_MAX;
  trace_pmu_host( &host );
  printf( "== counters, read by the thread that owns them (kernel time %s)\n",
          host.is_user_only ? "excluded" : "included" );
  buffer = (uint8_t *)mmap( NULL, buffer_bytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
  if( buffer == MAP_FAILED ) { printf( "workload buffer: %s\n", strerror( errno ) ); return; }
  memset( buffer, 1, buffer_bytes );
  printf( "workload buffer %lu MiB\n", (unsigned long)( buffer_bytes >> 20 ) );

  td_probe_ipi_read( ipi_cnt );
  usleep( TD_PROBE_IDLE_US );
  td_probe_ipi_delta( "IPIs per cpu, idle", ipi_cnt );
  for( int i = 0; i < metric_cnt; i++ ) {
    counter[ i ].fd   = -1;
    counter[ i ].page = NULL;
    if( only && !strstr( only, trace_metric[ i ].name ) ) {
      snprintf( counter[ i ].chosen, TRACE_CHOSEN_BYTES, "not probed in this run" );
      continue;
    }
    td_probe_counter_open( &trace_metric[ i ], &host, &counter[ i ] );
  }
  td_probe_ipi_delta( "IPIs per cpu, events opened", ipi_cnt );

  for( int i = 0; i < metric_cnt; i++ )
    if( counter[ i ].fd >= 0 ) count_before[ i ] = trace_pmu_read( counter[ i ].page, &is_live );
  printf( "workload checksum %llu\n",
          (unsigned long long)trace_pmu_workload( buffer, buffer_bytes, TD_PROBE_TOUCH_CNT ) );
  for( int i = 0; i < metric_cnt; i++ ) {
    char const * name = trace_metric[ i ].name;
    if( counter[ i ].fd < 0 ) { printf( "%-12s %s\n", name, counter[ i ].chosen ); continue; }
    td_probe_counter_report( name, &counter[ i ], count_before[ i ], tsc_hz );
  }

  td_probe_ipi_read( ipi_cnt );
  for( int i = 0; i < metric_cnt; i++ ) trace_pmu_close( &counter[ i ] );
  usleep( TD_PROBE_UNPATCH_US );
  td_probe_ipi_delta( "IPIs per cpu, closed and unpatched", ipi_cnt );
  munmap( buffer, buffer_bytes );
}
