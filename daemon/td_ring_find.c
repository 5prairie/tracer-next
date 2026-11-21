/* td_ring_find.c: finds the rings of traced threads and follows their lives. Maps and
   checks each ring file, which is not trusted; notes where its thread runs; sees it exit,
   and then removes the file. td_ring.c drains what is found here. */
#define _GNU_SOURCE
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "td_source.h"

#define TD_RING_STAT_BYTES        (1024)
#define TD_RING_SELF_LINK         "/proc/self"  /* links to the reader's pid in /proc's namespace */
#define TD_RING_PROCESS_DIR       "/proc/%u"
#define TD_RING_IDS_FORMAT        "%u.%u"       /* follows TRACE_RING_PREFIX in a file's name */
#define TD_RING_DEAD              (-2)          /* td_ring_thread_cpu: the thread has exited */
#define TD_RING_UNKNOWN           (-3)          /* ...or /proc did not say */
#define TD_RING_TEMP_KEEP_SECONDS (60L)         /* a ring unnamed for this long is debris */
#define TD_RING_STAT_FRESH_NS     (100UL * TD_NS_PER_MS)  /* a stat this recent is not read again */

/* A ring file as found, and what its header said when it was checked. The header is read
   once, into this: shared memory can change, and what was checked is what must be used. */
typedef struct {
  uint64_t          file_bytes, inode, device, header_bytes, event_cnt, start_ticks, cursor;
  uint32_t          user_id, pid, tid, site_cnt, is_locked;
  char              name[ TRACE_NAME_BYTES ];
  trace_site_name_t site[ TD_SITE_MAX ];
} td_ring_file_t;

int td_ring_unfollowed_cnt;

static uint64_t td_ring_pid_namespace;   /* this daemon's; 0 when /proc does not tell */
static uint64_t td_ring_time_namespace;  /* 0 when the kernel has none */
static int      td_ring_is_set_up;
static int      td_ring_keeps_files;
static int      td_ring_is_changed;      /* a look inside a discover saw a thread go */

/* A thread's stat, status and schedstat files, open for as long as the thread is followed:
   a read is one pread, and the fd is that thread's whatever its ids come to name. And its
   stat as last read and parsed, when, and what the read returned: the look that finds
   where the thread runs and the faults source share one read of a tick. */
static char const * const td_ring_task_leaf[ TD_TASK_LEAF_CNT ] = {
  [ TD_TASK_LEAF_STAT ]      = TD_TASK_STAT,
  [ TD_TASK_LEAF_STATUS ]    = TD_TASK_STATUS,
  [ TD_TASK_LEAF_SCHEDSTAT ] = TD_TASK_SCHEDSTAT,
};
static td_kept_t      td_ring_task_kept[ TD_THREAD_MAX ][ TD_TASK_LEAF_CNT ];
static td_task_stat_t td_ring_stat_last[ TD_THREAD_MAX ];
static uint64_t       td_ring_stat_ns  [ TD_THREAD_MAX ];  /* 0: not read yet */
static int            td_ring_stat_err [ TD_THREAD_MAX ];

/* The mapping being checked, not yet a thread's: a fault there is survived as well. */
static char const * volatile td_ring_trial_start;
static uint64_t volatile     td_ring_trial_bytes;
static volatile sig_atomic_t td_ring_trial_is_cut;

void td_ring_keep_files( void ) { td_ring_keeps_files = 1; }

/* Put zeroes where a ring was mapped. Returns 1 when `address` was within it. */
static int td_ring_blank( char const * address, char const * start, uint64_t byte_cnt ) {
  if( !start || address < start || address >= start + byte_cnt ) return 0;
  return mmap( (void *)start, byte_cnt, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
               0 ) != MAP_FAILED;
}

/* SIGBUS: a ring file was cut short under its mapping, by its owner or by anyone who may
   write it. The read is given zeroes to finish on, and the ring is dropped at the next
   look. A fault anywhere else is not survived; a SIGBUS someone sent is ignored. */
