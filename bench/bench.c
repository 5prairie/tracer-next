/* bench.c: what one trace event costs, by kind. Times loops of work with events against the
   same loop without them, and against the bare timestamp.
   Usage: ./build/bench [cpu]   (an isolated one, if there is one to spare) */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/trace.h"
#include "../src/trace_pmu.h"

/* A batch stays under half a millisecond, shorter than a scheduler slice: on a shared CPU
   most batches still run whole, and the tenth percentile is one of them. */
#define BENCH_BATCH_CNT   (2001)
#define BENCH_BATCH_TURNS (8192UL)
#define BENCH_PERCENTILE  (10)
#define BENCH_ALONE_MIN   (0.98)  /* of the wall time on the CPU, below which the CPU is shared */

enum { BENCH_SITE_WORK, BENCH_SITE_NEXT, BENCH_SITE_LEVEL, BENCH_SITE_CNT };
/* What a loop turn does besides its work. */
enum { BENCH_ALONE, BENCH_STAMP, BENCH_MARK, BENCH_SPAN, BENCH_NEXT, BENCH_GAUGE, BENCH_KIND_CNT };

static trace_site_t const bench_site[ BENCH_SITE_CNT ] = {
  { "work", TRACE_ROOT }, { "next", 0U }, { "level", 0U },
};
static uint64_t bench_batch_ns[ BENCH_BATCH_CNT ];  /* what each batch of a run took */

/* The stand-in for real work: cheap, and not removable by the compiler. */
static inline uint64_t bench_work( uint64_t state ) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

/* The time this thread has spent on a CPU, in nanoseconds. */
static uint64_t bench_thread_ns( void ) {
  struct timespec now;
  clock_gettime( CLOCK_THREAD_CPUTIME_ID, &now );
  return (uint64_t)now.tv_sec * TRACE_NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

/* Nanoseconds per loop turn: the tenth percentile of the batches. */
static double bench_run( int turn_kind ) {
  volatile uint64_t sink  = 0UL;
  uint64_t          state = TRACE_PMU_SEED;
  for( int batch = 0; batch < BENCH_BATCH_CNT; batch++ ) {
    uint64_t start_ns = trace_clock_ns();
    for( uint64_t i = 0UL; i < BENCH_BATCH_TURNS; i++ ) {
      if( turn_kind == BENCH_SPAN  ) trace_begin( BENCH_SITE_WORK, i );
      state = bench_work( state );
      if( turn_kind == BENCH_SPAN  ) trace_end( BENCH_SITE_WORK );
      if( turn_kind == BENCH_MARK  ) trace_mark( BENCH_SITE_WORK, i );
      if( turn_kind == BENCH_NEXT  ) trace_next( BENCH_SITE_WORK, BENCH_SITE_NEXT, i );
      if( turn_kind == BENCH_GAUGE ) trace_gauge( BENCH_SITE_LEVEL, i );
      if( turn_kind == BENCH_STAMP ) sink = sink + trace_tsc();
    }
    bench_batch_ns[ batch ] = trace_clock_ns() - start_ns;
  }
  sink = sink + state;
  trace_pmu_median( bench_batch_ns, BENCH_BATCH_CNT );  /* sorts them */
  return (double)bench_batch_ns[ BENCH_BATCH_CNT * BENCH_PERCENTILE / 100 ] /
         (double)BENCH_BATCH_TURNS;
}

int main( int arg_cnt, char ** arg ) {
  double   turn_ns[ BENCH_KIND_CNT ], alone;
  uint64_t wall_ns, thread_ns;
  if( arg_cnt >= 2 ) {
    cpu_set_t cpu_set;
    CPU_ZERO( &cpu_set );
    CPU_SET( atoi( arg[ 1 ] ), &cpu_set );
    if( sched_setaffinity( 0, sizeof( cpu_set ), &cpu_set ) ) perror( "bench: sched_setaffinity" );
  }
  if( trace_attach( bench_site, BENCH_SITE_CNT, "bench" ) ) return 1;
  bench_run( BENCH_SPAN );  /* warm up */
  wall_ns   = trace_clock_ns();
  thread_ns = bench_thread_ns();
  for( int kind = 0; kind < BENCH_KIND_CNT; kind++ ) turn_ns[ kind ] = bench_run( kind );
  wall_ns   = trace_clock_ns() - wall_ns;
  thread_ns = bench_thread_ns() - thread_ns;
  alone     = (double)thread_ns / (double)wall_ns;

  /* A span turn is a begin, which costs what a mark costs, the work, and the end. */
  printf( "ring of %lu events: begin or mark %.2f ns, end %.2f ns, next %.2f ns, gauge %.2f ns; "
          "the bare timestamp %.2f ns (work alone %.2f ns per turn)\n",
          (unsigned long)trace_writer.ring->event_cnt,
          turn_ns[ BENCH_MARK ] - turn_ns[ BENCH_ALONE ],
          turn_ns[ BENCH_SPAN ] - turn_ns[ BENCH_MARK ],
          turn_ns[ BENCH_NEXT ] - turn_ns[ BENCH_ALONE ],
          turn_ns[ BENCH_GAUGE ] - turn_ns[ BENCH_ALONE ],
          turn_ns[ BENCH_STAMP ] - turn_ns[ BENCH_ALONE ], turn_ns[ BENCH_ALONE ] );
  if( alone < BENCH_ALONE_MIN )
    fprintf( stderr, "bench: the thread had its CPU %.0f%% of the time: the CPU is shared and "
             "these numbers may be too high. Give it a CPU of its own.\n", alone * 100.0 );
  return 0;
}
