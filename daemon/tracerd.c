/* tracerd.c: the daemon. One thread, one loop, on a housekeeping CPU: every 10 ms it drains
   the rings and runs the sources that are due, every 50 ms it closes a slot and sends it to
   the page. The list of sources is here; adding one is its declaration and its row. */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "td_frame.h"
#include "td_http.h"
#include "td_probe.h"
#include "td_source.h"

#define TRACERD_PORT           7070
#define TRACERD_PORT_MAX       (65535L)
#define TRACERD_ADDRESS        "127.0.0.1"
#define TRACERD_ADDRESS_BYTES  (64)
#define TRACERD_TEXT_OF( token ) #token
#define TRACERD_TEXT( token )    TRACERD_TEXT_OF( token )  /* a constant as text, for the usage */

#define TRACERD_DISCOVER_TICKS (25)    /* look for new rings four times a second */
#define TRACERD_PROBE_TICKS    (500)   /* ask every source again what it can read */
#define TRACERD_SLACK_NS       (1UL)   /* how late the kernel may wake the loop to spare a
                                          wakeup: by default 50 us, a third or more of a tick's
                                          lateness */
#define TRACERD_HOLD_MODE      (0644)
#define TRACERD_OPTION_NO      "--no-"
#define TRACERD_OPTION_WITH    "--with-"
#define TRACERD_OPTION_HELP    "--help"
#define TRACERD_SECONDS_MAX    (1000000000L)  /* --seconds: thirty years is long enough */

extern td_source_t const td_src_ring, td_src_gauge, td_src_sched, td_src_faults, td_src_irq;
extern td_source_t const td_src_pmu, td_src_net, td_src_softnet, td_src_nic, td_src_sock;
extern td_source_t const td_src_numa, td_src_uncore, td_src_mesh;

/* Every source, in the order the page shows them. */
static td_source_t const * const tracerd_source[] = {
  &td_src_ring,
  &td_src_gauge,
  &td_src_sched,
  &td_src_faults,
  &td_src_irq,
  &td_src_pmu,
  &td_src_nic,
  &td_src_net,
  &td_src_softnet,
  &td_src_sock,
  &td_src_numa,    /* not run on a multi-node host: off unless --with-numa */
  &td_src_uncore,  /* not run on a host that has it: off unless --with-uncore */
  &td_src_mesh,    /* likewise: --with-mesh */
};
#define TRACERD_SOURCE_CNT TD_COUNT_OF( tracerd_source )

typedef struct {
  char const *  ring_dir;     /* searched as well as TRACE_RING_DIR; NULL for none */
  char          address[ TRACERD_ADDRESS_BYTES ];
  long          port;
  long          cpu;          /* -1: choose a housekeeping CPU */
  long          run_seconds;  /* 0: until a signal */
  int           want_probe, want_pmu, want_hold, is_any_clock;
  long          probe_mib;
  char const *  probe_only;
} tracerd_option_t;

static char const tracerd_usage[] =
  "usage: tracerd [options]\n"
  "  --listen ADDR:PORT  where the page is served: an IPv4 address (default " TRACERD_ADDRESS ":"
                                                               TRACERD_TEXT( TRACERD_PORT ) ")\n"
  "  --ring-dir DIR      look for rings here too (always in " TRACE_RING_DIR ")\n"
  "  --cpu N             run on this CPU (default: the first housekeeping CPU)\n"
  "  --sources LIST      run only these sources, by name and separated by commas\n"
  "  --no-NAME           do not run source NAME\n"
  "  --with-NAME         run a source that is off by default\n"
  "  --set S.M.red=X     override a threshold: source.metric.red or .amber\n"
  "  --seconds N         exit after N seconds\n"
  "  --any-clock         run even if the timestamp counter is not invariant\n"
  "  --probe [--pmu [--mib N] [--only LIST]]   say what this host exposes, then exit\n"
  "  --pmu-hold          only hold one perf event, so that opening others sends no IPI\n"
  "  --help              this text\n";

/* The sources by name, from the table: the usage text knows none of them. */
static void tracerd_usage_print( FILE * where ) {
  fprintf( where, "%ssources:", tracerd_usage );
  for( int i = 0; i < TRACERD_SOURCE_CNT; i++ )
    fprintf( where, " %s%s", tracerd_source[ i ]->name,
             tracerd_source[ i ]->is_unproven ? " (off by default)" : "" );
  fprintf( where, "\n" );
}