static void td_ring_on_fault( int signal_number, siginfo_t * info, void * context ) {
  char const * address     = (char const *)info->si_addr;
  int          saved_errno = errno;
  (void)context;
  if( info->si_code <= 0 ) return;  /* from kill(): there is no faulting address */
  if( td_ring_blank( address, td_ring_trial_start, td_ring_trial_bytes ) ) {
    td_ring_trial_is_cut = 1;
    errno = saved_errno;
    return;
  }
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t * thread = &td_thread[ i ];
    if( !thread->is_live ) continue;
    if( !td_ring_blank( address, (char const *)thread->ring, thread->ring_bytes ) ) continue;
    thread->is_cut = 1;
    errno = saved_errno;
    return;
  }
  signal( signal_number, SIG_DFL );  /* the read faults again, and this time ends tracerd */
}

/* Once, at the first look: survive cut files, and learn this daemon's namespaces. Pids
   are looked up in /proc: when that is another pid namespace's (its "self" is not this
   pid), a ring's thread would be taken for dead and its file removed. Then no ring is
   followed. */
static void td_ring_setup( void ) {
  struct sigaction action;
  struct stat      status;
  char             self[ TD_TASK_PATH_BYTES ];
  ssize_t          self_bytes = readlink( TD_RING_SELF_LINK, self, sizeof( self ) - 1UL );

  td_ring_is_set_up = 1;
  memset( &action, 0, sizeof( action ) );
  action.sa_sigaction = td_ring_on_fault;
  action.sa_flags     = SA_SIGINFO;
  sigaction( SIGBUS, &action, NULL );
  if( self_bytes > 0 ) self[ self_bytes ] = '\0';
  if( self_bytes > 0 && atol( self ) == (long)getpid() && !stat( TRACE_NAMESPACE_FILE, &status ) )
    td_ring_pid_namespace = (uint64_t)status.st_ino;
  if( !stat( TRACE_TIME_FILE, &status ) ) td_ring_time_namespace = (uint64_t)status.st_ino;
  if( !td_ring_pid_namespace )
    fprintf( stderr, "tracerd: /proc is not that of this pid namespace: no ring is followed\n" );
}

static td_thread_t * td_ring_find( char const * path ) {
  for( int i = 0; i < TD_THREAD_MAX; i++ )
    if( td_thread[ i ].is_live && !strcmp( td_thread[ i ].path, path ) ) return &td_thread[ i ];
  return NULL;
}

/* A free slot; or the slot of a gone thread whose ring was this very file of this very
   thread (cut short and restored, it comes back where it was); or NULL. */
static td_thread_t * td_ring_slot_free( td_ring_file_t const * file ) {
  td_thread_t * free_slot = NULL;
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t * thread = &td_thread[ i ];
    if( !thread->is_live ) { if( !free_slot ) free_slot = thread; continue; }
    if( !thread->is_gone || thread->inode != file->inode ) continue;
    if( thread->device != file->device || thread->pid != file->pid ) continue;
    if( thread->tid != file->tid ) continue;
    if( thread->start_ticks != file->start_ticks ) continue;  /* an inode number used again */
    munmap( (void *)thread->ring, thread->ring_bytes );
    thread->is_live = 0;
    return thread;
  }
  return free_slot;
}

/* The slot of the thread that has been gone longest, freed; or NULL when none is gone. A
   gone thread whose file could not be removed is the last to give up its slot: freed, its
   file would be found again as a new ring. */
static td_thread_t * td_ring_slot_evict( void ) {
  td_thread_t * oldest = NULL;
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t * thread = &td_thread[ i ];
    int           is_older;
    if( !thread->is_live || !thread->is_gone ) continue;
    is_older = !oldest || ( !thread->path[ 0 ] && oldest->path[ 0 ] ) ||
               ( !thread->path[ 0 ] == !oldest->path[ 0 ] && thread->gone_ns < oldest->gone_ns );
    if( is_older ) oldest = thread;
  }
  if( !oldest ) return NULL;
  munmap( (void *)oldest->ring, oldest->ring_bytes );
  oldest->is_live = 0;
  return oldest;
}

