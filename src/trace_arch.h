/* trace_arch.h: the architecture seam, x86-64 beside aarch64. Timestamp, counter read,
   counter event encoding. With trace_events.h, the only file that tests the architecture.
   Valid C and C++. */
#ifndef TRACE_ARCH_H
#define TRACE_ARCH_H

#include <stdint.h>
#include <time.h>

/* Inlined whatever the compiler thinks of the caller: gcc does not inline into main, or
   into a function it takes for cold, and a mark would then be a call. */
#define TRACE_INLINE static inline __attribute__(( always_inline ))

#define TRACE_ARCH_CALIBRATE_NS (50000000UL)  /* how long the tick rate is measured, on x86 */
#define TRACE_ARCH_PAIR_TRIES   (5)

#define TRACE_VENDOR_ANY   (0)
#define TRACE_VENDOR_INTEL (1)
#define TRACE_VENDOR_AMD   (2)

#if defined(__x86_64__)

#include <x86intrin.h>
#include <cpuid.h>

#define TRACE_ARCH_NAME        "x86-64"
#define TRACE_ARCH_PMU_GLOB    "/sys/bus/event_source/devices/{cpu,cpu_core}"
#define TRACE_ARCH_PMU_CONFIG1 (0UL)

/* Not serialising: the counter is read when the instruction enters the out-of-order window,
   which can be before the loads ahead of it have finished. A stage that ends in cache
   misses then loses them to whatever comes next: four dependent misses of 400 to 600 ns
   were recorded as 10 to 16 ns. Fine for a begin, a mark or a poll; an end uses the fenced
   read below. */
TRACE_INLINE uint64_t trace_tsc( void ) { return __rdtsc(); }

/* The same counter, read only once every instruction before it has completed: what closes
   a span. The fence costs 6 to 8 ns and nothing of the caller overlaps it. */
TRACE_INLINE uint64_t trace_tsc_fenced( void ) {
  _mm_lfence();
  return __rdtsc();
}

/* Ask for a cache line in the state a store needs, ahead of the store. The instruction
   itself, not __builtin_prefetch: without -mprfchw gcc makes that a prefetch for reading,
   which leaves the line to be asked for again by the store. */
TRACE_INLINE void trace_arch_own( void const * line ) {
  __asm__ volatile( "prefetchw %0" : : "m"( *(char const *)line ) );
}

/* Hand a line this core is done with down to the shared cache. A hint: a processor without
   CLDEMOTE runs it as a no-op. */
#define TRACE_ARCH_HAS_DEMOTE (1)
TRACE_INLINE void trace_arch_demote( void const * line ) {
  __asm__ volatile( "cldemote %0" : : "m"( *(char const *)line ) );
}

/* Ticks between two trace_own_back of a thread that polls: 0.5 to 1 ms on a counter of 2 to
   4 GHz. A count of ticks, not a time: no attach has to measure the tick rate for it. */
#define TRACE_ARCH_OWN_TICKS (1UL << 21)

/* Ticks per second of trace_tsc, or 0 when it has to be measured against a clock. */
static inline uint64_t trace_arch_tsc_hz( void ) { return 0UL; }

/* 1 when the timestamp counter runs at one rate in every power state. */
static inline int trace_tsc_is_invariant( void ) {
  unsigned eax, ebx, ecx, edx;
  if( !__get_cpuid( 0x80000007U, &eax, &ebx, &ecx, &edx ) ) return 0;
  return (int)( ( edx >> 8 ) & 1U );
}

/* The setting that lets a thread read its counters, as a file a user can read: none on x86
   (/sys/bus/event_source/devices/cpu/rdpmc is for root), so the events are opened to find out. */
#define TRACE_ARCH_USER_ACCESS_FILE ""
#define TRACE_ARCH_USER_ACCESS_NAME ""

/* counter_idx is perf_event_mmap_page.index - 1. Fixed counters carry bit 30. */
static inline uint64_t trace_counter_read( uint32_t counter_idx ) {
  return __rdpmc( (int)counter_idx );
}

