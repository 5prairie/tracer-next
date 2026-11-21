/* trace_pmu.h: hardware counters, read by the thread that owns them. Optional: include it
   and call trace_pmu_attach() after trace_attach(); without TRACE_PMU=1 it does nothing.
   Cold part: pick, open and test an event per metric. Hot part: one read per period. */
#ifndef TRACE_PMU_H
#define TRACE_PMU_H

#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "trace.h"
#include "trace_events.h"

#define TRACE_PMU_PERIOD_MIN_NS    (TRACE_NS_PER_SECOND / 1000UL)  /* never sample more often */
#define TRACE_PMU_PERIOD_MAX_NS    (TRACE_NS_PER_SECOND)           /* nor less often */
#define TRACE_PMU_COST_FACTOR      (100000UL)             /* period = read cost x this */
#define TRACE_PMU_COST_READS       (64)                   /* reads timed as one batch */
#define TRACE_PMU_COST_BATCHES     (5)                    /* the median batch is taken */
#define TRACE_PMU_TEST_BYTES       (16UL << 20)           /* self-test: random reads over this */
#define TRACE_PMU_TEST_READS       (200000UL)
#define TRACE_PMU_OVERFLOW_HZ      (6UL * 4000000000UL)   /* the fastest event: 6 slots a cycle */
#define TRACE_PMU_OVERFLOW_SECONDS (3600UL)               /* overflow sooner than this: refused */
#define TRACE_PMU_PATH_BYTES       (192)
#define TRACE_PMU_LINE_BYTES       (128)                  /* one line of a sysfs or /proc file */
#define TRACE_PMU_PARANOID_FILE    "/proc/sys/kernel/perf_event_paranoid"
#define TRACE_PMU_EVENT_FILE       "%s/events/%s"         /* under the PMU's sysfs directory */
#define TRACE_PMU_TYPE_FILE        "%s/type"
#define TRACE_PMU_SEED             (88172645463325252UL)  /* any non-zero start for xorshift */
#define TRACE_PMU_TERM_CNT         (3)                    /* terms of a sysfs event placed here */
#define TRACE_PMU_PARANOID_USER    (2)                    /* at or above: user space only */
#define TRACE_PMU_NO_EVENT         "none: no event for this CPU"
#define TRACE_PMU_UNPROVEN         " (unproven: read zero)"  /* kept though the self-test did */
#define TRACE_PMU_READ_ZERO        "every event that opened read zero under the self-test"
#define TRACE_PMU_USER_ACCESS_OFF  TRACE_ARCH_USER_ACCESS_NAME " is 0: no counter can be read here"
#define TRACE_PMU_SMALL_PAGE_BYTES (4096L)                /* the self-test's TLB misses need it */
/* Why no metric got an event: the reason that is kept is the one that says most. */
#define TRACE_PMU_RANK_NOT_OPENED  (1)                    /* the kernel would not open an event */
#define TRACE_PMU_RANK_READ_ZERO   (2)                    /* one opened, and counted nothing */
#define TRACE_PMU_RANK_REFUSED     (3)                    /* one opened, and cannot be used */

typedef struct {
  int                           fd;    /* -1: the metric has no usable event */
  struct perf_event_mmap_page * page;
  char                          chosen[ TRACE_CHOSEN_BYTES ];  /* which candidate, or why none */
} trace_pmu_counter_t;

/* What the host's core PMU looks like; found once per attach. */
typedef struct {
  char sysfs_dir[ TRACE_PMU_PATH_BYTES ];  /* "" when sysfs does not list one */
  int  vendor;
  int  is_user_only;    /* this user may not count kernel time */
  char refusal[ TRACE_CHOSEN_BYTES ];      /* why an event was refused: see trace_pmu_is_usable */
  int  refusal_rank;    /* TRACE_PMU_RANK_* of that reason; 0: there is none */
} trace_pmu_host_t;

/* The calling thread's counters. Each translation unit that includes this header has its
   own; a program uses the one whose trace_pmu_attach it calls. What the sampler reads on its
   way to the counter is on one cache line: a sample comes once per period, by when these
   lines may have left the cache, and each one more is one more miss in a row before the
   read (measured with all of them flushed: 406 to 428 ns for two lines, 264 to 269 for one). */