/* Close the slot's /proc files and forget its stat: its thread is gone, or another thread
   takes the slot. */
static void td_ring_task_close( int thread_idx ) {
  for( int leaf_idx = 0; leaf_idx < TD_TASK_LEAF_CNT; leaf_idx++ )
    td_kept_close( &td_ring_task_kept[ thread_idx ][ leaf_idx ] );
  td_ring_stat_ns[ thread_idx ] = 0UL;
}

long td_ring_task_read( int thread_idx, int leaf_idx, char * text, size_t text_bytes ) {
  td_thread_t const * thread = &td_thread[ thread_idx ];
  td_kept_t *         kept   = &td_ring_task_kept[ thread_idx ][ leaf_idx ];
  char                path[ TD_TASK_PATH_BYTES ] = "";
  if( !kept->fd_plus1 )  /* the path is wanted only to open the file */
    snprintf( path, sizeof( path ), TD_TASK_FILE, thread->pid, thread->tid,
              td_ring_task_leaf[ leaf_idx ] );
  return td_kept_read( kept, path, text, text_bytes, 1 );
}

int td_ring_task_stat( int thread_idx, uint64_t since_ns, td_task_stat_t * stat ) {
  char text[ TD_RING_STAT_BYTES ];
  if( !td_ring_stat_ns[ thread_idx ] || td_ring_stat_ns[ thread_idx ] < since_ns ) {
    long byte_cnt = td_ring_task_read( thread_idx, TD_TASK_LEAF_STAT, text, sizeof( text ) );
    td_ring_stat_err[ thread_idx ] = byte_cnt < 0L
                                     ? (int)byte_cnt
                                     : td_task_stat_parse( text, &td_ring_stat_last[ thread_idx ] );
    td_ring_stat_ns [ thread_idx ] = td_clock_ns();
  }
  *stat = td_ring_stat_last[ thread_idx ];
  return td_ring_stat_err[ thread_idx ];
}

/* Where a thread runs, from a read of its stat file (err is what the read returned): its
   CPU; or TD_RING_DEAD when it has exited (no such thread, a zombie, or a later thread
   under ids used again: another start time); or TD_RING_UNKNOWN when /proc did not say
   just now. */
static int td_ring_stat_cpu( int err, td_task_stat_t const * stat, uint64_t start_ticks ) {
  if( err == -ENOENT || err == -ESRCH ) return TD_RING_DEAD;
  if( err ) return TD_RING_UNKNOWN;  /* a passing error says nothing of the thread */
  if( stat->state == 'Z' || stat->state == 'X' ) return TD_RING_DEAD;
  if( start_ticks && stat->start_ticks != start_ticks ) return TD_RING_DEAD;
  return stat->cpu >= 0 ? stat->cpu : TD_RING_UNKNOWN;
}

/* The same for a thread that has no slot yet, by its ids. */
static int td_ring_thread_cpu( uint32_t pid, uint32_t tid, uint64_t start_ticks ) {
  td_task_stat_t stat     = { 0 };
  char           text[ TD_RING_STAT_BYTES ];
  long           byte_cnt = td_task_read( pid, tid, TD_TASK_STAT, text, sizeof( text ) );
  int            err      = byte_cnt < 0L ? (int)byte_cnt : td_task_stat_parse( text, &stat );
  return td_ring_stat_cpu( err, &stat, start_ticks );
}

/* Read the header once and say whether this tracerd reads it: its magic, its namespaces,
   sizes that fit the file. Says why not, once per reason; a header not written yet is not
   worth a word, it is tried again. */
