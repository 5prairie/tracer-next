/* td_probe.h: the measuring half of `tracerd --probe`. The host and its clock; and, on
   request, what the hardware counters do: which count, what a read costs.
   Writes nothing on the host; the counters part opens perf events, and says what that did. */
#ifndef TD_PROBE_H
#define TD_PROBE_H

#include <stdint.h>

/* The host and its clock. Returns timestamp ticks per second. */
uint64_t td_probe_host( void );

/* Open every counter of the table on the calling thread, run a workload over buffer_mib of
   memory (0: the default) and report what counts and what a read costs. Opening perf events
   can IPI every CPU of an x86 host: this part is opt-in. `only`, when not NULL, is a list
   of metric names to probe, for hosts with fewer counters than metrics. */
void td_probe_counters( uint64_t tsc_hz, uint64_t buffer_mib, char const * only );

#endif /* TD_PROBE_H */
