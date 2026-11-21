/* src_uncore.c: pressure outside the cores. Two sources. "uncore": what the memory
   controllers move. "mesh": traffic between sockets and on the mesh. PMUs, events and
   their encodings all come from sysfs. NOT RUN: no bare-metal host yet; off by default. */
#define _GNU_SOURCE
#include <glob.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>

#include "td_source.h"

enum { SRC_UNCORE_RATE, SRC_UNCORE_METRIC_CNT };

/* One row per metric. The key says which PMU, event and CPU: "uncore_imc_0 cas_count_read 0". */
static td_metric_t const src_uncore_metric[ SRC_UNCORE_METRIC_CNT ] = {
  { "rate", "", "/s", TD_TABLE, TD_RATE, 0, 0 },
};

typedef struct {
  char const * pmu_glob;    /* PMUs under /sys/bus/event_source/devices */
  char const * event_part;  /* events of theirs whose name contains this */
} src_uncore_want_t;

/* Memory controllers: Intel's count of reads and writes; Arm's DMC-620 likewise. */
static src_uncore_want_t const src_uncore_want[] = {
  { "uncore_imc_*", "cas_count_" }, { "arm_dmc620_*", "rdwr" },
};
/* Links between sockets, and the Arm mesh: data sent. A PMU that names no event in sysfs,
   as Intel's link PMUs do on many kernels, gives nothing: the page then says so. */
static src_uncore_want_t const src_mesh_want[] = {
  { "uncore_qpi_*", "_data" }, { "uncore_upi_*", "flit" }, { "arm_cmn_*", "txflit" },
};

#define SRC_UNCORE_DEVICES    "/sys/bus/event_source/devices/"
#define SRC_UNCORE_EVENTS     "/events/"          /* under a PMU's directory */
#define SRC_UNCORE_GLOB       SRC_UNCORE_DEVICES "%s" SRC_UNCORE_EVENTS "*%s*"
#define SRC_UNCORE_FORMAT     "%s/format/%.*s"    /* where a term's bits go */
#define SRC_UNCORE_SCALE      "%s" SRC_UNCORE_EVENTS "%s.scale"
#define SRC_UNCORE_UNIT       "%s" SRC_UNCORE_EVENTS "%s.unit"
#define SRC_UNCORE_TYPE       "%s/type"
#define SRC_UNCORE_CPUMASK    "%s/cpumask"
#define SRC_UNCORE_EVENT_MAX  (64)   /* events opened per source: one per event and PMU CPU */
#define SRC_UNCORE_LINE_BYTES (256)
#define SRC_UNCORE_CONFIG_CNT (3)   /* perf's config, config1 and config2 */

typedef struct {
  int    fd;
  int    cpu;       /* the CPU the kernel reads it on */
  int    is_moved;  /* that CPU has left the PMU's mask: the kernel reads it on another now */
  double scale;     /* from <event>.scale, else 1 */
  char   key[ TD_KEY_BYTES ];
  char   pmu_dir[ TD_PATH_BYTES ];
} src_uncore_event_t;

typedef struct {
  src_uncore_want_t const * want;
  int                       want_cnt;
  src_uncore_event_t        event[ SRC_UNCORE_EVENT_MAX ];
  int                       event_cnt;
  int                       is_probed;
  int                       is_full;   /* more events were found than are opened */
  char                      why[ TD_WHY_BYTES ];  /* why the last event that failed did */
} src_uncore_set_t;

static src_uncore_set_t src_uncore_set = {
  .want = src_uncore_want, .want_cnt = TD_COUNT_OF( src_uncore_want ),
};
static src_uncore_set_t src_mesh_set = {
  .want = src_mesh_want, .want_cnt = TD_COUNT_OF( src_mesh_want ),
};

/* Put `value` where the PMU's format file says the term goes: "config:0-7,21" spreads its
   bits over those of config, lowest first. Returns 0 or a negative errno. */