static __thread struct {
  struct perf_event_mmap_page * next_page;  /* page[ next_idx ]: no second line to fetch first */
  uint64_t                      period_tsc;
  uint64_t                      next_tsc;   /* when the next counter is read */
  int                           counter_cnt;
  int                           next_idx;   /* round robin */
  struct perf_event_mmap_page * page[ TRACE_COUNTER_MAX ];
} trace_pmu_thread __attribute__(( tls_model( "initial-exec" ), aligned( 64 ) ));

/* The same counters as they were opened: the descriptors and which event each is. Cold. */
static __thread trace_pmu_counter_t trace_pmu_opened[ TRACE_COUNTER_MAX ]
  __attribute__(( tls_model( "initial-exec" ) ));

/* Ticks per second of trace_tsc, measured once for the process: in trace_attach.c. */
#ifdef __cplusplus
extern "C" {
#endif
uint64_t trace_attach_tsc_hz( void );
#ifdef __cplusplus
}
#endif

static inline long trace_pmu_file_line( char const * path, char * text, size_t text_bytes ) {
  FILE * file = fopen( path, "r" );
  if( !file ) return -errno;
  if( !fgets( text, (int)text_bytes, file ) ) text[ 0 ] = '\0';
  fclose( file );
  return 0L;
}

static inline void trace_pmu_host( trace_pmu_host_t * host ) {
  glob_t found;
  char   text[ TRACE_PMU_LINE_BYTES ] = "";
  host->sysfs_dir[ 0 ] = host->refusal[ 0 ] = '\0';
  host->refusal_rank   = 0;
  if( !glob( TRACE_ARCH_PMU_GLOB, GLOB_BRACE, NULL, &found ) ) {
    char const * first = found.gl_pathc ? found.gl_pathv[ 0 ] : "";
    snprintf( host->sysfs_dir, sizeof( host->sysfs_dir ), "%s", first );
    globfree( &found );
  }
  host->vendor = trace_arch_vendor();
  host->is_user_only = 1;
  if( !trace_pmu_file_line( TRACE_PMU_PARANOID_FILE, text, sizeof( text ) ) )
    host->is_user_only = atoi( text ) >= TRACE_PMU_PARANOID_USER;
}

/* Turn sysfs "event=0xc5,umask=0x00" into a perf config. Returns -ENOTSUP for a term this
   code does not place (inv, edge, period, ...): better no counter than a wrong one. */
static inline int trace_pmu_sysfs_config( char const * text, uint64_t * config ) {
  static char const * const term[ TRACE_PMU_TERM_CNT ] = { "event", "umask", "cmask" };
  uint64_t                  value[ TRACE_PMU_TERM_CNT ] = { 0UL };
  while( *text && *text != '\n' ) {
    char * end;
    size_t name_bytes = strcspn( text, "=,\n" );
    int    term_idx   = 0;
    while( term_idx < TRACE_PMU_TERM_CNT && ( strlen( term[ term_idx ] ) != name_bytes ||
                                              strncmp( text, term[ term_idx ], name_bytes ) ) )
      term_idx++;
    if( term_idx == TRACE_PMU_TERM_CNT || text[ name_bytes ] != '=' ) return -ENOTSUP;
    value[ term_idx ] = strtoull( text + name_bytes + 1UL, &end, 0 );
    text = *end == ',' ? end + 1 : end;
  }
  *config = TRACE_ARCH_EVENT( value[ 0 ], value[ 1 ], value[ 2 ] );
  return 0;
}

/* Fill attr for one candidate. Returns 0, or a negative errno when sysfs lacks the name. */
static inline int trace_pmu_candidate_attr( trace_candidate_t const * candidate,
                                            trace_pmu_host_t const * host,
                                            struct perf_event_attr * attr, char * chosen ) {
  char     path[ TRACE_PMU_PATH_BYTES + TRACE_NAME_BYTES + sizeof( "/events/" ) ];
  char     text[ TRACE_PMU_LINE_BYTES ];
  uint64_t config;
  int      err;
  if( !candidate->sysfs_name ) {
    attr->type   = candidate->type;
    attr->config = candidate->config;
    snprintf( chosen, TRACE_CHOSEN_BYTES, "type %u config 0x%llx", attr->type,
              (unsigned long long)attr->config );
    return 0;
  }
  if( !host->sysfs_dir[ 0 ] ) return -ENOENT;
  snprintf( path, sizeof( path ), TRACE_PMU_EVENT_FILE, host->sysfs_dir, candidate->sysfs_name );
  if( trace_pmu_file_line( path, text, sizeof( text ) ) ) return -ENOENT;
  err = trace_pmu_sysfs_config( text, &config );
  if( err ) return err;
  attr->config = config;
  snprintf( path, sizeof( path ), TRACE_PMU_TYPE_FILE, host->sysfs_dir );
  if( trace_pmu_file_line( path, text, sizeof( text ) ) ) return -ENOENT;
  attr->type = (uint32_t)strtoul( text, NULL, 0 );
  snprintf( chosen, TRACE_CHOSEN_BYTES, "sysfs %s", candidate->sysfs_name );
  return 0;
}

