# Tried and discarded

Performance ideas that were built and measured, then not kept: the test, then what it
showed. Numbers come from isolated, tick-less CPU 2 of two KVM guests unless said otherwise:
GNR, an Intel Xeon 6975P-C (Granite Rapids), and V2, a Neoverse V2 with 64 KiB pages, both
gcc 11. Zen 4 is a shared, non-isolated EPYC 9454P guest: its numbers are indicative only.

The victim is a pointer chase over a working set of 32 KiB to 2 MiB (the L2), 7 events per
message. The drain test puts an atomic right after each event and a reader on another core
every 10 ms.

## The event

- **Non-temporal stores (`movnti`).** Victim, bench. With an `sfence` before each publish:
  150 to 300 ns per event. With one `sfence` per cache line: 43.7 ns per mark, p99 1.3 us
  on small working sets; it matches `cldemote` only at 80% of the L2 or more.
- **`cldemote` of the previous ring line as each line fills.** Victim; a burst followed by
  `mfence` or `lock add`. The ring's cost to the program fell 49 ns per message at 80% of
  the L2 and 74 ns at 100%. But an atomic or a fence after an event waits for the demote:
  68 to 128 ns with `mfence`, 67 to 129 ns with `lock add`. Kept instead, as the opt-in
  `TRACE_CLDEMOTE`: the demotes in `trace_begin_at`, a pair of lines at a time.
- **That opt-in without a record of the last line demoted** (the 2 lines behind, at every
  begin). A line demoted twice waits for the first demote: `trace_begin_at` in a loop went
  12.1 to 37.8 ns, a begin and an atomic 36 to 73 ns.
- **With an exact count of the lines behind.** Its branch mispredicts: +1.5 to 2 ns per
  message, +3 to 6 ns before a fence.
- **With a fixed 2 lines per begin.** Demotes some lines twice: messages of 4 and 5 events
  back to back went 33.3 to 42.1 ns and 33.8 to 40.7 ns.
- **The same demotes in `trace_begin` too.** A begin right before an atomic then waits, and
  a nested begin sits next to the send path.
- **`clflushopt` of the previous line.** Victim. The gain of `cldemote`, but 83 ns per
  event for unattached threads, and the lines go to DRAM.
- **`stnp`, `dc civac` (aarch64).** Victim. No pollution gain. `dc civac` costs 3 to 4 ns
  per message, p99 121 ns against 75.
- **`prefetchw` at every event, 64 B to 4 KiB ahead.** Bench, cold bursts. +0.16 ns per
  event in a tight loop; +1.5 to 3.4 ns per hot burst, no gain over the prefetch in
  `trace_end`.
- **The same prefetch in `trace_next`.** Bench. +1.0 ns per call on GNR, +0.56 on V2; a
  15-event message 7 to 8 ns slower.
- **The prefetch gated on the ring mask.** Bench. +0.76 ns per end; unattached threads lose
  nothing without the gate.
- **`PRFM PSTL1KEEP`, `PSTL2KEEP` (aarch64).** Cold bursts, victim. L1: 55 to 128 ns after
  sequential work. L2: the victim 48 to 114 ns slower per message at 100% of the L2.
  `PSTL3KEEP` is kept.
- **`prefetchw` in every `trace_now()`.** Empty poll turn: 12.06 to 13.09 ns. Replaced by a
  re-own every 0.78 ms on the periodic branch, which costs empty polls nothing.
- **Re-own one slot line instead of three.** Burst after 12 ms of polls and a drain: 99 ns
  against 67.
- **A store ahead of the cursor at every event.** Drain test. No gain on V2; 128-gauge
  bursts got worse, 203 and 621 ns.
- **`rdtsc` as `asm volatile`, volatile event stores (x86).** Assembly, bench. 18 fewer
  instructions per message, the same time; clang still reloads, and on aarch64 a lone
  gauge went 2.5 to 4.6 ns.
- **No private cursor copy.** Bench. GNR gauge 0.89 to 0.66 ns, but mark 12.84 to 13.34 and
  the demo message 75.8 to 77.9.
- **`rdtscp` or `lfence; rdtsc` for every timestamp.** Bench (Zen 4): 18.2 and 16.6 ns
  against 9.4. Only `trace_end` is fenced.
- **Local-exec TLS.** Assembly. One `mov` less per call; the tracer could no longer be
  loaded from a shared library.
- **Plain `str` of the cursor instead of `stlr` (aarch64).** Bench. Gauge 1.54 to 1.08 ns,
  nothing else moves; the cursor would no longer be ordered after the event.

## The ring

- **No shared cursor: a 16-bit lap in each event, the reader finds the head.** Torture of
  1e10 events per host, bench. Correct, with no torn event. GNR +0.25 to 0.75 ns per mark
  and +2 to 5 ns per 6-event message; V2 gauge 1.54 to 1.20 ns. The reader's copy takes
  twice as long, and the ring format breaks.
- **A default ring of 1024 or 4096 events.** Victim. On GNR 1024 removes the pollution and
  4096 half of it; no consistent gain on V2. 1024 events overrun above 77k events/s.
- **A 2 MiB ring through `MADV_COLLAPSE`.** Bench under TLB pressure (Zen 4). No gain, and
  2 MiB per ring. hugetlbfs is the documented way.
- **A private slot per unattached thread, on its first `trace_now()`.** 4 hot threads
  (Zen 4): 46.8 to 16.4 ns per event. The dummy slot on one cache line (25 to 27 ns) is
  kept as the simpler change.

## The drain

- **Copy only complete cache lines.** Drain test (GNR): 64.1 slow events per 10 ms against
  64 to 66.
- **Stop 32 lines short of the cursor, or copy downwards.** Same: at most 15% fewer slow
  events, x86 only, and the last 128 events wait a tick.
- **One cursor read per drain, the batch checked at the next.** Same: 59.3 slow events per
  10 ms, and half the ring a tick may use (1.94 M events lost at a 100 ms period).
- **`wall_ms` computed only for the slowest trace.** Drain benchmark: GNR +0.2%, V2 -1.0%.
- **No `hist->cnt` in the hot add.** Drain benchmark (Zen 4): 2% at best, 0.9 us more per
  slot close.

## The page and its stream

- **Binary frames, typed arrays over fetch.** Recorded stream, headless Chrome. 48% of the
  JSON bytes, as the dense JSON that is kept; no CPU gain; loses EventSource reconnect and
  `curl` recordings.
- **gzip per client.** Recorded stream. 10 to 13% of the bytes, for 9 to 20 us per frame
  per client, 256 KiB per client, and zlib.
- **Delta frames.** Recorded stream. 33% of the bytes; values came out wrong where replay
  and live frames meet.
- **`%.4g` or `%.6g` numbers.** 8 to 10% fewer bytes, lossy.
- **`writev`, `TCP_CORK`, zero-copy send.** strace, frame benchmark: nothing to gain.
- **One `Path2D` per colour.** Headless Chrome: +0.9 CPU-s/min.
- **Opaque canvases.** -0.43 CPU-s/min, all of it in software compositing.
- **`contain`, `will-change`; a top-8 selection instead of a sort.** Under 0.2 ms/s.

## Not kept, worth a later look

- **Re-own on aarch64**, with `PSTL2KEEP` on the periodic branch: a burst after a drain went
  92-119 to 59 ns, with empty polls unchanged at 12.13 ns. Only x86 was asked for.