static int src_uncore_place( char const * pmu_dir, char const * term, int term_bytes,
                             uint64_t value, uint64_t * config ) {
  char   path[ 2 * TD_PATH_BYTES ], text[ SRC_UNCORE_LINE_BYTES ];
  char * cursor;
  int    config_idx = 0, bit_used = 0;

  snprintf( path, sizeof( path ), SRC_UNCORE_FORMAT, pmu_dir, term_bytes, term );
  if( td_file_read( path, text, sizeof( text ) ) <= 0L ) return -ENOTSUP;
  cursor = strchr( text, ':' );
  if( !cursor || strncmp( text, "config", sizeof( "config" ) - 1UL ) ) return -ENOTSUP;
  if( cursor[ -1 ] >= '1' && cursor[ -1 ] <= '9' ) config_idx = cursor[ -1 ] - '0';
  if( config_idx >= SRC_UNCORE_CONFIG_CNT ) return -ENOTSUP;  /* config1 and config2 only */
  while( *cursor == ':' || *cursor == ',' ) {
    long first = strtol( cursor + 1, &cursor, 10 ), last = first;
    if( *cursor == '-' ) last = strtol( cursor + 1, &cursor, 10 );
    if( first < 0L || last < first ) return -ENOTSUP;
    for( long bit = first; bit <= last && bit < 64L && bit_used < 64; bit++, bit_used++ )
      config[ config_idx ] |= ( ( value >> bit_used ) & 1UL ) << bit;
  }
  return bit_used < 64 && ( value >> bit_used ) ? -ERANGE : 0;
}

/* Turn a sysfs event description, "event=0x04,umask=0x03" or "event=0x2,inv", into perf
   configs, one term at a time. Returns 0 or a negative errno. */
static int src_uncore_configs( char const * pmu_dir, char const * text, uint64_t * config ) {
  while( *text && *text != '\n' ) {
    int      term_bytes = (int)strcspn( text, "=,\n" );
    uint64_t value      = 1UL;  /* a term without a value is a flag */
    char *   end        = (char *)text + term_bytes;
    if( *end == '=' ) value = strtoull( end + 1, &end, 0 );
    if( src_uncore_place( pmu_dir, text, term_bytes, value, config ) ) return -ENOTSUP;
    text = *end == ',' ? end + 1 : end;
  }
  return 0;
}

/* Does a traced thread run on `cpu`? A core can be set apart by a cpuset or by pinning
   alone, which the kernel's isolated and tickless lists do not show. */
static int src_uncore_is_traced( int cpu ) {
  for( int i = 0; i < TD_THREAD_MAX; i++ )
    if( td_thread[ i ].is_live && !td_thread[ i ].is_gone && td_thread[ i ].cpu == cpu ) return 1;
  return 0;
}

/* Open one event of one PMU on one of the CPUs the PMU names. Refused when that CPU is
   isolated or tickless: opening and reading the event interrupt it. Returns 0 or a
   negative errno, with the reason in set->why. */