/* Open candidate `candidate_idx` of a metric on the calling thread. Pinned: it counts all
   the time or not at all. No sample period: it interrupts only on overflow. On failure
   fd is -1 and chosen says why. Closing a counter IPIs the cores of the process's other
   running threads: like everything here, only before the hot loops start. */
static inline void trace_pmu_open( trace_metric_t const * metric, int candidate_idx,
                                   trace_pmu_host_t const * host, trace_pmu_counter_t * counter ) {
  trace_candidate_t const * candidate = &metric->candidate[ candidate_idx ];
  struct perf_event_attr    attr;
  void *                    page = NULL;
  long                      fd   = -1L;
  int                       err;

  counter->fd   = -1;
  counter->page = NULL;
  snprintf( counter->chosen, TRACE_CHOSEN_BYTES, TRACE_PMU_NO_EVENT );
  if( candidate->vendor != TRACE_VENDOR_ANY && candidate->vendor != host->vendor ) return;

  memset( &attr, 0, sizeof( attr ) );
  attr.size           = sizeof( attr );
  attr.pinned         = 1;
  attr.exclude_kernel = !!host->is_user_only;
  attr.exclude_hv     = 1;
  attr.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
  attr.config1        = TRACE_ARCH_PMU_CONFIG1;
  err = trace_pmu_candidate_attr( candidate, host, &attr, counter->chosen );
  if( !err ) {
    fd  = syscall( SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC );
    err = fd < 0L ? -errno : 0;
  }
  if( !err ) {
    page = mmap( NULL, (size_t)sysconf( _SC_PAGESIZE ), PROT_READ, MAP_SHARED, (int)fd, 0 );
    if( page == MAP_FAILED ) { err = -errno; close( (int)fd ); }
  }
  if( err ) {
    snprintf( counter->chosen, TRACE_CHOSEN_BYTES, "none: %s", strerror( -err ) );
    return;
  }
  counter->fd   = (int)fd;
  counter->page = (struct perf_event_mmap_page *)page;
}

static inline void trace_pmu_close( trace_pmu_counter_t * counter ) {
  if( counter->page ) munmap( counter->page, (size_t)sysconf( _SC_PAGESIZE ) );
  if( counter->fd >= 0 ) close( counter->fd );
  counter->fd   = -1;
  counter->page = NULL;
}

/* Read a counter from user space: no syscall. *is_live is 0 when the kernel does not let
   this thread read it directly (not on a hardware counter, or user access is off); the
   value is then the kernel's last saved count and does not advance. The kernel's protocol:
   the lock is read before and after, and the compiler may move no other load across either
   read, hence the two barriers. They emit no instruction. */
static inline uint64_t trace_pmu_read( struct perf_event_mmap_page const * page, int * is_live ) {
  uint32_t seq, index;
  uint64_t value;
  do {
    seq   = __atomic_load_n( &page->lock, __ATOMIC_ACQUIRE );
    __asm__ volatile( "" ::: "memory" );  /* what follows is loaded after the lock, not before */
    index = page->cap_user_rdpmc ? page->index : 0U;
    value = (uint64_t)page->offset;
    if( index ) {
      int      shift = 64 - (int)page->pmc_width;
      uint64_t raw   = trace_counter_read( index - 1U );
      value += (uint64_t)( (int64_t)( raw << shift ) >> shift );
    }
    __asm__ volatile( "" ::: "memory" );  /* reload the lock: the kernel may have rewritten it */
  } while( __atomic_load_n( &page->lock, __ATOMIC_ACQUIRE ) != seq );
  *is_live = index != 0U;
  return value;
}

