/* trace_attach.c: the cold half of the tracer. Creates the calling thread's ring in shared
   memory and points the thread's writer at it. Runs before the hot loops; nothing is ever
   unmapped, because unmapping would send IPIs to the other threads of the process. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "trace.h"

#define TRACE_ATTACH_PATH_BYTES (256)
#define TRACE_ATTACH_TEMP_BYTES ( TRACE_ATTACH_PATH_BYTES + sizeof( TRACE_RING_TEMP ) )
#define TRACE_ATTACH_RING_MAX   (64)  /* rings whose file is removed when the process exits */
#define TRACE_ATTACH_MODE       (0600)
#define TRACE_ATTACH_STAT_BYTES (1024)

/* The slot of the unattached. Nothing reads it and every unattached thread writes it, event
   and cursor both: on one cache line, so that hot threads pass one line between their cores
   and not two. Measured per event: 13 ns with 2 such threads, 25 ns with 4; on two lines
   21 to 29 ns and 52 to 55 ns. */
static struct {
  trace_event_t event;
  uint64_t      cursor;
} trace_attach_no_ring __attribute__(( aligned( 64 ) ));

/* The slot of a thread whose attach failed: its own, see trace_attach_failed. */
static __thread trace_event_t trace_attach_own_event  __attribute__(( aligned( 64 ) ));
static __thread uint64_t      trace_attach_own_cursor __attribute__(( aligned( 64 ) ));

/* A writer with no ring: every event lands in the one slot above, and no counter is read. */
#define TRACE_ATTACH_NO_RING { .mask            = 0UL,                          \
                               .event           = &trace_attach_no_ring.event,  \
                               .cursor_shared   = &trace_attach_no_ring.cursor, \
                               .sample_next_tsc = UINT64_MAX }

__thread trace_writer_t trace_writer
  __attribute__(( tls_model( "initial-exec" ), aligned( 64 ) )) = TRACE_ATTACH_NO_RING;

/* Ticks per second of trace_tsc, measured once for the process: on x86 the measure is a
   50 ms spin, which each thread asking for counters would otherwise repeat. Two threads that
   ask at the same instant both measure. 0 when the firmware does not say (aarch64). */
uint64_t trace_attach_tsc_hz( void );  /* its one caller, trace_pmu.h, declares it as well */

uint64_t trace_attach_tsc_hz( void ) {
  static uint64_t known_hz;
  uint64_t        hz = __atomic_load_n( &known_hz, __ATOMIC_ACQUIRE );
  if( hz ) return hz;
  hz = trace_tsc_hz();
  __atomic_store_n( &known_hz, hz, __ATOMIC_RELEASE );
  return hz;
}

/* What trace_now calls on a thread with a ring and no counters: once per period, the lines
   tracerd took at its drains are asked back, before an event has to wait for them. */
static void trace_attach_own_back( trace_writer_t * writer, uint64_t tsc ) {
  writer->sample_next_tsc = trace_own_back( writer, tsc );
}

static char  trace_attach_path[ TRACE_ATTACH_RING_MAX ][ TRACE_ATTACH_PATH_BYTES ];
static int   trace_attach_path_cnt;
static pid_t trace_attach_pid;  /* the process that created the rings */

/* The files go when the process exits normally; tracerd keeps its mapping and the data.
   After a crash they stay, and tracerd removes them once it sees the thread is gone. A
   forked child that exits must leave its parent's rings alone. */
static void trace_attach_unlink_all( void ) {
  int path_cnt = __atomic_load_n( &trace_attach_path_cnt, __ATOMIC_ACQUIRE );
  if( getpid() != trace_attach_pid ) return;
  if( path_cnt > TRACE_ATTACH_RING_MAX ) path_cnt = TRACE_ATTACH_RING_MAX;
  for( int i = 0; i < path_cnt; i++ ) unlink( trace_attach_path[ i ] );
}

/* After a fork the child holds the parent's ring mapped, with a cursor of its own: it must
   not write there. It goes back to the ring of the unattached. */
static void trace_attach_in_child( void ) {
  trace_writer          = (trace_writer_t)TRACE_ATTACH_NO_RING;
  trace_attach_path_cnt = 0;  /* the rings to remove at exit are the parent's, not its own */
}