static int td_ring_header_read( char const * path, trace_ring_t const * ring,
                                td_ring_file_t * file ) {
  uint64_t magic = __atomic_load_n( &ring->magic, __ATOMIC_ACQUIRE );
  int      is_sized, is_here;
  file->event_cnt    = ring->event_cnt;
  file->header_bytes = ring->header_bytes;
  file->start_ticks  = ring->start_ticks;
  file->pid          = ring->pid;
  file->tid          = ring->tid;
  file->site_cnt     = ring->site_cnt;
  file->is_locked    = ring->is_locked;
  file->cursor       = __atomic_load_n( &ring->cursor, __ATOMIC_ACQUIRE );
  memcpy( file->name, ring->thread_name, sizeof( file->name ) );
  memcpy( file->site, ring->site, sizeof( file->site ) );
  file->name[ sizeof( file->name ) - 1UL ] = '\0';
  is_sized = file->event_cnt >= TRACE_RING_EVENTS_MIN && file->event_cnt <= TRACE_RING_EVENTS_MAX &&
             !( file->event_cnt & ( file->event_cnt - 1UL ) ) &&
             file->header_bytes >= sizeof( trace_ring_t ) &&
             !( file->header_bytes % sizeof( trace_event_t ) ) &&
             file->header_bytes + file->event_cnt * sizeof( trace_event_t ) <= file->file_bytes;
  is_here  = ring->pid_namespace == td_ring_pid_namespace &&
             ring->time_namespace == td_ring_time_namespace;
  if( !magic || !td_ring_pid_namespace ) return 0;
  if( magic != TRACE_RING_MAGIC || !is_sized ) {
    TD_LOG_ONCE( "tracerd: %s has a header this tracerd does not read (the first such file)\n",
                 path );
    return 0;
  }
  if( !is_here )
    TD_LOG_ONCE( "tracerd: %s is of another pid or time namespace, or of one it could not "
                 "tell: ignored (the first such file)\n", path );
  return is_here;
}

/* Map one ring read-only and read its header. A ring file is not trusted: it must be a
   regular file of this user (any user, for root) with a sane header. Returns the mapping,
   or NULL. */
static trace_ring_t const * td_ring_map( char const * path, td_ring_file_t * file ) {
  struct stat          status;
  trace_ring_t const * ring = (trace_ring_t const *)MAP_FAILED;
  int                  fd   = open( path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK );
  int                  is_ours, is_sane;

  if( fd < 0 ) return NULL;
  is_ours = !fstat( fd, &status ) && S_ISREG( status.st_mode ) &&
            ( !geteuid() || status.st_uid == geteuid() );  /* root follows every user's rings */
  if( !is_ours ) {
    TD_LOG_ONCE( "tracerd: %s is not a ring file of this user, ignored (the first such file)\n",
                 path );
    close( fd );
    return NULL;
  }
  file->file_bytes = (uint64_t)status.st_size;
  file->inode      = (uint64_t)status.st_ino;
  file->device     = (uint64_t)status.st_dev;
  file->user_id    = (uint32_t)status.st_uid;
  if( file->file_bytes >= sizeof( trace_ring_t ) )
    ring = (trace_ring_t const *)mmap( NULL, file->file_bytes, PROT_READ, MAP_SHARED, fd, 0 );
  close( fd );
  if( ring == MAP_FAILED ) return NULL;
  td_ring_trial_bytes  = file->file_bytes;
  td_ring_trial_is_cut = 0;
  td_ring_trial_start  = (char const *)ring;  /* from here a cut file gives zeroes, no signal */
  is_sane = td_ring_header_read( path, ring, file ) && !td_ring_trial_is_cut;
  td_ring_trial_start  = NULL;                /* nothing reads the mapping until it is a thread's */
  if( is_sane ) return ring;
  munmap( (void *)ring, file->file_bytes );
  return NULL;
}

/* Is the file named after the ids of its header, exactly? And, for root, is the process
   its owner's? Otherwise any user could have root's tracerd read and show another's
   thread. Returns NULL, or what is wrong. */
