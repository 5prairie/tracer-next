# tracer-next

A tracer for low-latency programs: pinned, spinning threads on isolated cores.

Your program marks its own stages. Each mark is one timestamp and one 16-byte store into a
ring in shared memory: no branch, no syscall. A daemon, `tracerd`, runs on a housekeeping
CPU. It drains the rings and reads what the machine did to your threads: page faults, cache
and TLB misses, stalls, interrupts, NIC and socket queues. One web page shows both on one
time axis.

| Event | Cost |
| --- | --- |
| A span opens, or an instant is marked | 11 ns |
| A span closes, or a stage boundary closes one and opens the next | 19 ns |
| A gauge, which has no timestamp | under 2 ns |

Closing a span waits for the stage's work to complete before it reads the clock, so that
the span holds all of it: that is the 8 ns more. Measured on isolated cores of a Granite
Rapids and a Neoverse V2 host; `make bench` measures yours.

The tracer sends no interrupt to a traced core and takes no lock a traced thread can wait
on. What cannot be read that way is not read. The exceptions are few, rare and stated:
"What can reach a traced core" in [docs/reading-the-page.md](docs/reading-the-page.md)
lists each with the option that removes it.

## Build and try

Linux, x86-64 or aarch64, gcc or clang. No dependency beyond libc.

```
make                          # builds tracerd, demo, bench and disturb in build/
./build/tracerd &             # serves the page on http://127.0.0.1:7070
                              # (in a VM without an invariant TSC: add --any-clock)
./build/demo 60 2             # a stand-in program: 60 seconds, pinned to CPU 2
```

Open the page. On a remote host, tunnel port 7070 through ssh, or serve it with
`--listen 0.0.0.0:7070`.

## Trace your own program

```c
#include "trace.h"

enum { SITE_RX, SITE_DECIDE, SITE_CNT };
static trace_site_t const site[ SITE_CNT ] = { { "rx", TRACE_ROOT }, { "decide", 0 } };

trace_attach( site, SITE_CNT, "feed" );   /* once per thread, before its hot loop */

for(;;) {
  uint64_t tsc = trace_now();             /* before the poll */
  if( !poll_one( &message ) ) continue;   /* an empty poll writes nothing */
  trace_begin_at( SITE_RX, message.sequence, tsc );
  ...
  trace_next( SITE_RX, SITE_DECIDE, message.sequence );   /* a stage boundary */
  ...
  trace_end( SITE_DECIDE );
}
```

Compile `src/trace_attach.c` into the program. More in
[docs/instrumenting.md](docs/instrumenting.md).

## What it shows, and what it needs

| Data | Needs |
| --- | --- |
| Latency per site and per trace, slowest traces, gauges | `tracerd` as the program's user or root |
| Page faults, scheduling checks, interrupts on the core | The same |
| NIC queues and driver counters, socket queues | The same, and the same network namespace |
| Cache and TLB misses, stalls | `TRACE_PMU=1`, a running `tracerd --pmu-hold`, the settings |

`tracerd` and the program share a pid namespace. Hardware counters are off by default:
reading one costs the thread about 1 us in a VM; about 10 ns is expected on bare metal. See
[docs/reading-the-page.md](docs/reading-the-page.md).

| Setting | For |
| --- | --- |
| x86: `/sys/bus/event_source/devices/cpu/rdpmc` not 0 (the default is 1) | Counters |
| aarch64: `kernel.perf_user_access=1`, kernel 5.17 or a backport, 64-bit counters | Counters |
| `sysctl kernel.perf_event_paranoid=1` or lower | Counting the thread's kernel time too |

Whatever a host does not allow is listed at the bottom of the page with the reason.

## Where things are

| Path | What |
| --- | --- |
| `src/` | What your program includes: `trace.h`, and `trace_pmu.h` for counters |
| `daemon/` | `tracerd`. Each data source is one `src_*.c` file |
| `ui/` | The page: plain JavaScript modules, embedded in `tracerd` |
| `demo/`, `bench/` | A program to watch, the cost of an event |