/* Counts left before the hardware counter overflows and interrupts its core, or 0 when it
   cannot be read. The kernel starts a counting event at minus its maximum period. */
static inline uint64_t trace_pmu_overflow_cnt( struct perf_event_mmap_page const * page ) {
  int     shift = 64 - (int)page->pmc_width;
  int64_t raw;
  if( !page->cap_user_rdpmc || !page->index ) return 0UL;
  raw = (int64_t)( trace_counter_read( page->index - 1U ) << shift ) >> shift;
  return raw < 0L ? (uint64_t)-raw : 1UL << ( page->pmc_width - 1 );
}

/* ---- the sampler ---------------------------------------------------------------------- */

/* The median of a few durations. Sorts in place: the arrays are short and this runs once. */
static inline uint64_t trace_pmu_median( uint64_t * value, int value_cnt ) {
  for( int i = 1; i < value_cnt; i++ ) {
    uint64_t moved = value[ i ];
    int      slot  = i;
    for( ; slot > 0 && value[ slot - 1 ] > moved; slot-- ) value[ slot ] = value[ slot - 1 ];
    value[ slot ] = moved;
  }
  return value[ value_cnt / 2 ];
}

/* Once per period: read one counter, write it as an event. tsc is the timestamp trace_now
   took just before: the event carries it, so the count is stamped at the start of its read
   and not at the end (the same offset every time, which a rate does not see), and a sample
   costs no timestamp of its own: 8 ns less of 18 on Zen 4, 14 less of 29 on Granite Rapids,
   the counter read aside. */
static inline void trace_pmu_sample( uint64_t tsc ) {
  int      counter_idx = trace_pmu_thread.next_idx, next_idx, is_live;
  uint64_t value       = trace_pmu_read( trace_pmu_thread.next_page, &is_live );
  if( is_live ) trace_emit( tsc, trace_word( TRACE_KIND_COUNTER, counter_idx, value ) );
  next_idx = counter_idx + 1 < trace_pmu_thread.counter_cnt ? counter_idx + 1 : 0;
  trace_pmu_thread.next_idx  = next_idx;
  trace_pmu_thread.next_page = trace_pmu_thread.page[ next_idx ];
  trace_pmu_thread.next_tsc  = tsc + trace_pmu_thread.period_tsc;
}

/* What trace_now calls on a thread with counters, in place of the one trace_attach set. So
   it does that one's work first, the ring's lines asked back at each call, then reads a
   counter if the counters' own period is over. Due again at the earlier of the two. */
static void trace_pmu_sampler( trace_writer_t * writer, uint64_t tsc ) {
  uint64_t own_next_tsc = trace_own_back( writer, tsc );
  if( tsc >= trace_pmu_thread.next_tsc ) trace_pmu_sample( tsc );
  writer->sample_next_tsc = own_next_tsc < trace_pmu_thread.next_tsc ? own_next_tsc
                                                                     : trace_pmu_thread.next_tsc;
}

/* Memory and branches no predictor or cache can absorb: every miss and stall metric moves. */
static inline uint64_t trace_pmu_workload( uint8_t const * buffer, uint64_t buffer_bytes,
                                           uint64_t read_cnt ) {
  uint64_t          random = TRACE_PMU_SEED, sum = 0UL;
  volatile uint64_t taken  = 0UL;
  for( uint64_t i = 0UL; i < read_cnt; i++ ) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    sum += buffer[ random % buffer_bytes ];
    if( random & 0x10000UL ) taken = taken + 1UL;
  }
  return sum + taken;
}

/* Is the counter on a hardware counter, readable from here, and slow to overflow? When
   not, host->refusal says why: the reason given if no metric gets an event at all. */
static inline int trace_pmu_is_usable( trace_pmu_counter_t const * counter,
                                       trace_pmu_host_t * host ) {
  uint64_t     floor_cnt = (uint64_t)TRACE_PMU_OVERFLOW_SECONDS * TRACE_PMU_OVERFLOW_HZ;
  char const * why       = counter->chosen;  /* "none: <the error>" when it did not open */
  int          is_live   = 0;
  if( counter->fd >= 0 ) {
    trace_pmu_read( counter->page, &is_live );
    if( is_live && trace_pmu_overflow_cnt( counter->page ) >= floor_cnt ) return 1;
    why = is_live ? "a counter would overflow, and interrupt, within the hour"
                  : "no counter is readable from user space";
  }
  /* Another vendor's candidate is no refusal; nor does an event that did not open hide
     what was learnt from one that did. */
  if( !strcmp( why, TRACE_PMU_NO_EVENT ) ) return 0;
  if( counter->fd < 0 && host->refusal_rank >= TRACE_PMU_RANK_NOT_OPENED ) return 0;
  host->refusal_rank = counter->fd < 0 ? TRACE_PMU_RANK_NOT_OPENED : TRACE_PMU_RANK_REFUSED;
  snprintf( host->refusal, sizeof( host->refusal ), "%s", why );
  return 0;
}