static int tracerd_refuse( char const * option_name ) {
  fprintf( stderr, "tracerd: bad option %s\n", option_name );
  tracerd_usage_print( stderr );
  return -EINVAL;
}

static volatile sig_atomic_t tracerd_stop;

static void tracerd_on_signal( int signal_number ) { (void)signal_number; tracerd_stop = 1; }

/* Returns 0, or -EINVAL after naming the bad option. */
static int tracerd_parse( int arg_cnt, char ** arg, tracerd_option_t * option ) {
  for( int i = 1; i < arg_cnt; i++ ) {
    char const * name  = arg[ i ];
    char const * value = i + 1 < arg_cnt ? arg[ i + 1 ] : "";
    char const * colon = strchr( value, ':' );
    int          err   = 0, has_value = 1;
    if(      !strcmp( name, "--probe"     ) ) { option->want_probe = 1; has_value = 0; }
    else if( !strcmp( name, "--pmu"       ) ) { option->want_pmu   = 1; has_value = 0; }
    else if( !strcmp( name, "--pmu-hold"  ) ) { option->want_hold  = 1; has_value = 0; }
    else if( !strcmp( name, "--any-clock" ) ) { option->is_any_clock = 1; has_value = 0; }
    else if( !strncmp( name, TRACERD_OPTION_NO, sizeof( TRACERD_OPTION_NO ) - 1UL ) ) {
      err       = td_store_select( name + sizeof( TRACERD_OPTION_NO ) - 1UL, 0 );
      has_value = 0;
    } else if( !strncmp( name, TRACERD_OPTION_WITH, sizeof( TRACERD_OPTION_WITH ) - 1UL ) ) {
      err       = td_store_select( name + sizeof( TRACERD_OPTION_WITH ) - 1UL, 1 );
      has_value = 0;
    } else if( !value[ 0 ] ) { err = -EINVAL; }
    else if( !strcmp( name, "--ring-dir" ) ) { option->ring_dir = value; }
    else if( !strcmp( name, "--cpu"      ) ) {
      err = td_parse_long( value, &option->cpu );
      if( !err && option->cpu >= CPU_SETSIZE ) err = -EINVAL;
    } else if( !strcmp( name, "--seconds"  ) ) {
      err = td_parse_long( value, &option->run_seconds );
      if( !err && option->run_seconds > TRACERD_SECONDS_MAX ) err = -EINVAL;
    } else if( !strcmp( name, "--mib"    ) ) err = td_parse_long( value, &option->probe_mib );
    else if( !strcmp( name, "--only"     ) ) option->probe_only  = value;
    else if( !strcmp( name, "--sources"  ) ) err = td_store_select( value, -1 );
    else if( !strcmp( name, "--set"      ) ) err = td_store_set( value );
    else if( !strcmp( name, "--listen"   ) && colon ) {
      snprintf( option->address, sizeof( option->address ), "%.*s", (int)( colon - value ), value );
      err = td_parse_long( colon + 1, &option->port );
      if( !err && ( option->port < 1L || option->port > TRACERD_PORT_MAX ) ) err = -EINVAL;
    } else err = -EINVAL;
    if( err ) return tracerd_refuse( name );
    i += has_value;
  }
  /* An option that would be ignored is a mistake worth a word: these are the likely ones. */
  if( option->want_hold && arg_cnt > 2 ) return tracerd_refuse( "--pmu-hold with another option" );
  if( option->want_pmu && !option->want_probe ) return tracerd_refuse( "--pmu without --probe" );
  if( ( option->probe_mib || option->probe_only ) && !option->want_pmu )
    return tracerd_refuse( "--mib or --only without --probe --pmu" );
  return 0;
}

static int tracerd_is_same_dir( char const * path, char const * other_path ) {
  struct stat status, other_status;
  if( stat( path, &status ) || stat( other_path, &other_status ) ) return 0;
  return status.st_dev == other_status.st_dev && status.st_ino == other_status.st_ino;
}

/* Pin to option_cpu, or to the first CPU this process may use that is neither isolated nor
   tickless: a daemon does not belong on a traced core. Returns 0 or a negative errno. */