static char const * td_ring_name_fault( char const * ids, td_ring_file_t const * file ) {
  struct stat status;
  char        process_dir[ TD_TASK_PATH_BYTES ], named[ TD_TASK_PATH_BYTES ];
  snprintf( named, sizeof( named ), TD_RING_IDS_FORMAT, file->pid, file->tid );
  if( strcmp( named, ids ) ) return "is not named after the thread of its header";
  snprintf( process_dir, sizeof( process_dir ), TD_RING_PROCESS_DIR, file->pid );
  if( geteuid() || stat( process_dir, &status ) || (uint32_t)status.st_uid == file->user_id )
    return NULL;
  return "is not its process's user's (it changed user after attaching)";
}

/* Is this file, by another path, already a thread's ring? "--ring-dir /dev/shm/" is one. */
static int td_ring_is_followed( td_ring_file_t const * file ) {
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t const * thread = &td_thread[ i ];
    if( thread->is_live && !thread->is_gone && thread->inode == file->inode &&
        thread->device == file->device ) return 1;
  }
  return 0;
}

/* Make the slot the ring's thread, from what the header said when it was checked. */
static void td_ring_fill( td_thread_t * thread, char const * path, trace_ring_t const * ring,
                          td_ring_file_t const * file ) {
  td_ring_task_close( (int)( thread - td_thread ) );  /* the slot's former thread's, if any */
  memset( thread, 0, sizeof( *thread ) );
  snprintf( thread->path, sizeof( thread->path ), "%s", path );
  memcpy( thread->name, file->name, sizeof( thread->name ) );
  memcpy( thread->site, file->site, sizeof( thread->site ) );
  thread->ring        = ring;
  thread->event       = (trace_event_t const *)( (char const *)ring + file->header_bytes );
  thread->ring_bytes  = file->file_bytes;
  thread->inode       = file->inode;
  thread->device      = file->device;
  thread->pid         = file->pid;
  thread->tid         = file->tid;
  thread->start_ticks = file->start_ticks;
  thread->is_locked   = (int)file->is_locked;
  thread->mask        = file->event_cnt - 1UL;
  thread->site_cnt    = file->site_cnt < TD_SITE_MAX ? (int)file->site_cnt : TD_SITE_MAX;
  thread->cpu         = -1;
  thread->is_new      = 1;
  thread->found_ns    = td_clock_ns();
  /* A ring older than this tracerd: what it has overwritten was never ours to lose. */
  thread->seen_cnt    = file->cursor;
  thread->seen_cnt   -= thread->seen_cnt < file->event_cnt / 2UL ? thread->seen_cnt
                                                                 : file->event_cnt / 2UL;
  thread->is_live     = 1;  /* last: from here the fault handler knows the mapping as a ring's */
  td_ring_follow( thread );  /* the drain is told the slot is live */
  if( file->site_cnt > TD_SITE_MAX )
    fprintf( stderr, "tracerd: %s has %u sites, the first %d are followed\n", path,
             file->site_cnt, TD_SITE_MAX );
}

/* Remove `path` if it is still the file of this inode: a new thread of the same ids may
   have renamed its own ring onto the name meanwhile. Returns 0, or a negative errno when
   the file is there, is that file, and could not be removed. */
static int td_ring_unlink( char const * path, uint64_t inode, uint64_t device ) {
  struct stat status;
  if( td_ring_keeps_files || stat( path, &status ) ) return 0;
  if( (uint64_t)status.st_ino != inode || (uint64_t)status.st_dev != device ) return 0;
  return unlink( path ) && errno != ENOENT ? -errno : 0;
}

/* A slot for a new ring: a free one; else, for a thread that lives, the slot of the
   thread gone longest. Threads that died since the last look are seen gone first, so that
   a program restarted between two looks finds the slots of its former threads. NULL: none;
   *is_dead says the ring's own thread is dead, and its ring has no claim to history. */
static td_thread_t * td_ring_slot_take( td_ring_file_t const * file, int * is_dead ) {
  td_thread_t * thread = td_ring_slot_free( file );
  *is_dead = 0;
  if( thread ) return thread;
  *is_dead = td_ring_thread_cpu( file->pid, file->tid, file->start_ticks ) == TD_RING_DEAD;
  if( *is_dead ) return NULL;
  td_ring_is_changed |= td_ring_check_all();
  thread = td_ring_slot_free( file );
  return thread ? thread : td_ring_slot_evict();
}