/* May this metric keep an event that read zero under the self-test? See trace_events.h. */
static inline int trace_pmu_may_idle( trace_metric_t const * metric ) {
  if( metric->may_idle == TRACE_EVENTS_MAY_IDLE_BIG_PAGES )
    return sysconf( _SC_PAGESIZE ) > TRACE_PMU_SMALL_PAGE_BYTES;
  return metric->may_idle == TRACE_EVENTS_MAY_IDLE;
}

/* Find the event of one metric: the first candidate that is usable and counts under the
   workload. One that reads zero is closed: sampled, it would cost a read every round and
   show nothing, as does a counter the hypervisor does not back (seen: the sixth of six on
   one guest, the last-level events on another). host->refusal then says so, unless an
   event was refused outright. Only a metric the workload cannot be expected to move keeps
   its first usable candidate, marked unproven. Returns 1 if an event is kept. */
static inline int trace_pmu_pick( trace_metric_t const * metric, trace_pmu_host_t * host,
                                  uint8_t const * buffer, trace_pmu_counter_t * kept,
                                  int * is_proven ) {
  trace_pmu_counter_t tried;
  int                 is_live, may_idle = trace_pmu_may_idle( metric );
  kept->fd   = -1;
  kept->page = NULL;
  *is_proven = 0;
  for( int i = 0; i < TRACE_CANDIDATE_MAX && !*is_proven; i++ ) {
    trace_candidate_t const * candidate = &metric->candidate[ i ];
    uint64_t                  before;
    uint64_t                  sum;
    if( !candidate->sysfs_name && !candidate->type && !candidate->config ) break;
    trace_pmu_open( metric, i, host, &tried );
    if( !trace_pmu_is_usable( &tried, host ) ) { trace_pmu_close( &tried ); continue; }
    before = trace_pmu_read( tried.page, &is_live );
    sum    = trace_pmu_workload( buffer, TRACE_PMU_TEST_BYTES, TRACE_PMU_TEST_READS );
    __asm__ volatile( "" : : "r"( sum ) );  /* unused, the reads would be dropped */
    *is_proven = trace_pmu_read( tried.page, &is_live ) != before;
    if( *is_proven || ( may_idle && kept->fd < 0 ) ) {  /* it counts, or is the first to idle */
      trace_pmu_close( kept );
      *kept = tried;
      continue;
    }
    trace_pmu_close( &tried );
    if( may_idle || host->refusal_rank >= TRACE_PMU_RANK_READ_ZERO ) continue;
    host->refusal_rank = TRACE_PMU_RANK_READ_ZERO;
    snprintf( host->refusal, sizeof( host->refusal ), TRACE_PMU_READ_ZERO );
  }
  return kept->fd >= 0;
}

/* Open one event per metric, in priority order. Returns the number kept, or -ENOMEM. When
   it is none, the ring's counter_status says why: the refusal of an event that opened, if
   there was one, else that those which opened read zero, else the first failure to open. */