static int tracerd_pin( long option_cpu ) {
  char      isolated[ TD_CPU_LIST_BYTES ] = "", nohz_full[ TD_CPU_LIST_BYTES ] = "";
  cpu_set_t allowed, chosen;
  int       cpu = (int)option_cpu;

  td_file_read( TD_ISOLATED_FILE,  isolated,  sizeof( isolated ) );
  td_file_read( TD_NOHZ_FULL_FILE, nohz_full, sizeof( nohz_full ) );
  if( sched_getaffinity( 0, sizeof( allowed ), &allowed ) ) return -errno;
  for( int i = 0; cpu < 0 && i < CPU_SETSIZE; i++ ) {
    if( !CPU_ISSET( i, &allowed ) ) continue;
    if( !td_list_has( isolated, i ) && !td_list_has( nohz_full, i ) ) cpu = i;
  }
  if( cpu < 0 ) return -ENOENT;
  CPU_ZERO( &chosen );
  CPU_SET( cpu, &chosen );
  return sched_setaffinity( 0, sizeof( chosen ), &chosen ) ? -errno : 0;
}

/* --pmu-hold: hold one hardware counting event and an exclusive lock on the hold file, and
   do nothing else until a signal. While the event exists, opening and closing per-task
   events patches no kernel text, hence sends no IPI; the lock is how traced threads know.
   Returns 0, or a negative errno after one log line. */
static int tracerd_pmu_hold( void ) {
  struct perf_event_attr attr;
  sigset_t               none, both;
  long                   event_fd;
  char const *           why     = "cannot open a perf event";
  int                    lock_fd = -1, err = 0;

  memset( &attr, 0, sizeof( attr ) );
  attr.size           = sizeof( attr );
  attr.type           = PERF_TYPE_HARDWARE;
  attr.config         = PERF_COUNT_HW_INSTRUCTIONS;
  attr.exclude_kernel = 1;
  attr.exclude_hv     = 1;
  event_fd = syscall( SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC );
  if( event_fd < 0L ) err = -errno;
  if( !err ) {  /* no file without the event: a file is what says "held" */
    lock_fd = open( TRACE_PMU_HOLD_FILE, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                    TRACERD_HOLD_MODE );
    if( lock_fd < 0 || flock( lock_fd, LOCK_EX | LOCK_NB ) ) err = -errno;
    else fchmod( lock_fd, TRACERD_HOLD_MODE );  /* every user's programs must be able to look */
    if( err == -EWOULDBLOCK ) why = "another --pmu-hold runs";
    else if( err ) why = "cannot open " TRACE_PMU_HOLD_FILE " (left by another user? remove it)";
  }
  if( err ) {
    fprintf( stderr, "tracerd: --pmu-hold: %s: %s\n", why, strerror( -err ) );
    return err;
  }
  sigemptyset( &none );
  sigemptyset( &both );  /* blocked from here on, so that one cannot slip in before the wait */
  sigaddset( &both, SIGINT );
  sigaddset( &both, SIGTERM );
  sigprocmask( SIG_BLOCK, &both, NULL );
  signal( SIGHUP, SIG_IGN );  /* a logout must not end the hold: the close reaches every CPU */
  while( !tracerd_stop ) sigsuspend( &none );
  unlink( TRACE_PMU_HOLD_FILE );
  return 0;
}

/* --probe: the host, each source's verdict, and on request the counters. Removes nothing:
   the ring of a dead thread is left for a running tracerd. */