/* A new ring: map it, and only then take a slot for it. `ids` is the "<pid>.<tid>" of its
   name. With no slot free, the history of a gone thread gives way to a thread that lives,
   not to the ring of a dead one: that is removed here. Returns 1 when a thread was added. */
static int td_ring_add( char const * path, char const * ids ) {
  td_ring_file_t       file;
  trace_ring_t const * ring   = td_ring_map( path, &file );
  char const *         fault  = ring ? td_ring_name_fault( ids, &file ) : NULL;
  td_thread_t *        thread = NULL;
  int                  is_dead = 0;

  if( !ring ) return 0;
  if( fault ) TD_LOG_ONCE( "tracerd: %s %s: ignored (the first such file)\n", path, fault );
  if( !fault && !td_ring_is_followed( &file ) ) {
    thread = td_ring_slot_take( &file, &is_dead );
    if( is_dead && td_ring_unlink( path, file.inode, file.device ) )
      TD_LOG_ONCE( "tracerd: cannot remove %s: %s (the first such file)\n", path,
                   strerror( errno ) );
    if( !thread && !is_dead ) {
      TD_LOG_ONCE( "tracerd: more than %d threads, %s and any further are not followed\n",
                   TD_THREAD_MAX, path );
      td_ring_unfollowed_cnt++;
    }
  }
  if( !thread ) {
    munmap( (void *)ring, file.file_bytes );
    return 0;
  }
  td_ring_fill( thread, path, ring, &file );
  return 1;
}

/* The thread is no more (is_dead: its file is then removed, if it is still the file that
   was mapped), or its file is no longer its ring. A file that cannot be removed keeps its
   slot's name, so that it is not found again as a new ring four times a second. */
static int td_ring_gone( td_thread_t * thread, int is_dead ) {
  int is_left = is_dead && td_ring_unlink( thread->path, thread->inode, thread->device );
  if( is_left )
    TD_LOG_ONCE( "tracerd: cannot remove %s: %s (the first such file)\n", thread->path,
                 strerror( errno ) );
  td_ring_finish( thread, is_dead );  /* its last events, and the trace they end */
  td_ring_task_close( (int)( thread - td_thread ) );  /* no one reads a gone thread */
  thread->is_gone = 1;
  thread->gone_ns = td_clock_ns();
  if( !is_left ) thread->path[ 0 ] = '\0';  /* a new ring may now take the name */
  return 1;
}

/* Has the thread exited, or has another ring taken its file name? Also notes the CPU the
   thread last ran on. Returns 1 when something the page shows changed. */
static int td_ring_check( td_thread_t * thread ) {
  struct stat    status;
  td_task_stat_t task_stat;
  int            cpu, is_changed, stat_err;

  if( thread->is_gone ) {
    /* A file that could not be removed is tried again at every look, and holds its slot
       until it can be: freed, the slot's file would be found again as a new ring. */
    if( thread->path[ 0 ] && !td_ring_unlink( thread->path, thread->inode, thread->device ) )
      thread->path[ 0 ] = '\0';
    if( thread->path[ 0 ] || td_clock_ns() - thread->gone_ns < TD_GONE_KEEP_NS ) return 0;
    munmap( (void *)thread->ring, thread->ring_bytes );
    thread->is_live = 0;
    return 1;
  }
  if( thread->is_cut ) {
    TD_LOG_ONCE( "tracerd: %s was cut short while mapped: no longer followed\n", thread->path );
    return td_ring_gone( thread, 0 );
  }
  if( stat( thread->path, &status ) || (uint64_t)status.st_ino != thread->inode )
    return td_ring_gone( thread, 0 );
  /* The faults source reads the thread's stat ten times a second: its last read is taken
     when it is recent, and the thread is not read once more for this look. Else a read is
     made here, which the faults source is given if it comes later in the tick. */
  stat_err = td_ring_task_stat( (int)( thread - td_thread ),
                                td_clock_ns() - TD_RING_STAT_FRESH_NS, &task_stat );
  cpu      = td_ring_stat_cpu( stat_err, &task_stat, thread->start_ticks );
  if( cpu == TD_RING_DEAD ) return td_ring_gone( thread, 1 );
  if( cpu == TD_RING_UNKNOWN ) return 0;
  is_changed  = cpu != thread->cpu;
  if( thread->cpu >= 0 && is_changed ) thread->move_cnt++;
  thread->cpu = cpu;
  return is_changed;
}