static inline int trace_pmu_open_all( trace_ring_t * ring ) {
  trace_pmu_host_t host;
  uint8_t *        buffer = (uint8_t *)mmap( NULL, TRACE_PMU_TEST_BYTES, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
  int              kept_cnt = 0, is_proven;

  trace_pmu_host( &host );
  if( buffer == MAP_FAILED ) return -ENOMEM;
  /* No huge pages: with 4 KiB pages the buffer is 4096 of them and the TLB must miss. With
     64 KiB pages it is 256, which a TLB holds: 519 walks in 10 million reads on a Neoverse
     V2, so dtlb_walk may then idle (TRACE_EVENTS_MAY_IDLE_BIG_PAGES). */
  madvise( buffer, TRACE_PMU_TEST_BYTES, MADV_NOHUGEPAGE );
  memset( buffer, 1, TRACE_PMU_TEST_BYTES );
  for( int i = 0; i < TRACE_METRIC_CNT && kept_cnt < TRACE_COUNTER_MAX; i++ ) {
    trace_pmu_counter_t * counter = &trace_pmu_opened[ kept_cnt ];
    if( !trace_pmu_pick( &trace_metric[ i ], &host, buffer, counter, &is_proven ) ) continue;
    trace_pmu_thread.page[ kept_cnt ] = counter->page;
    snprintf( ring->counter[ kept_cnt ].name, TRACE_NAME_BYTES, "%s", trace_metric[ i ].name );
    snprintf( ring->counter[ kept_cnt ].chosen, TRACE_CHOSEN_BYTES, "%s%s", counter->chosen,
              is_proven ? "" : TRACE_PMU_UNPROVEN );
    kept_cnt++;
  }
  munmap( buffer, TRACE_PMU_TEST_BYTES );
  if( !kept_cnt )
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "%s",
              host.refusal[ 0 ] ? host.refusal : TRACE_PMU_NO_EVENT );
  return kept_cnt;
}

/* Cost in nanoseconds, rounded, of one read of this counter by this thread: a batch of reads
   timed as one interval, the median of a few such batches. One read timed alone carries the
   cost of a timestamp, as much as the read itself where reads do not trap, and cannot be
   resolved where the timestamp counter steps by 10 ns (Zen: every value a multiple of 27.5
   ticks at 2.75 GHz). The median sets aside a batch that an interrupt fell into. */
static inline uint64_t trace_pmu_read_cost_ns( struct perf_event_mmap_page const * page,
                                               uint64_t tsc_hz ) {
  uint64_t cost_tsc[ TRACE_PMU_COST_BATCHES ], batch_hz = tsc_hz * TRACE_PMU_COST_READS;
  uint64_t sum = 0UL;
  int      is_live;
  for( int batch = 0; batch < TRACE_PMU_COST_BATCHES; batch++ ) {
    uint64_t start_tsc = trace_tsc();
    for( int i = 0; i < TRACE_PMU_COST_READS; i++ ) sum += trace_pmu_read( page, &is_live );
    cost_tsc[ batch ] = trace_tsc() - start_tsc;
  }
  __asm__ volatile( "" : : "r"( sum ) );  /* unused, the reads could be dropped */
  return ( trace_pmu_median( cost_tsc, TRACE_PMU_COST_BATCHES ) * TRACE_NS_PER_SECOND +
           batch_hz / 2UL ) / batch_hz;
}

/* 1 when the host forbids reading counters from user space and has a file that says so
   (aarch64): every event would then open and be found unreadable, one after the other. */
static inline int trace_pmu_is_user_access_off( void ) {
  char const * path = TRACE_ARCH_USER_ACCESS_FILE;
  char         text[ TRACE_PMU_LINE_BYTES ] = "";
  if( !path[ 0 ] || trace_pmu_file_line( path, text, sizeof( text ) ) ) return 0;
  return text[ 0 ] == '0';
}

/* 1 when a `tracerd --pmu-hold` is running: it keeps an exclusive lock on the hold file, so
   a shared lock cannot be had. A file left behind by a dead holder is not locked. */
static inline int trace_pmu_is_held( void ) {
  int fd = open( TRACE_PMU_HOLD_FILE, O_RDONLY | O_CLOEXEC ), is_held;
  if( fd < 0 ) return 0;
  is_held = flock( fd, LOCK_SH | LOCK_NB ) && errno == EWOULDBLOCK;
  close( fd );  /* drops the shared lock if it was granted */
  return is_held;
}

/* TRACE_PMU_PERIOD_US as nanoseconds, or `period_ns` after one line when it is no number
   or outside what the sampler accepts. */
static inline uint64_t trace_pmu_period_ns( char const * text, uint64_t period_ns ) {
  char *   end;
  uint64_t wanted_us = strtoull( text, &end, 10 );
  if( end != text && !*end && wanted_us >= TRACE_PMU_PERIOD_MIN_NS / TRACE_NS_PER_US &&
      wanted_us <= TRACE_PMU_PERIOD_MAX_NS / TRACE_NS_PER_US ) return wanted_us * TRACE_NS_PER_US;
  fprintf( stderr, "trace: TRACE_PMU_PERIOD_US=%s is not a count from %lu to %lu, ignored\n", text,
           TRACE_PMU_PERIOD_MIN_NS / TRACE_NS_PER_US, TRACE_PMU_PERIOD_MAX_NS / TRACE_NS_PER_US );
  return period_ns;
}

