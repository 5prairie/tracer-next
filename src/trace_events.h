/* trace_events.h: the hardware counters, one row per metric, in priority order. A row lists
   candidates; the first that opens and counts on the host is used. The stall rows do not
   share a unit. x86: front_stall and back_stall count pipeline slots, several per cycle
   (4.4 per cycle measured for back_stall on Granite Rapids), unless only the generic
   stalled-cycles event is left, which counts cycles; mem_stall counts cycles. aarch64: all
   three count cycles. */
#ifndef TRACE_EVENTS_H
#define TRACE_EVENTS_H

#include <linux/perf_event.h>

#include "trace_arch.h"

#define TRACE_CANDIDATE_MAX (4)

/* A candidate is an event name under <core PMU>/events in sysfs, or a perf type and config.
   A raw code is valid for one vendor: Intel perfmon lists (Golden Cove and later), AMD PPR
   (Zen 2 and later; the stall events Zen 4 and later). */
typedef struct {
  char const * sysfs_name;  /* NULL: use type and config */
  int          vendor;
  uint32_t     type;
  uint64_t     config;
} trace_candidate_t;

/* A candidate that opens and then reads zero under the self-test of trace_pmu.h is dropped:
   it would cost a read every round and show nothing (a counter the hypervisor does not back
   reads zero for ever). Two metrics are kept even so, marked unproven, because the self-test
   cannot be expected to move them: remote loads on a host with one node, and last-level
   misses, since the test's 16 MiB lie within the last-level cache of a large server. A
   third only where the kernel's pages are larger than 4 KiB: dTLB walks, since those 16 MiB
   are then few enough pages for the TLB to hold. */
#define TRACE_EVENTS_MUST_COUNT         (0)
#define TRACE_EVENTS_MAY_IDLE           (1)
#define TRACE_EVENTS_MAY_IDLE_BIG_PAGES (2)

typedef struct {
  char const *      name;
  int               may_idle;  /* TRACE_EVENTS_*: is it kept though the self-test read zero */
  trace_candidate_t candidate[ TRACE_CANDIDATE_MAX ];  /* ends at the first all-zero entry */
} trace_metric_t;

#define TRACE_EVENTS_SYSFS( name )       { name, TRACE_VENDOR_ANY, 0U, 0UL }
#define TRACE_EVENTS_SYSFS_INTEL( name ) { name, TRACE_VENDOR_INTEL, 0U, 0UL }
#define TRACE_EVENTS_GENERIC( config )   { NULL, TRACE_VENDOR_ANY, PERF_TYPE_HARDWARE, config }
#define TRACE_EVENTS_INTEL( event, umask, cmask ) \
  { NULL, TRACE_VENDOR_INTEL, PERF_TYPE_RAW, TRACE_ARCH_EVENT( event, umask, cmask ) }
#define TRACE_EVENTS_AMD( event, umask ) \
  { NULL, TRACE_VENDOR_AMD, PERF_TYPE_RAW, TRACE_ARCH_EVENT( event, umask, 0 ) }
#define TRACE_EVENTS_CACHE_MISS( cache )                                      \
  { NULL, TRACE_VENDOR_ANY, PERF_TYPE_HW_CACHE,                               \
    (uint64_t)(cache) | ( (uint64_t)PERF_COUNT_HW_CACHE_OP_READ << 8 ) |      \
    ( (uint64_t)PERF_COUNT_HW_CACHE_RESULT_MISS << 16 ) }

static trace_metric_t const trace_metric[] = {
#if defined(__x86_64__)
  { "dtlb_walk",   TRACE_EVENTS_MAY_IDLE_BIG_PAGES,
    { TRACE_EVENTS_INTEL( 0x12, 0x0e, 0 ), TRACE_EVENTS_AMD( 0x45, 0xf0 ),
      TRACE_EVENTS_CACHE_MISS( PERF_COUNT_HW_CACHE_DTLB ) } },
  { "llc_miss",    TRACE_EVENTS_MAY_IDLE,
    { TRACE_EVENTS_SYSFS_INTEL( "cache-misses" ), TRACE_EVENTS_INTEL( 0xd1, 0x20, 0 ) } },
  { "mem_stall",   TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_INTEL( 0xa6, 0x21, 5 ), TRACE_EVENTS_INTEL( 0xa3, 0x05, 5 ) } },
  { "front_stall", TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_INTEL( 0x9c, 0x01, 0 ), TRACE_EVENTS_AMD( 0x1a0, 0x01 ),
      TRACE_EVENTS_GENERIC( PERF_COUNT_HW_STALLED_CYCLES_FRONTEND ) } },
  { "l2_miss",     TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_INTEL( 0xd1, 0x10, 0 ), TRACE_EVENTS_AMD( 0x64, 0x09 ) } },
  { "back_stall",  TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_INTEL( 0xa4, 0x02, 0 ), TRACE_EVENTS_AMD( 0x1a0, 0x1e ),
      TRACE_EVENTS_GENERIC( PERF_COUNT_HW_STALLED_CYCLES_BACKEND ) } },
  { "l1d_miss",    TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_INTEL( 0xd1, 0x08, 0 ), TRACE_EVENTS_CACHE_MISS( PERF_COUNT_HW_CACHE_L1D ) } },
  { "branch_miss", TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_SYSFS( "branch-misses" ),
      TRACE_EVENTS_GENERIC( PERF_COUNT_HW_BRANCH_MISSES ) } },
  { "remote_load", TRACE_EVENTS_MAY_IDLE,   { TRACE_EVENTS_INTEL( 0xd3, 0x02, 0 ) } },
#elif defined(__aarch64__)
  { "dtlb_walk",   TRACE_EVENTS_MAY_IDLE_BIG_PAGES, { TRACE_EVENTS_SYSFS( "dtlb_walk" ) } },
  /* No l2d_cache_lmiss_rd in its place: on a Neoverse V2 that counts L2 misses (8.78M beside
     8.61M l2d_cache_refill on a 16 MiB workload that fits the L3). No event, no metric. */
  { "llc_miss",    TRACE_EVENTS_MAY_IDLE,   { TRACE_EVENTS_SYSFS( "ll_cache_miss_rd" ) } },
  { "mem_stall",   TRACE_EVENTS_MUST_COUNT, { TRACE_EVENTS_SYSFS( "stall_backend_mem" ) } },
  { "front_stall", TRACE_EVENTS_MUST_COUNT, { TRACE_EVENTS_SYSFS( "stall_frontend" ) } },
  { "l2_miss",     TRACE_EVENTS_MUST_COUNT, { TRACE_EVENTS_SYSFS( "l2d_cache_refill" ) } },
  { "back_stall",  TRACE_EVENTS_MUST_COUNT, { TRACE_EVENTS_SYSFS( "stall_backend" ) } },
  { "l1d_miss",    TRACE_EVENTS_MUST_COUNT, { TRACE_EVENTS_SYSFS( "l1d_cache_refill" ) } },
  { "branch_miss", TRACE_EVENTS_MUST_COUNT,
    { TRACE_EVENTS_SYSFS( "br_mis_pred_retired" ), TRACE_EVENTS_SYSFS( "br_mis_pred" ) } },
  { "remote_load", TRACE_EVENTS_MAY_IDLE,   { TRACE_EVENTS_SYSFS( "remote_access" ) } },
#endif
};

#define TRACE_METRIC_CNT ( (int)( sizeof( trace_metric ) / sizeof( trace_metric[ 0 ] ) ) )

#endif /* TRACE_EVENTS_H */