static void trace_attach_remember( char const * path ) {
  int path_idx = __atomic_fetch_add( &trace_attach_path_cnt, 1, __ATOMIC_ACQ_REL );
  if( path_idx == TRACE_ATTACH_RING_MAX )
    fprintf( stderr, "trace: more than %d rings: the later ones stay for tracerd to remove\n",
             TRACE_ATTACH_RING_MAX );
  if( path_idx >= TRACE_ATTACH_RING_MAX ) return;
  snprintf( trace_attach_path[ path_idx ], TRACE_ATTACH_PATH_BYTES, "%s", path );
  if( path_idx ) return;
  trace_attach_pid = getpid();
  atexit( trace_attach_unlink_all );
  pthread_atfork( NULL, NULL, trace_attach_in_child );
}

static uint64_t trace_attach_event_cnt( void ) {
  char const * text      = getenv( "TRACE_RING_EVENTS" );
  char *       end       = NULL;
  uint64_t     wanted    = text ? strtoull( text, &end, 10 ) : TRACE_RING_EVENTS;
  uint64_t     event_cnt = TRACE_RING_EVENTS_MIN;
  if( text && ( end == text || *end || !wanted || wanted > TRACE_RING_EVENTS_MAX ) ) {
    fprintf( stderr, "trace: TRACE_RING_EVENTS=%s is not a count from 1 to %lu, using %lu\n", text,
             (unsigned long)TRACE_RING_EVENTS_MAX, (unsigned long)TRACE_RING_EVENTS );
    wanted = TRACE_RING_EVENTS;
  }
  while( event_cnt < wanted && event_cnt < TRACE_RING_EVENTS_MAX ) event_cnt <<= 1;
  return event_cnt;
}

/* When the calling thread started, in clock ticks since boot, or 0 when /proc does not say. */
static uint64_t trace_attach_start_ticks( void ) {
  char          text[ TRACE_ATTACH_STAT_BYTES ], state;
  char const *  after_name;
  unsigned long start_ticks = 0UL;
  int           fd          = open( TRACE_STAT_FILE, O_RDONLY | O_CLOEXEC );
  ssize_t       byte_cnt    = fd < 0 ? -1 : read( fd, text, sizeof( text ) - 1UL );
  if( fd >= 0 ) close( fd );
  if( byte_cnt <= 0 ) return 0UL;
  text[ byte_cnt ] = '\0';
  after_name = strrchr( text, ')' );  /* the command name may hold spaces */
  if( after_name ) sscanf( after_name + 1, TRACE_STAT_FORMAT, &state, &start_ticks );
  return start_ticks;
}

static void trace_attach_header( trace_ring_t * ring, trace_site_t const * site, int site_cnt,
                                 char const * thread_name, uint64_t header_bytes,
                                 uint64_t event_cnt ) {
  struct stat  status;
  char const * name = thread_name ? thread_name : "thread";
  ring->pid_namespace  = stat( TRACE_NAMESPACE_FILE, &status ) ? 0UL : (uint64_t)status.st_ino;
  ring->time_namespace = stat( TRACE_TIME_FILE, &status )      ? 0UL : (uint64_t)status.st_ino;
  ring->start_ticks    = trace_attach_start_ticks();
  ring->header_bytes   = (uint32_t)header_bytes;
  ring->pid            = (uint32_t)getpid();
  ring->tid            = (uint32_t)syscall( SYS_gettid );
  ring->site_cnt       = (uint32_t)site_cnt;
  ring->event_cnt      = event_cnt;
  snprintf( ring->thread_name, sizeof( ring->thread_name ), "%s", name );
  snprintf( ring->counter_status, sizeof( ring->counter_status ),
            "the program did not ask for counters" );
  for( int i = 0; i < site_cnt; i++ ) {
    name = site[ i ].name ? site[ i ].name : "?";
    ring->site[ i ].flags = site[ i ].flags;
    snprintf( ring->site[ i ].name, sizeof( ring->site[ i ].name ), "%s", name );
  }
}

/* Create the ring file under its temporary name and map it. The final name appears only
   once the header is complete, and replaces a stale file instead of truncating memory
   someone may still have mapped. Returns the mapping, or NULL with *err set. */