/* Give the calling thread its counters. Call after trace_attach, before any hot loop.
   Does nothing unless TRACE_PMU is 1 or force. Refuses when the host's setting forbids
   reading counters from user space (aarch64: kernel.perf_user_access is 0), before any event
   is opened. Refuses when no `tracerd --pmu-hold` runs: opening the host's first perf event
   sends IPIs to every CPU (force overrides this). TRACE_PMU_PERIOD_US overrides the
   sampling period, within 1 ms to 1 s. Returns the number of counters. */
static inline int trace_pmu_attach( void ) {
  trace_writer_t * writer = &trace_writer;
  trace_ring_t *   ring   = writer->ring;
  char const *     wanted = getenv( "TRACE_PMU" ), * period_text = getenv( "TRACE_PMU_PERIOD_US" );
  uint64_t         tsc_hz, read_ns, period_ns;
  int              counter_cnt;

  if( !ring || !wanted || !strcmp( wanted, "0" ) ) return 0;
  if( strcmp( wanted, "1" ) && strcmp( wanted, "force" ) ) {
    fprintf( stderr, "trace: TRACE_PMU=%s is neither 1 nor force: no counters\n", wanted );
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "TRACE_PMU is neither 1 nor force" );
    return 0;
  }
  if( writer->sampler == trace_pmu_sampler ) return trace_pmu_thread.counter_cnt;  /* again */
  memset( &trace_pmu_thread, 0, sizeof( trace_pmu_thread ) );  /* a forked child starts afresh */
  if( trace_pmu_is_user_access_off() ) {
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, TRACE_PMU_USER_ACCESS_OFF );
    return 0;
  }
  if( strcmp( wanted, "force" ) && !trace_pmu_is_held() ) {
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "no tracerd --pmu-hold is running" );
    return 0;
  }
  snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "counters are being set up" );
  tsc_hz = trace_attach_tsc_hz();  /* measured once for the process: 50 ms of spin on x86 */
  if( !tsc_hz ) {  /* firmware that does not say: no time base to pace the reads with */
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "the clock's frequency is not known" );
    return 0;
  }
  counter_cnt = trace_pmu_open_all( ring );
  if( counter_cnt < 0 )
    snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "no memory for the counters' self-test" );
  if( counter_cnt <= 0 ) return 0;
  trace_pmu_thread.counter_cnt = counter_cnt;
  trace_pmu_thread.next_page   = trace_pmu_thread.page[ 0 ];
  read_ns   = trace_pmu_read_cost_ns( trace_pmu_thread.page[ 0 ], tsc_hz );
  period_ns = read_ns * TRACE_PMU_COST_FACTOR;
  if( period_text ) period_ns = trace_pmu_period_ns( period_text, period_ns );
  if( period_ns < TRACE_PMU_PERIOD_MIN_NS ) period_ns = TRACE_PMU_PERIOD_MIN_NS;
  if( period_ns > TRACE_PMU_PERIOD_MAX_NS ) period_ns = TRACE_PMU_PERIOD_MAX_NS;

  ring->counter_read_ns   = (uint32_t)read_ns;
  ring->counter_period_us = (uint32_t)( period_ns / TRACE_NS_PER_US );
  snprintf( ring->counter_status, TRACE_CHOSEN_BYTES, "on" );
  __atomic_store_n( &ring->counter_cnt, (uint32_t)trace_pmu_thread.counter_cnt, __ATOMIC_RELEASE );

  trace_pmu_thread.period_tsc = period_ns * tsc_hz / TRACE_NS_PER_SECOND;
  trace_pmu_thread.next_tsc   = trace_tsc() + trace_pmu_thread.period_tsc;
  writer->sampler             = trace_pmu_sampler;
  if( trace_pmu_thread.next_tsc < writer->sample_next_tsc )
    writer->sample_next_tsc = trace_pmu_thread.next_tsc;
  return trace_pmu_thread.counter_cnt;
}

#endif /* TRACE_PMU_H */