static inline int trace_arch_vendor( void ) {
  unsigned eax, ebx, ecx, edx;
  if( !__get_cpuid( 0U, &eax, &ebx, &ecx, &edx ) ) return TRACE_VENDOR_ANY;
  if( ecx == 0x6c65746eU ) return TRACE_VENDOR_INTEL;  /* "ntel" */
  if( ecx == 0x444d4163U ) return TRACE_VENDOR_AMD;    /* "cAMD" */
  return TRACE_VENDOR_ANY;
}

/* perf config of a core PMU event: event 0-7 (bits 8-11 at 32-35), umask 8-15, cmask 24-31. */
#define TRACE_ARCH_EVENT( event, umask, cmask )                              \
  ( ( (uint64_t)(event) & 0xffUL ) | ( (uint64_t)(umask) << 8 ) |            \
    ( (uint64_t)(cmask) << 24 ) | ( ( ( (uint64_t)(event) >> 8 ) & 0xfUL ) << 32 ) )

#elif defined(__aarch64__)

#define TRACE_ARCH_NAME        "aarch64"
#define TRACE_ARCH_PMU_GLOB    "/sys/bus/event_source/devices/armv8_*"
#define TRACE_ARCH_PMU_CONFIG1 (3UL)  /* bit 0: 64-bit counter. bit 1: readable from user space */

/* Not serialising, as on x86: read ahead of the loads before it, see there. */
TRACE_INLINE uint64_t trace_tsc( void ) {
  uint64_t tsc;
  __asm__ volatile( "mrs %0, cntvct_el0" : "=r"( tsc ) );
  return tsc;
}

/* Read once everything before it has completed: the isb costs about 7 ns. */
TRACE_INLINE uint64_t trace_tsc_fenced( void ) {
  uint64_t tsc;
  __asm__ volatile( "isb\n\tmrs %0, cntvct_el0" : "=r"( tsc ) );
  return tsc;
}

/* Ask for a cache line in the state a store needs, ahead of the store: into the last-level
   cache and no nearer. Measured on a Neoverse V2: pstl1keep made the burst that follows
   slower (55 to 128 ns). pstl2keep gained most, and put the ring's lines in an L2 that the
   core's stores alone leave them out of: a program with a working set of 80% of that L2 or
   more lost 7 to 51 ns per message of 7 events to it, back to back, and more with pauses
   between the messages; 1 to 3 ns with pstl3keep. */
TRACE_INLINE void trace_arch_own( void const * line ) {
  __asm__ volatile( "prfm pstl3keep, [%0]" : : "r"( line ) );
}

/* No instruction hands a line down to the shared cache. */
#define TRACE_ARCH_HAS_DEMOTE (0)

/* No trace_own_back from trace_now: 0 is never. */
#define TRACE_ARCH_OWN_TICKS (0UL)

static inline uint64_t trace_arch_tsc_hz( void ) {
  uint64_t hz;
  __asm__ volatile( "mrs %0, cntfrq_el0" : "=r"( hz ) );
  return hz;
}

static inline int trace_tsc_is_invariant( void ) { return 1; }

/* 0 in this file: no counter is readable from user space, whatever is opened. Absent before
   kernel 5.17: the events are then opened to find out. */
#define TRACE_ARCH_USER_ACCESS_FILE "/proc/sys/kernel/perf_user_access"
#define TRACE_ARCH_USER_ACCESS_NAME "kernel.perf_user_access"

/* counter_idx is perf_event_mmap_page.index - 1; 31 is the cycle counter. A system
   register cannot be chosen at run time, hence one case per counter. */