static trace_ring_t * trace_attach_map( char const * temp_path, uint64_t * ring_bytes, int * err ) {
  struct statfs  filesystem;
  trace_ring_t * ring;
  int            fd;

  unlink( temp_path );
  fd = open( temp_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, TRACE_ATTACH_MODE );
  if( fd < 0 ) { *err = -errno; return NULL; }
  if( !fstatfs( fd, &filesystem ) && filesystem.f_bsize > 0 ) {  /* hugetlbfs: whole huge pages */
    uint64_t block_bytes = (uint64_t)filesystem.f_bsize;
    *ring_bytes = ( *ring_bytes + block_bytes - 1UL ) / block_bytes * block_bytes;
    /* On a disk the kernel writes the ring's dirty pages back, and a store can wait for it. */
    if( filesystem.f_type != TMPFS_MAGIC && filesystem.f_type != HUGETLBFS_MAGIC )
      fprintf( stderr, "trace: %s is neither on tmpfs nor on hugetlbfs: events may stall\n",
               temp_path );
  }
  /* Reserve the space now: a full filesystem must fail here, not at the first touch. */
  ring  = (trace_ring_t *)MAP_FAILED;
  errno = posix_fallocate( fd, 0, (off_t)*ring_bytes );
  if( errno == EOPNOTSUPP && !ftruncate( fd, (off_t)*ring_bytes ) ) errno = 0;
  if( !errno )
    ring = (trace_ring_t *)mmap( NULL, *ring_bytes, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_POPULATE, fd, 0 );
  *err = ring == MAP_FAILED ? -errno : 0;
  close( fd );
  if( *err ) { unlink( temp_path ); return NULL; }
  memset( ring, 0, *ring_bytes );  /* write-touches every page now, not in the hot loop */
  ring->is_locked = !mlock( ring, *ring_bytes );
  return ring;
}

/* A thread whose attach failed writes to a slot of its own: two hot threads writing the
   one shared slot would pass its cache line back and forth on every event. */
static int trace_attach_failed( int err ) {
  trace_writer.event         = &trace_attach_own_event;
  trace_writer.cursor_shared = &trace_attach_own_cursor;
  return err;
}

int trace_attach( trace_site_t const * site, int site_cnt, char const * thread_name ) {
  trace_writer_t * writer       = &trace_writer;
  char const *     dir          = getenv( "TRACE_RING_DIR" );
  uint64_t         page_bytes   = (uint64_t)sysconf( _SC_PAGESIZE );
  uint64_t         header_pages = ( sizeof( trace_ring_t ) + page_bytes - 1UL ) / page_bytes;
  uint64_t         header_bytes = header_pages * page_bytes;
  uint64_t         event_cnt, ring_bytes;
  char             path[ TRACE_ATTACH_PATH_BYTES ], temp_path[ TRACE_ATTACH_TEMP_BYTES ];
  trace_ring_t *   ring;
  int              err = 0, path_bytes;

  if( writer->ring ) return 0;
  if( site_cnt < 0 || site_cnt > TRACE_SITE_MAX ) {
    fprintf( stderr, "trace: %d sites, at most %d are supported\n", site_cnt, TRACE_SITE_MAX );
    return trace_attach_failed( -EINVAL );
  }
  path_bytes = snprintf( path, sizeof( path ), "%s/" TRACE_RING_PREFIX "%d.%ld",
                         dir ? dir : TRACE_RING_DIR, (int)getpid(), syscall( SYS_gettid ) );
  if( path_bytes >= (int)sizeof( path ) ) {
    fprintf( stderr, "trace: the ring's path is longer than %d bytes\n",
             TRACE_ATTACH_PATH_BYTES - 1 );
    return trace_attach_failed( -ENAMETOOLONG );
  }
  snprintf( temp_path, sizeof( temp_path ), "%s" TRACE_RING_TEMP, path );
  event_cnt  = trace_attach_event_cnt();
  ring_bytes = header_bytes + event_cnt * sizeof( trace_event_t );
  ring = trace_attach_map( temp_path, &ring_bytes, &err );
  if( !ring ) {
    fprintf( stderr, "trace: creating %s: %s\n", path, strerror( -err ) );
    return trace_attach_failed( err );
  }

  trace_attach_header( ring, site, site_cnt, thread_name, header_bytes, event_cnt );
  __atomic_store_n( &ring->magic, TRACE_RING_MAGIC, __ATOMIC_RELEASE );  /* header complete */
  if( rename( temp_path, path ) ) {
    err = -errno;
    fprintf( stderr, "trace: naming %s: %s\n", path, strerror( -err ) );
    unlink( temp_path );
    return trace_attach_failed( err );  /* the mapping stays: this file never unmaps */
  }
  trace_attach_remember( path );

  writer->event         = (trace_event_t *)( (char *)ring + header_bytes );
  writer->cursor_shared = &ring->cursor;
  writer->mask          = event_cnt - 1UL;
  writer->cursor        = 0UL;
  writer->ring          = ring;
  writer->sampler         = trace_attach_own_back;
  writer->sample_next_tsc = trace_own_back( writer, trace_tsc() );
  return 0;
}
