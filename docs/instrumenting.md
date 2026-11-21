# Instrumenting a program

Include `src/trace.h`, compile `src/trace_attach.c` into the program. The headers are valid
C and C++; `trace_attach.c` is C. Link it into the program itself, not into a library
loaded later.

## Sites

A site is a place you mark. Give each a number and a name:

```c
enum { SITE_RX, SITE_DECIDE, SITE_ORDER, SITE_DEPTH, SITE_CNT };
static trace_site_t const site[ SITE_CNT ] = {
  { "rx", TRACE_ROOT }, { "decide", 0 }, { "order", 0 }, { "depth", 0 },
};
```

A site flagged `TRACE_ROOT` opens a trace. The trace runs from that begin to the last end
before the next root, so idle time is not in it. Without a root site, each site is simply
measured on its own. A trace lives on one thread.

## Calls

| Call | Does |
| --- | --- |
| `trace_attach( site, cnt, name )` | Creates the thread's ring. Once, before any hot loop |
| `trace_begin( site, value )` | A span opens now |
| `trace_begin_at( site, value, tsc )` | A span opened at `tsc`, a timestamp from `trace_now()` |
| `trace_end( site )` | The span closes, once the work before it has completed |
| `trace_next( end_site, begin_site, value )` | A stage boundary: one span closes, the next opens |
| `trace_mark( site, value )` | An instant |
| `trace_gauge( site, value )` | A level, such as a queue depth. Drawn as a lane |
| `trace_now()` | A timestamp for a poll loop. Also samples counters, asks the ring's lines back |
| `trace_sample()` | For a thread without a poll loop: sample counters here |

`value` is 48 bits and is yours: a sequence number, an order id. It is shown beside slow
traces.

Call `trace_now()` before the poll and pass it to `trace_begin_at` when a message arrived.
An empty poll then costs one timestamp and writes nothing.

Between two stages, call `trace_next`, not `trace_end` then `trace_begin`: the timestamp is
most of what an event costs, and a boundary needs one.

```c
trace_begin_at( SITE_RX, sequence, tsc );
...                                          /* rx */
trace_next( SITE_RX, SITE_DECIDE, sequence );
...                                          /* decide */
trace_next( SITE_DECIDE, SITE_ORDER, sequence );
...                                          /* order */
trace_end( SITE_ORDER );
```

`trace_end` and `trace_next` wait for the work before them to complete before they read
the clock. Without that wait the processor reads it early, while the stage's last loads
are still in flight, and a stage that ends in cache misses is recorded as a few
nanoseconds; its time shows up after it. The wait costs 6 to 8 ns. Compile with
`-DTRACE_END_UNFENCED` to drop it, for stages that end on no load worth timing. A begin, a
mark and `trace_now()` never wait.

## Rules

- Every thread attaches before any thread's hot loop starts. Mapping memory can stall the
  other threads of a process for a moment.
- Nothing is unmapped while the program runs. There is no detach.
- A thread that never attached may still call everything: it writes to a dummy slot. All
  such threads share that one slot, so with several of them hot each event costs a cache
  line passed between cores: about 16 ns with two of them, 25 with four, more with each
  one added, not 11. Attach every hot thread.
- If `tracerd` is not running, nothing changes for the program. The ring wraps.
- `tracerd` follows 16 threads and the first 32 sites of each, and says so, on the page for
  threads and on its standard error for sites, when a program has more. It keeps the first
  32 spans of a trace; later ones still count in their site's latency.
- A trace ends at the next root, or after 20 ms with no begin, end or mark and no span
  open. Keep a span open across a wait that belongs to the trace.
- A program has at most 128 sites; `trace_attach` fails with more.
- A forked child starts unattached. It may attach, and ask for counters, like any thread.
- The program runs in `tracerd`'s pid namespace (and time namespace), with `/proc` mounted
  for it. A ring from elsewhere is ignored: its thread ids would name other threads.
- The program is dumpable and keeps the user it attached as, when `tracerd` runs as root.

## Settings, in the program's environment

| Variable | Default | Meaning |
| --- | --- | --- |
| `TRACE_RING_EVENTS` | 65536 | Ring capacity. Rounded up to a power of two, 1024 at least |
| `TRACE_RING_DIR` | `/dev/shm` | The ring's directory, on tmpfs or hugetlbfs. See `--ring-dir` |
| `TRACE_PMU` | off | `1` turns hardware counters on; `force` does so without a hold. See below |
| `TRACE_PMU_PERIOD_US` | from the read cost | Time between two counter reads, 1000 to 1000000 |

A value that is no number, or out of range, is said on the program's standard error and
the default is taken. A ring directory that cannot be used makes `trace_attach` fail, with
one line; the program runs on untraced.

`tracerd` drains every 10 ms and stays a quarter of the ring behind the writer: what a
thread writes between two drains has to fit in 75% of `TRACE_RING_EVENTS`, 49152 events at
the default, and the rest is counted as lost. A ring larger than the default belongs on
hugetlbfs (`TRACE_RING_DIR`): on 4 KiB pages, in a VM, an event went from 9 ns to 20 ns at
262144 events, one TLB miss per page written.