static int src_uncore_open( src_uncore_set_t * set, char const * pmu_dir, char const * event_name,
                            int cpu, struct perf_event_attr const * attr ) {
  src_uncore_event_t * event = &set->event[ set->event_cnt ];
  char                 path[ 2 * TD_PATH_BYTES ], text[ SRC_UNCORE_LINE_BYTES ] = "";
  char                 key[ 2 * TD_PATH_BYTES + SRC_UNCORE_LINE_BYTES ];
  char                 isolated[ TD_CPU_LIST_BYTES ] = "", tickless[ TD_CPU_LIST_BYTES ] = "";
  char const *         pmu_name = strrchr( pmu_dir, '/' ) + 1;
  long                 isolated_err, tickless_err;
  int                  is_unknown;

  /* A list that cannot be read in full says nothing: then the answer is no. */
  isolated_err = td_file_read( TD_ISOLATED_FILE,  isolated, sizeof( isolated ) );
  tickless_err = td_file_read( TD_NOHZ_FULL_FILE, tickless, sizeof( tickless ) );
  is_unknown   = ( isolated_err < 0L && isolated_err != -ENOENT ) ||
                 ( tickless_err < 0L && tickless_err != -ENOENT );
  if( is_unknown || td_list_has( isolated, cpu ) || td_list_has( tickless, cpu ) ) {
    snprintf( set->why, sizeof( set->why ), "%s is read on cpu %d, which %s", pmu_name, cpu,
              is_unknown ? "may be isolated: lists unreadable" : "is isolated or tickless" );
    return -EBUSY;
  }
  if( src_uncore_is_traced( cpu ) ) {  /* set apart by pinning alone: the lists do not say */
    snprintf( set->why, sizeof( set->why ), "%s is read on cpu %d, where a traced thread runs",
              pmu_name, cpu );
    return -EBUSY;
  }
  event->fd = (int)syscall( SYS_perf_event_open, attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC );
  if( event->fd < 0 ) {
    int err = errno;  /* snprintf may change errno */
    snprintf( set->why, sizeof( set->why ), "%s %s: %s%s", pmu_name, event_name, strerror( err ),
              err == EACCES ? " (needs perf_event_paranoid 0 or lower)" : "" );
    return -err;
  }
  event->cpu      = cpu;
  event->is_moved = 0;
  event->scale    = 1.0;
  snprintf( event->pmu_dir, sizeof( event->pmu_dir ), "%s", pmu_dir );
  snprintf( path, sizeof( path ), SRC_UNCORE_SCALE, pmu_dir, event_name );
  if( td_file_read( path, text, sizeof( text ) ) > 0L ) event->scale = atof( text );
  snprintf( path, sizeof( path ), SRC_UNCORE_UNIT, pmu_dir, event_name );
  text[ 0 ] = '\0';  /* the scaled count's unit, "MiB" for instance: part of the row's name */
  if( td_file_read( path, text, sizeof( text ) ) > 0L ) text[ strcspn( text, "\n" ) ] = '\0';
  snprintf( key, sizeof( key ), "%s %s %d%s%s", pmu_name, event_name, cpu, text[ 0 ] ? " " : "",
            text );
  snprintf( event->key, sizeof( event->key ), "%.*s", TD_KEY_BYTES - 1, key );
  set->event_cnt++;
  return 0;
}

/* One event file of sysfs: build its perf configs and open it on every CPU of its PMU. */
static void src_uncore_open_event( src_uncore_set_t * set, char const * path ) {
  struct perf_event_attr attr;
  uint64_t               config[ SRC_UNCORE_CONFIG_CNT ] = { 0UL };
  char const *           event_name = strrchr( path, '/' ) + 1;
  char                   pmu_dir[ TD_PATH_BYTES ], leaf[ 2 * TD_PATH_BYTES ];
  char                   text[ SRC_UNCORE_LINE_BYTES ] = "", cpus[ TD_CPU_LIST_BYTES ] = "";
  long                   cpu;
  int                    dir_bytes = (int)( strstr( path, SRC_UNCORE_EVENTS ) - path );

  if( strchr( event_name, '.' ) ) return;  /* a .scale or .unit file, not an event */
  snprintf( pmu_dir, sizeof( pmu_dir ), "%.*s", dir_bytes, path );
  memset( &attr, 0, sizeof( attr ) );
  attr.size = sizeof( attr );
  snprintf( leaf, sizeof( leaf ), SRC_UNCORE_TYPE, pmu_dir );
  if( td_file_read( leaf, text, sizeof( text ) ) <= 0L ) return;
  attr.type = (uint32_t)atoi( text );
  text[ 0 ] = '\0';
  if( td_file_read( path, text, sizeof( text ) ) <= 0L ) {
    snprintf( set->why, sizeof( set->why ), "%.*s: cannot be read", TD_KEY_BYTES, event_name );
    return;
  }
  if( src_uncore_configs( pmu_dir, text, config ) ) {
    snprintf( set->why, sizeof( set->why ), "%.*s: an encoding that cannot be placed",
              TD_KEY_BYTES, event_name );
    return;
  }
  attr.config  = config[ 0 ];
  attr.config1 = config[ 1 ];
  attr.config2 = config[ 2 ];
  snprintf( leaf, sizeof( leaf ), SRC_UNCORE_CPUMASK, pmu_dir );
  td_file_read( leaf, cpus, sizeof( cpus ) );
  for( long nth = 0L; ( cpu = td_list_nth( cpus, nth ) ) >= 0L; nth++ ) {
    if( set->event_cnt == SRC_UNCORE_EVENT_MAX ) { set->is_full = 1; break; }
    src_uncore_open( set, pmu_dir, event_name, (int)cpu, &attr );
  }
}