static void tracerd_probe( tracerd_option_t const * option ) {
  uint64_t tsc_hz    = td_probe_host();
  int      has_rings = 0;
  td_ring_keep_files();
  td_ring_discover( TRACE_RING_DIR );
  if( option->ring_dir ) td_ring_discover( option->ring_dir );
  td_ring_check_all();  /* which threads live, and where each runs */
  for( int i = 0; i < TD_THREAD_MAX; i++ )
    has_rings |= td_thread[ i ].is_live && !td_thread[ i ].is_gone;
  td_store_probe( td_clock_ns() );
  printf( "== sources%s\n", has_rings ? "" : " (no traced thread found: host sources only)" );
  for( int i = 0; i < td_source_cnt; i++ ) {
    td_source_t const * source  = td_source_state[ i ].source;
    int                 is_host = source->scope == TD_SCOPE_HOST;
    char const *        why     = is_host || has_rings ? "available" : "needs a traced thread";
    if( !td_source_state[ i ].is_on ) why = "off";
    for( int entry_idx = 0; entry_idx < td_unavailable_cnt; entry_idx++ ) {
      td_unavailable_t const * entry = &td_unavailable_list[ entry_idx ];
      if( entry->source_idx == i && entry->metric_idx < 0 ) why = entry->why;
    }
    printf( "%-28s %s\n", source->name, why );
    for( int entry_idx = 0; entry_idx < td_unavailable_cnt; entry_idx++ ) {  /* metric notes */
      td_unavailable_t const * entry = &td_unavailable_list[ entry_idx ];
      if( entry->source_idx != i || entry->metric_idx < 0 ) continue;
      printf( "  %-26s %s\n", source->metric[ entry->metric_idx ].label, entry->why );
    }
  }
  if( option->want_pmu )
    td_probe_counters( tsc_hz, (uint64_t)option->probe_mib, option->probe_only );
  else printf( "== counters: not probed. --pmu opens perf events, which can IPI every CPU.\n" );
}

/* What the loop remembers from one tick to the next. */
static uint64_t tracerd_slot_next, tracerd_second_next;  /* the next ones to close */
static uint64_t tracerd_discover_tick;  /* when to look for rings again */
static uint64_t tracerd_probe_tick;     /* when to ask the sources again what they can read */
static int      tracerd_is_changed = 1; /* the page's hello frame is out of date */

/* Close the slots before slot_end and the seconds before second_end. is_known 0: they
   ended long ago (the daemon was stopped) and the values of now are not their own; the
   page is sent no frame for them. */
static void tracerd_close( uint64_t slot_end, uint64_t second_end, int is_known ) {
  for( ; tracerd_slot_next < slot_end; tracerd_slot_next++ ) {
    td_ring_close_slot( tracerd_slot_next );
    td_store_close_slot( tracerd_slot_next, is_known );
    td_frame_tick( tracerd_slot_next, is_known );
  }
  for( ; tracerd_second_next < second_end; tracerd_second_next++ ) {
    td_ring_close_second( tracerd_second_next );
    td_store_close_second( tracerd_second_next, is_known );
    td_frame_second( tracerd_second_next, is_known );
  }
}

/* One tick of work. A late loop may have skipped ticks: every slot and second that ended
   since the last one is closed, so that no history index is left holding old data. */
static void tracerd_tick( tracerd_option_t const * option, uint64_t tick, uint64_t now_ns ) {
  uint64_t slot = tick / TD_SLOT_TICKS, second = tick / TD_SECOND_TICKS;
  uint64_t slots_per_second = TD_SECOND_TICKS / TD_SLOT_TICKS;
  uint64_t last_slot = slot - 1UL, last_second = last_slot / slots_per_second;
  if( !tracerd_slot_next ) {  /* the first tick: nothing before it is ours to close */
    tracerd_slot_next   = slot;
    tracerd_second_next = second;
  }
  if( tick >= tracerd_discover_tick ) {
    td_ring_unfollowed_cnt = 0;
    tracerd_is_changed |= td_ring_discover( TRACE_RING_DIR );
    if( option->ring_dir ) tracerd_is_changed |= td_ring_discover( option->ring_dir );
    tracerd_is_changed |= td_ring_check_all();  /* after: a new thread's CPU is known at once */
    tracerd_discover_tick = tick + TRACERD_DISCOVER_TICKS;
  }
  /* Probe when something changed, and now and then: hello is sent only if the answer did. */
  if( tracerd_is_changed || tick >= tracerd_probe_tick ) {
    tracerd_is_changed |= td_store_probe( now_ns );
    tracerd_probe_tick  = tick + TRACERD_PROBE_TICKS;
  }
  /* What the drain below brings goes to the slot that just ended and to the second that
     slot is in: all before them is closed first. After a stop of over a second (SIGSTOP, a
     paused VM), the slot and the second that were open then are closed with what was read
     and drained in them; the rest, as far back as the page shows, as not known. */
  if( slot - tracerd_slot_next > slots_per_second ) {
    tracerd_close( tracerd_slot_next + 1UL,
                   tracerd_second_next < last_second ? tracerd_second_next + 1UL : last_second, 1 );
    if( last_slot - tracerd_slot_next > TD_FAST_CNT ) tracerd_slot_next = last_slot - TD_FAST_CNT;
    if( last_second - tracerd_second_next > TD_SLOW_CNT )
      tracerd_second_next = last_second - TD_SLOW_CNT;
    tracerd_close( last_slot, last_second, 0 );
  }
  tracerd_close( last_slot, last_second, 1 );
  td_ring_drain( now_ns );
  tracerd_is_changed |= td_store_tick( tick, now_ns );
  if( tracerd_is_changed ) td_frame_hello_all();
  tracerd_is_changed = 0;
  tracerd_close( slot, second, 1 );
  td_frame_replay_step();
  td_http_flush_all();  /* the tick's frames and its part of the replays: one send per page */
}