On an x86 host with 4 KiB pages, put the default ring on hugetlbfs as well when the
program's own work is heavy on the TLB: the ring is then one 2 MiB page where it was 258,
and an event no longer finds its page gone from the TLB. After work that touched 20000
pages, the 99th percentile of a message of 7 events went from 160 ns over the untraced
message to 100 ns on Granite Rapids. `TRACE_RING_DIR=/dev/hugepages` is enough where that
mount is writable by the program's user and the host lets it take a surplus huge page
(`vm.nr_overcommit_hugepages`); no root is needed then. Where the kernel's pages are
64 KiB this gained nothing.

## The ring and the processor's caches

An event is a store into the ring. When the thread's core does not hold the ring's line
ready to be written, the store waits for the line in the store buffer. The program does
not wait, until it runs an atomic operation, a barrier or a system call: those wait for the
store buffer to empty, and so for the ring's line. That wait is 20 to 120 ns. It comes
after work that pushed the ring's lines out of the core's caches, and after `tracerd` read
them: a line it has read must be asked back before it is written again.

The tracer asks for the lines ahead of the events:

- `trace_end` asks for the lines of the next 4 to 8 events (`prefetchw` on x86,
  `prfm pstl3keep` on aarch64). A message of 7 events followed by an atomic was up to 57 ns
  shorter on Granite Rapids after work that had evicted the lines, 36 to 97 ns on a
  Neoverse V2. It costs nothing measurable in a tight loop; 1 to 3 ns per message when the
  program's working set fills its L2; and on Granite Rapids 5 to 12 ns per message after
  TLB-heavy work, when no atomic follows the message.
- On x86, `trace_now()` asks back the lines `tracerd` took, once every 2^21 timestamp ticks
  (0.5 to 1 ms), on a thread that attached. After 12 ms of empty polls, a message followed
  by an atomic took 120 ns more than the untraced one without this and 68 ns with it, as
  much as with no `tracerd` at all. An empty poll costs what it did, 12 ns. The one poll per
  period that asks takes 27 ns more, 40 to 47 ns after work that evicted the lines; a
  thread that calls `trace_now()` only once a message is there pays that ahead of a message.

Compile the program and `trace_attach.c` with `-DTRACE_NO_PREFETCH` to leave both out.

The ring's 1 MiB also passes through the core's own caches and pushes the program's data
out of them. On Granite Rapids a program whose working set filled 65 to 100% of its L2 did
22 to 98 ns more work per message of 7 events with the tracer than without; below 50%,
nothing. Compile the program with `-DTRACE_CLDEMOTE` and, on x86, `trace_begin_at` hands
the lines the events before it filled down to the shared cache (`cldemote`): that took
back two thirds to four fifths of the cost. It is off by default: an atomic or a barrier
less than about 100 ns after the `trace_begin_at` waits for the hand-down, 83 ns right
after it, 32 ns after 50 ns of work, nothing after 100 ns. Turn it on when the working set
is large and the message's first atomic comes later than that. It does nothing on aarch64.

What remains while `tracerd` drains:

- In a stream of events with no pause of a millisecond, an atomic or a barrier right after
  an event still waits for a few events in a thousand: the first event on each 4 KiB of
  the ring, and those written during a drain. On Granite Rapids an event and its atomic
  took 37 ns, and 120 ns at the 99.9th percentile where it was 48 ns with no `tracerd`.
- A burst of gauges costs 0.5 ns more per gauge on Granite Rapids, 1.7 ns on a Neoverse V2.
- Keep the ring at 16384 events or more. At 4096 events one event in ten made the atomic
  after it wait more than 80 ns, on both hosts; at 16384 and above, one in a hundred or fewer.

## Hardware counters

Include `src/trace_pmu.h` and call `trace_pmu_attach()` right after `trace_attach()`. It
does nothing unless `TRACE_PMU=1`.

- Start `tracerd --pmu-hold` first, at boot. Opening the host's first perf event sends a
  few IPIs to every CPU; the hold process takes that once, before anything is hot.
  Without it the thread refuses counters and the page says why.
- The thread reads its own counters inside `trace_now()`, one per period. The period is
  100,000 times the cost of a read: about 100 ms where reads trap (every VM measured),
  1 ms expected on bare metal.
- A trace never contains a counter read.
- Each event is tested on a small workload before it is kept. One that reads zero there is
  dropped, except for metrics the test cannot be expected to move: remote loads, last-level
  misses (the test does not reach a large L3), and dTLB walks on a kernel with pages larger
  than 4 KiB.
- On aarch64 with `kernel.perf_user_access=0` the thread refuses counters at once and the
  page names that setting.
- A thread without a poll loop calls `trace_sample()` at least once per period (the page
  prints it): called less often, its counter lanes come and go.
- A counter that runs through its range interrupts its core once: after 2^47 counts on
  x86, 2^63 on aarch64. That is hours apart for the fastest event, years for most. An
  event that could do so within an hour is refused.
- While a thread reads counters, leave `kernel.perf_user_access` (aarch64) and
  `/sys/bus/event_source/devices/cpu/rdpmc` (x86) alone. Turning user access off makes the
  thread's next read fault, and the thread dies.