/* What has changed under the events since they were opened. The CPU of one may have gone
   offline: the kernel then reads the event on another CPU of its choice, maybe a traced
   one. Or a thread may by now run on it. Such an event is no longer read, and said so; it
   is not closed, because closing interrupts that CPU as well. */
static void src_uncore_recheck( src_uncore_set_t * set, td_out_t * out ) {
  char path[ 2 * TD_PATH_BYTES ], cpus[ TD_CPU_LIST_BYTES ], why[ TD_WHY_BYTES ];
  for( int i = 0; i < set->event_cnt; i++ ) {
    src_uncore_event_t * event = &set->event[ i ];
    snprintf( path, sizeof( path ), SRC_UNCORE_CPUMASK, event->pmu_dir );
    if( td_file_read( path, cpus, sizeof( cpus ) ) <= 0L || !td_list_has( cpus, event->cpu ) )
      event->is_moved = 1;
    if( !event->is_moved && !src_uncore_is_traced( event->cpu ) ) continue;
    snprintf( why, sizeof( why ), "%s: no longer read, cpu %d %s", event->key, event->cpu,
              event->is_moved ? "left its PMU's mask" : "now runs a traced thread" );
    td_unavailable( out, SRC_UNCORE_RATE, why );
  }
}

/* Open every wanted event the host has. Runs once: events stay open. */
static int src_uncore_probe_set( src_uncore_set_t * set, td_out_t * out ) {
  for( int want_idx = 0; !set->is_probed && want_idx < set->want_cnt; want_idx++ ) {
    char   pattern[ TD_PATH_BYTES ];
    glob_t found;
    snprintf( pattern, sizeof( pattern ), SRC_UNCORE_GLOB, set->want[ want_idx ].pmu_glob,
              set->want[ want_idx ].event_part );
    if( glob( pattern, 0, NULL, &found ) ) continue;
    for( size_t i = 0UL; i < found.gl_pathc; i++ )
      src_uncore_open_event( set, found.gl_pathv[ i ] );
    globfree( &found );
  }
  if( set->is_probed ) src_uncore_recheck( set, out );
  set->is_probed = 1;
  if( set->event_cnt && set->why[ 0 ] ) td_unavailable( out, SRC_UNCORE_RATE, set->why );
  if( set->is_full ) td_unavailable( out, SRC_UNCORE_RATE, "more events than are opened" );
  if( set->event_cnt ) return 0;
  if( !set->why[ 0 ] ) snprintf( set->why, sizeof( set->why ), "no such PMU with named events" );
  td_unavailable( out, -1, set->why );
  return -ENOENT;
}

static void src_uncore_read_set( src_uncore_set_t const * set, td_out_t * out ) {
  for( int i = 0; i < set->event_cnt; i++ ) {
    uint64_t count;
    if( set->event[ i ].is_moved ) continue;
    if( src_uncore_is_traced( set->event[ i ].cpu ) ) continue;  /* the read would interrupt it */
    if( read( set->event[ i ].fd, &count, sizeof( count ) ) != (ssize_t)sizeof( count ) ) continue;
    td_emit( out, SRC_UNCORE_RATE, set->event[ i ].key, (double)count * set->event[ i ].scale );
  }
}

static int src_uncore_probe( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  return src_uncore_probe_set( &src_uncore_set, out );
}

static void src_uncore_read( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  src_uncore_read_set( &src_uncore_set, out );
}

static int src_mesh_probe( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  return src_uncore_probe_set( &src_mesh_set, out );
}

static void src_mesh_read( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  src_uncore_read_set( &src_mesh_set, out );
}

td_source_t const td_src_uncore = {
  .name = "uncore", .scope = TD_SCOPE_HOST, .period_ms = 1000, .is_unproven = 1,
  .metric = src_uncore_metric, .metric_cnt = SRC_UNCORE_METRIC_CNT,
  .probe = src_uncore_probe, .read = src_uncore_read,
};

td_source_t const td_src_mesh = {
  .name = "mesh", .scope = TD_SCOPE_HOST, .period_ms = 1000, .is_unproven = 1,
  .metric = src_uncore_metric, .metric_cnt = SRC_UNCORE_METRIC_CNT,
  .probe = src_mesh_probe, .read = src_mesh_read,
};