static void tracerd_loop( tracerd_option_t const * option ) {
  uint64_t tick_ns = (uint64_t)TD_TICK_MS * TD_NS_PER_MS;
  uint64_t tick    = td_clock_ns() / tick_ns, stop_tick = UINT64_MAX;

  if( option->run_seconds > 0L )
    stop_tick = tick + (uint64_t)option->run_seconds * TD_MS_PER_SECOND / TD_TICK_MS;
  /* A slot takes what is drained until the first tick past its end: that tick's lateness is
     time given to the wrong slot. Refused (not expected), the loop runs 50 us later. */
  prctl( PR_SET_TIMERSLACK, TRACERD_SLACK_NS );
  while( !tracerd_stop && tick < stop_tick ) {
    uint64_t now_ns  = td_clock_ns();
    uint64_t due_ns  = ( tick + 1UL ) * tick_ns;
    int64_t  wait_ns = now_ns < due_ns ? (int64_t)( due_ns - now_ns ) : 0L;
    td_http_poll( wait_ns, td_frame_replay_start );
    now_ns = td_clock_ns();
    if( now_ns < due_ns ) continue;
    tick = now_ns / tick_ns;
    tracerd_tick( option, tick, now_ns );
  }
}

int main( int arg_cnt, char ** arg ) {
  tracerd_option_t option = { .address = TRACERD_ADDRESS, .port = TRACERD_PORT, .cpu = -1 };
  uint64_t         tsc_hz;
  int              err;

  signal( SIGINT, tracerd_on_signal );
  signal( SIGTERM, tracerd_on_signal );
  td_store_init( tracerd_source, TRACERD_SOURCE_CNT );
  for( int i = 1; i < arg_cnt; i++ )
    if( !strcmp( arg[ i ], TRACERD_OPTION_HELP ) ) { tracerd_usage_print( stdout ); return 0; }
  if( tracerd_parse( arg_cnt, arg, &option ) ) return 2;
  if( option.ring_dir && tracerd_is_same_dir( option.ring_dir, TRACE_RING_DIR ) )
    option.ring_dir = NULL;  /* "/dev/shm/": listed once is enough */
  err = tracerd_pin( option.cpu );
  if( err ) {
    fprintf( stderr, "tracerd: %s: %s\n", option.cpu < 0 ? "no housekeeping CPU to run on"
                                                        : "cannot run on the CPU of --cpu",
             strerror( -err ) );
    return 1;
  }
  if( option.want_probe ) { tracerd_probe( &option ); return 0; }
  if( option.want_hold ) return tracerd_pmu_hold() ? 1 : 0;
  if( !trace_tsc_is_invariant() && !option.is_any_clock ) {
    fprintf( stderr, "tracerd: the timestamp counter is not invariant; --any-clock runs anyway\n" );
    return 1;
  }
  err = td_http_init( option.address, (int)option.port );
  if( err ) {
    fprintf( stderr, "tracerd: listening on %s:%ld: %s\n", option.address, option.port,
             strerror( -err ) );
    return 1;
  }
  tsc_hz = trace_tsc_hz();
  if( !tsc_hz ) {
    fprintf( stderr, "tracerd: the timestamp counter's frequency is not known\n" );
    return 1;
  }
  td_ring_init( (double)TD_NS_PER_SECOND / (double)tsc_hz );
  tracerd_loop( &option );
  return 0;
}