#define TRACE_ARCH_COUNTER_CASE( counter ) \
  case counter: __asm__ volatile( "mrs %0, pmevcntr" #counter "_el0" : "=r"( value ) ); break;

static inline uint64_t trace_counter_read( uint32_t counter_idx ) {
  uint64_t value = 0UL;
  switch( counter_idx ) {
    TRACE_ARCH_COUNTER_CASE(  0 ) TRACE_ARCH_COUNTER_CASE(  1 ) TRACE_ARCH_COUNTER_CASE(  2 )
    TRACE_ARCH_COUNTER_CASE(  3 ) TRACE_ARCH_COUNTER_CASE(  4 ) TRACE_ARCH_COUNTER_CASE(  5 )
    TRACE_ARCH_COUNTER_CASE(  6 ) TRACE_ARCH_COUNTER_CASE(  7 ) TRACE_ARCH_COUNTER_CASE(  8 )
    TRACE_ARCH_COUNTER_CASE(  9 ) TRACE_ARCH_COUNTER_CASE( 10 ) TRACE_ARCH_COUNTER_CASE( 11 )
    TRACE_ARCH_COUNTER_CASE( 12 ) TRACE_ARCH_COUNTER_CASE( 13 ) TRACE_ARCH_COUNTER_CASE( 14 )
    TRACE_ARCH_COUNTER_CASE( 15 ) TRACE_ARCH_COUNTER_CASE( 16 ) TRACE_ARCH_COUNTER_CASE( 17 )
    TRACE_ARCH_COUNTER_CASE( 18 ) TRACE_ARCH_COUNTER_CASE( 19 ) TRACE_ARCH_COUNTER_CASE( 20 )
    TRACE_ARCH_COUNTER_CASE( 21 ) TRACE_ARCH_COUNTER_CASE( 22 ) TRACE_ARCH_COUNTER_CASE( 23 )
    TRACE_ARCH_COUNTER_CASE( 24 ) TRACE_ARCH_COUNTER_CASE( 25 ) TRACE_ARCH_COUNTER_CASE( 26 )
    TRACE_ARCH_COUNTER_CASE( 27 ) TRACE_ARCH_COUNTER_CASE( 28 ) TRACE_ARCH_COUNTER_CASE( 29 )
    TRACE_ARCH_COUNTER_CASE( 30 )
    case 31: __asm__ volatile( "mrs %0, pmccntr_el0" : "=r"( value ) ); break;
    default: break;
  }
  return value;
}

static inline int trace_arch_vendor( void ) { return TRACE_VENDOR_ANY; }

/* perf config of a core PMU event: the event number. */
#define TRACE_ARCH_EVENT( event, umask, cmask ) ( (uint64_t)(event) )

#else
#error "tracer-next supports x86-64 and aarch64"
#endif

#define TRACE_NS_PER_SECOND (1000000000UL)
#define TRACE_NS_PER_US     (1000UL)

/* The raw monotonic clock, in nanoseconds. For cold paths: it is a call into the vDSO. */
static inline uint64_t trace_clock_ns( void ) {
  struct timespec now;
  clock_gettime( CLOCK_MONOTONIC_RAW, &now );
  return (uint64_t)now.tv_sec * TRACE_NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

/* The clock and the timestamp counter read as one instant: the tightest of a few pairs, so
   that being interrupted between the two reads does not skew a calibration. */
static inline uint64_t trace_clock_pair( uint64_t * tsc ) {
  uint64_t best_ns = 0UL, best_gap_ns = UINT64_MAX;
  for( int i = 0; i < TRACE_ARCH_PAIR_TRIES; i++ ) {
    uint64_t before_ns = trace_clock_ns(), now_tsc = trace_tsc(), gap_ns;
    gap_ns = trace_clock_ns() - before_ns;
    if( gap_ns >= best_gap_ns ) continue;
    best_gap_ns = gap_ns;
    best_ns     = before_ns + gap_ns / 2UL;
    *tsc        = now_tsc;
  }
  return best_ns;
}

/* Ticks per second of trace_tsc: asked of the CPU, or measured against the clock. */
static inline uint64_t trace_tsc_hz( void ) {
  uint64_t hz = trace_arch_tsc_hz(), start_ns, start_tsc = 0UL, stop_ns, stop_tsc = 0UL;
  if( hz ) return hz;
  start_ns = trace_clock_pair( &start_tsc );
  while( trace_clock_ns() - start_ns < TRACE_ARCH_CALIBRATE_NS ) { /* spin: no sleep to trust */ }
  stop_ns = trace_clock_pair( &stop_tsc );
  return ( stop_tsc - start_tsc ) * TRACE_NS_PER_SECOND / ( stop_ns - start_ns );
}

#endif /* TRACE_ARCH_H */
