/* disturb.c: is a spinning thread disturbed by being observed? The README's promise, that
   the tracer sends no interrupt to a traced core, tested. Pins a thread, marks events at a
   chosen rate and records every gap between two turns of its loop. Run it alone, then with
   tracerd watching, and compare. TRACE_PMU=1 adds counters. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../daemon/td_util.h"
#include "../src/trace.h"
#include "../src/trace_pmu.h"

#define DISTURB_GAP_BUCKET_CNT (5)
#define DISTURB_TEXT_BYTES     (16UL << 20)  /* /proc/interrupts of a large host */
#define DISTURB_SECONDS        (10.0)  /* when the command line gives none */
#define DISTURB_MARK_EVERY     (64UL)  /* loop turns per event, likewise; a power of two */

enum { DISTURB_SITE_TURN, DISTURB_SITE_CNT };
static trace_site_t const disturb_site[ DISTURB_SITE_CNT ] = { { "turn", 0U } };

static double const disturb_gap_floor_ns[ DISTURB_GAP_BUCKET_CNT ] = {
  500.0, 2000.0, 10000.0, 50000.0, 1000000.0,
};
static char disturb_text[ DISTURB_TEXT_BYTES ];  /* where /proc/interrupts is read into */

/* Every interrupt `cpu` has taken, all kinds summed; or -1 when the table cannot be read
   or has no column for it: "0 interrupts" must never mean "not known". */
static long disturb_interrupt_cnt( int cpu ) {
  long      total = 0L;
  td_rows_t rows;
  if( td_file_read( TD_INTERRUPTS_FILE, disturb_text, sizeof( disturb_text ) ) <= 0L ) return -1L;
  if( td_rows_begin( &rows, disturb_text, cpu ) ) return -1L;
  while( td_rows_next( &rows ) ) total += (long)rows.count;
  return total;
}

static void disturb_report( uint64_t turn_cnt, uint64_t mark_cnt, uint64_t longest_tsc,
                            uint64_t const * gap_cnt, double run_seconds, uint64_t tsc_hz ) {
  double ns_per_second = (double)TD_NS_PER_SECOND;
  printf( "turns %lu, %.1f ns each, %.2f M marks per second, longest gap %.0f ns\n",
          (unsigned long)turn_cnt, run_seconds * ns_per_second / (double)turn_cnt,
          (double)mark_cnt / run_seconds / 1e6,
          (double)longest_tsc * ns_per_second / (double)tsc_hz );
  for( int i = 0; i < DISTURB_GAP_BUCKET_CNT; i++ )
    printf( "gaps over %7.0f ns: %lu\n", disturb_gap_floor_ns[ i ], (unsigned long)gap_cnt[ i ] );
}

int main( int arg_cnt, char ** arg ) {
  double    run_seconds = arg_cnt >= 3 ? atof( arg[ 2 ] ) : DISTURB_SECONDS;
  uint64_t  mark_every  = arg_cnt >= 4 ? strtoul( arg[ 3 ], NULL, 0 ) : DISTURB_MARK_EVERY;
  uint64_t  gap_cnt[ DISTURB_GAP_BUCKET_CNT ] = { 0UL }, gap_floor_tsc[ DISTURB_GAP_BUCKET_CNT ];
  uint64_t  tsc_hz, stop_tsc, previous_tsc, turn_cnt = 0UL, mark_cnt = 0UL, longest_tsc = 0UL;
  long      interrupt_before, interrupt_after;
  cpu_set_t cpu_set;
  int       cpu, counter_cnt;

  if( arg_cnt < 2 || !mark_every || ( mark_every & ( mark_every - 1UL ) ) ) {
    fprintf( stderr, "usage: disturb CPU [SECONDS [MARK_EVERY_N_TURNS, a power of two]]\n" );
    return 2;
  }
  cpu = atoi( arg[ 1 ] );
  CPU_ZERO( &cpu_set );
  CPU_SET( cpu, &cpu_set );
  if( sched_setaffinity( 0, sizeof( cpu_set ), &cpu_set ) ) {
    perror( "disturb: sched_setaffinity" );
    return 1;
  }
  if( trace_attach( disturb_site, DISTURB_SITE_CNT, "disturb" ) ) return 1;
  counter_cnt = trace_pmu_attach();

  tsc_hz = trace_tsc_hz();
  for( int i = 0; i < DISTURB_GAP_BUCKET_CNT; i++ )
    gap_floor_tsc[ i ] = (uint64_t)( disturb_gap_floor_ns[ i ] * (double)tsc_hz /
                                     (double)TD_NS_PER_SECOND );
  interrupt_before = disturb_interrupt_cnt( cpu );
  printf( "disturb: cpu %d, %d counters, a mark every %lu turns\n", sched_getcpu(), counter_cnt,
          (unsigned long)mark_every );
  fflush( stdout );

  previous_tsc = trace_now();
  stop_tsc     = previous_tsc + (uint64_t)( run_seconds * (double)tsc_hz );
  while( previous_tsc < stop_tsc ) {
    uint64_t now_tsc = trace_now();  /* with counters on, one read per period happens in here */
    uint64_t gap_tsc = now_tsc - previous_tsc;
    if( !( turn_cnt & ( mark_every - 1UL ) ) ) {
      trace_mark( DISTURB_SITE_TURN, gap_tsc );
      mark_cnt++;
    }
    if( gap_tsc > longest_tsc ) longest_tsc = gap_tsc;
    for( int i = 0; i < DISTURB_GAP_BUCKET_CNT; i++ ) gap_cnt[ i ] += gap_tsc >= gap_floor_tsc[ i ];
    turn_cnt++;
    previous_tsc = now_tsc;
  }

  disturb_report( turn_cnt, mark_cnt, longest_tsc, gap_cnt, run_seconds, tsc_hz );
  interrupt_after = disturb_interrupt_cnt( cpu );
  if( interrupt_before < 0L || interrupt_after < 0L )
    printf( "interrupts on cpu %d during the run: not known, the table was not read\n", cpu );
  else
    printf( "interrupts on cpu %d during the run: %ld\n", cpu, interrupt_after - interrupt_before );
  return 0;
}
