/* demo.c: a stand-in program to watch on the page. One thread sends itself UDP datagrams
   at a fixed rate and handles each in three stages: rx, decide, order. Every so often it
   sends a burst: a queue fills, a slow trace appears. Usage: ./build/demo [seconds [cpu]] */
#define _GNU_SOURCE
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/trace.h"
#include "../src/trace_pmu.h"

#define DEMO_RATE_HZ              (20000.0)
#define DEMO_SECONDS              (30.0)  /* when the command line gives none */
#define DEMO_DECIDE_READS         (4)     /* dependent reads of one decision */
#define DEMO_BURST_PERIOD_SECONDS (5.0)
#define DEMO_BURST_CNT            (200)   /* in a burst: most of a default socket buffer */
#define DEMO_BOOK_BYTES           (1UL << 22)

enum { DEMO_SITE_RX, DEMO_SITE_DECIDE, DEMO_SITE_ORDER, DEMO_SITE_PENDING, DEMO_SITE_CNT };

static trace_site_t const demo_site[ DEMO_SITE_CNT ] = {
  [ DEMO_SITE_RX      ] = { "rx",      TRACE_ROOT },
  [ DEMO_SITE_DECIDE  ] = { "decide",  0U },
  [ DEMO_SITE_ORDER   ] = { "order",   0U },
  [ DEMO_SITE_PENDING ] = { "pending", 0U },  /* a gauge: datagrams sent and not yet handled */
};

static double demo_seconds( void ) {
  struct timespec now;
  clock_gettime( CLOCK_MONOTONIC, &now );
  return (double)now.tv_sec + (double)now.tv_nsec / (double)TRACE_NS_PER_SECOND;
}

/* The "decision": a few dependent reads in a 4 MiB table: past the L2 of most hosts, within
   the L3 of the large ones. */
static uint64_t demo_decide( uint8_t const * book, uint64_t sequence ) {
  uint64_t slot = sequence * 0x9e3779b97f4a7c15UL;
  uint64_t sum  = 0UL;
  for( int i = 0; i < DEMO_DECIDE_READS; i++ ) {
    sum += book[ slot % DEMO_BOOK_BYTES ];
    slot = slot * 6364136223846793005UL + sum + 1UL;
  }
  return sum;
}

static void demo_pin( int cpu ) {
  cpu_set_t cpu_set;
  CPU_ZERO( &cpu_set );
  CPU_SET( cpu, &cpu_set );
  if( sched_setaffinity( 0, sizeof( cpu_set ), &cpu_set ) ) perror( "demo: sched_setaffinity" );
}

int main( int arg_cnt, char ** arg ) {
  struct sockaddr_in address       = { .sin_family = AF_INET };
  socklen_t          address_bytes = sizeof( address );
  double             run_seconds   = arg_cnt >= 2 ? atof( arg[ 1 ] ) : DEMO_SECONDS;
  int                cpu           = arg_cnt >= 3 ? atoi( arg[ 2 ] ) : -1;
  uint8_t *          book          = (uint8_t *)malloc( DEMO_BOOK_BYTES );
  int                fd            = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0 );
  uint64_t           sent = 0UL, handled = 0UL, checksum = 0UL, message;
  double             start_seconds, send_seconds, burst_seconds;

  if( cpu >= 0 ) demo_pin( cpu );
  address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  if( !book || fd < 0 || bind( fd, (struct sockaddr *)&address, sizeof( address ) ) ||
      getsockname( fd, (struct sockaddr *)&address, &address_bytes ) ) {
    perror( "demo" );
    return 1;
  }
  memset( book, 1, DEMO_BOOK_BYTES );
  if( trace_attach( demo_site, DEMO_SITE_CNT, "demo" ) ) return 1;
  trace_pmu_attach();  /* counters, when TRACE_PMU=1 */

  start_seconds = demo_seconds();
  send_seconds  = start_seconds;
  burst_seconds = start_seconds + DEMO_BURST_PERIOD_SECONDS;
  while( demo_seconds() - start_seconds < run_seconds ) {
    double   now_seconds = demo_seconds();
    int      send_cnt = 0;
    uint64_t poll_tsc;
    if( now_seconds >= burst_seconds ) {
      send_cnt       = DEMO_BURST_CNT;
      burst_seconds += DEMO_BURST_PERIOD_SECONDS;
    } else if( now_seconds >= send_seconds ) {
      send_cnt      = 1;
      send_seconds += 1.0 / DEMO_RATE_HZ;
    }
    for( int i = 0; i < send_cnt; i++ )  /* a datagram the full socket refused was not sent */
      sent += sendto( fd, &sent, sizeof( sent ), 0, (struct sockaddr *)&address,
                      sizeof( address ) ) == (ssize_t)sizeof( sent );

    poll_tsc = trace_now();
    if( recv( fd, &message, sizeof( message ), 0 ) != (ssize_t)sizeof( message ) ) continue;
    trace_begin_at( DEMO_SITE_RX, message, poll_tsc );
    handled++;
    trace_gauge( DEMO_SITE_PENDING, sent - handled );
    trace_next( DEMO_SITE_RX, DEMO_SITE_DECIDE, message );  /* a stage boundary: one timestamp */
    checksum += demo_decide( book, message );
    trace_next( DEMO_SITE_DECIDE, DEMO_SITE_ORDER, message );
    checksum ^= checksum << 7;
    trace_end( DEMO_SITE_ORDER );
  }
  printf( "demo: %lu sent, %lu handled, checksum %lx\n", (unsigned long)sent,
          (unsigned long)handled, (unsigned long)checksum );
  return 0;
}