int td_ring_check_all( void ) {
  int is_changed = 0;
  for( int i = 0; i < TD_THREAD_MAX; i++ )
    if( td_thread[ i ].is_live ) is_changed |= td_ring_check( &td_thread[ i ] );
  return is_changed;
}

/* A ring under its temporary name for a minute was left by a process killed while it
   attached: nothing will ever rename or remove it. */
static void td_ring_temp_reap( char const * path ) {
  struct stat status;
  if( td_ring_keeps_files || lstat( path, &status ) || !S_ISREG( status.st_mode ) ) return;
  if( geteuid() && status.st_uid != geteuid() ) return;
  if( time( NULL ) - status.st_mtime > TD_RING_TEMP_KEEP_SECONDS ) unlink( path );
}

/* Is `name` exactly "<pid>.<tid>" followed by `tail`? Other files of the directory, a
   copy or a note beside a ring, are not rings and not ours to touch. */
static int td_ring_is_ids( char const * name, char const * tail ) {
  char     canonical[ TD_PATH_BYTES ];
  unsigned pid = 0U, tid = 0U;
  if( sscanf( name, TD_RING_IDS_FORMAT, &pid, &tid ) != 2 ) return 0;
  snprintf( canonical, sizeof( canonical ), TD_RING_IDS_FORMAT "%s", pid, tid, tail );
  return !strcmp( canonical, name );
}

int td_ring_discover( char const * dir_path ) {
  size_t          prefix_bytes = strlen( TRACE_RING_PREFIX );
  DIR *           dir;
  struct dirent * entry;
  int             is_changed   = 0;

  if( !td_ring_is_set_up ) td_ring_setup();
  dir = opendir( dir_path );
  if( !dir ) {
    TD_LOG_ONCE( "tracerd: cannot list %s: %s\n", dir_path, strerror( errno ) );
    return 0;
  }
  while( ( entry = readdir( dir ) ) ) {
    struct stat   status;
    char          path[ TD_PATH_BYTES ];
    char const *  name = entry->d_name;
    td_thread_t * thread;
    char const *  ids  = name + prefix_bytes;
    int           is_temp;
    if( strncmp( name, TRACE_RING_PREFIX, prefix_bytes ) ) continue;
    is_temp = td_ring_is_ids( ids, TRACE_RING_TEMP );
    if( !is_temp && !td_ring_is_ids( ids, "" ) ) continue;  /* tracer.pmu-hold, a copy, a note */
    if( snprintf( path, sizeof( path ), "%s/%s", dir_path, name ) >= (int)sizeof( path ) ) {
      TD_LOG_ONCE( "tracerd: rings of %s have paths over %d bytes: not followed\n", dir_path,
                   TD_PATH_BYTES - 1 );
      continue;
    }
    if( is_temp ) { td_ring_temp_reap( path ); continue; }
    thread = td_ring_find( path );
    /* A gone thread whose file could not be removed still holds the name: unless the file
       under that name is by now another one. */
    if( thread && thread->is_gone &&
        ( stat( path, &status ) || (uint64_t)status.st_ino != thread->inode ) ) {
      thread->path[ 0 ] = '\0';
      thread            = NULL;
    }
    if( !thread ) is_changed |= td_ring_add( path, ids );
  }
  closedir( dir );
  is_changed        |= td_ring_is_changed;
  td_ring_is_changed = 0;
  return is_changed;
}
