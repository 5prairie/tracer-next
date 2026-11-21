# Reading the page

`tracerd` serves it on `http://127.0.0.1:7070`. Everything on it is generated from what the
daemon found: the sites of the thread you picked, and the sources that could read this host.

## Top to bottom

| Part | Shows |
| --- | --- |
| Header | Host, clock resolution, the thread shown, the window (1 or 10 minutes) |
| Checks | What a sanitized thread should never see, each with its count, in a fixed place |
| Latency | The whole trace: median (white), 99th percentile (blue), maximum (dots), log scale |
| Lanes | One row per metric over time, on the chart's time axis. Amber or red past a threshold |
| Sites | Every site over the last second: p50, p99, max, calls |
| Slowest traces | The slowest trace of each second in the window. Click one |
| Trace | The spans of the picked trace, then what else happened in that second |
| Source cards | Current values per queue, socket, CPU or interrupt, by key. Quiet rows are dimmed |
| Not available here | What a source could not read on this host, and why |

## Finding the cause of a spike

1. The triangles over the chart mark the slowest traces. Click one, or a row of the list.
   A blue line then crosses the chart and every lane at that second.
2. Read "in that second" under the trace: counts that grew, levels past a threshold.
   What is past a threshold the whole window is listed apart: it is not this trace's.
3. Look along the blue line for a lane that moves. Put the pointer anywhere to read every
   lane's value at that instant.

Both windows move at every tick, 50 ms. In the 10-minute window the newest column is the
second still running, made from its ticks so far: its maximum is exact, its median and 99th
percentile near. The second's own numbers replace it when it ends. No panel changes height
with what it shows: a card keeps its rows, the slowest list its eight lines, the trace its
box. A trace with more spans than its thread has sites scrolls in that box.

Pause freezes the page on what it holds: the pointer, the picks, the thread and the window
still work on that data, and nothing newer comes in. Resuming reconnects, and the daemon
sends the window again.

A lane prints how fine it is: 50 ms for gauges; 100 ms for the ring, faults, interrupts
and sockets, more when a source proved costly on this host and was slowed down: a source whose
reads cost over a millisecond on average is read half as often, and again, until it costs
1 % of a CPU or less; one single read of over 10 ms slows it further. Stolen time and the
sibling's busy time are counted by the kernel in clock ticks and read once a second. Counters
are coarser where a read is expensive: the period times the number of counters. A counter
that has never counted has no lane. The notes at the bottom say, per counter metric, which
event was taken for it, or that the host gave none.

What is read once per process (sockets) or per core (interrupts) shows for every traced
thread of that process or on that core.

A trace runs from a root to the last end before the next root. With no span open, 20 ms of
silence ends it: spans that come later, before the next root, belong to no trace.

## The numbers

- **Per trace.** Miss counts (dTLB walks, L1D, L2, last-level, mispredicts, remote loads)
  are divided by the traces completed in the same interval. This takes a spin loop to make
  no miss, which is not measured. While no trace completes the lane shows a gap, and the
  misses of that time go to the next trace.
- **Per millisecond.** Stalls accrue while spinning too. They are shown per millisecond of
  the thread, spin included: read them as a trend.
- **Stall per miss.** Memory stall divided by last-level misses: what a miss costs. It
  rises when the memory system is contended. A gap where there was no miss to price, and
  at the first value after a lull; a thread that completes fewer than two traces a second
  gets none.
- **Checks.** Red at or above the red threshold. Two have an amber one below it. On a host
  that is not isolated several are red, truthfully.
- **Totals.** Counts such as involuntary switches start one second after `tracerd` found
  the thread. What starting up costs a thread (being moved to its core, its first page
  faults) is not counted against it.
- **Sockets.** The receive queue of a TCP socket is bytes left to read; of a UDP socket,
  the memory its waiting packets are charged, overhead included, which is what the kernel
  holds against the buffer's size. A socket the kernel cannot be asked for alone (two
  sockets sharing a port) is read sixteen times less often; its lane's name says so, in
  full when the pointer is on it. A process's sockets are looked for every 1.6 s among its
  first thousand fds: a socket bound after it was made shows within seconds if it is UDP
  or listens; a TCP socket connected late, within a minute if it was made less than a
  minute before, within eight otherwise.
- **NIC counters.** Read every 10 seconds, not more often: see below.

## What can reach a traced core

A traced thread in user space never waits for the tracer and is never interrupted by a
read. These are the exceptions, each rare or short, and what removes it.

- **`nic`.** Each read makes the kernel allocate and free a block. Freed blocks pile up
  until the kernel clears them with a TLB flush on every CPU: on x86, an interrupt on
  every core. With four interfaces on eight CPUs, read every 10 s, that is about one flush
  in eleven hours added to the host's own; more often in proportion for drivers with over
  512 counters. `--no-nic` removes it.
- **`faults`, and finding where a thread runs.** Reading a thread's `stat` file while it
  runs on a tickless (`nohz_full`) core takes that core's run-queue lock for a moment, on
  recent kernels (mainline 5.16 and later, and distribution kernels that carry the same
  change). No interrupt, and the thread in user space does not wait; the
  kernel on that core does if it enters the scheduler just then. The file is read ten times
  a second per thread; four of those reads are also the ones that find where it runs.
  `--no-faults` removes the other six.
- **`faults`, `sched`, and the process's signal lock.** Reading a thread's `stat` or
  `status` file takes the signal lock its process's threads share, and `status` the
  thread's own task lock as well, each for a moment. No interrupt, and a thread in user
  space does not wait. A thread that is in the kernel to change its signal mask, to take a
  signal or to arm a timer just then waits about a microsecond. `stat` is read ten times a
  second per thread (above). `status` is read once a second, and once more each time the
  thread has been switched in since the last read, ten times a second at the most: a
  thread that is never switched out costs itself one read a second. `--no-sched` removes
  the `status` reads.
- **`irq`.** Printing `/proc/interrupts` takes each device interrupt's lock in turn. A
  device interrupt arriving on a traced core just then waits a fraction of a microsecond.
  `--no-irq` removes it.
- **`sock`.** Finding a new socket that is neither UDP nor listening takes a walk of every
  TCP connection of the host, in two dumps of milliseconds each (5 ms here), seconds
  apart: time of the daemon, and for the traced thread at most a wait on a hash bucket's
  lock if it binds, connects or closes a socket of that bucket just then. A socket of a
  kind the kernel does not list here (a unix socket) is walked for too, a few times in its
  first minute and then every eight. No two walk dumps, of any process, come within two
  seconds. On a host where one dump takes over 10 ms, `sock` reads at twice its period
  for a few seconds after it; the card prints the period. Two or more traced processes
  with a thousand fds each slow it too. `--no-sock`.
- **Attaching late.** A thread that calls `trace_pmu_attach` while other threads of its
  process are already hot interrupts each of them a few times (three, measured on x86
  with a hold running; six without): once when its first counter is mapped, and a TLB
  flush when the self-test memory and each event it tried and dropped are unmapped.
  `trace_attach` alone was not seen to interrupt them; it takes the process's memory-map
  lock, which a hot thread that faults would wait for. Hence the rule: every thread
  attaches before any hot loop starts.
- **Counters, when the program turns them on.** A counter that runs through its range
  interrupts its core once: per 2^47 counts on x86, 2^63 on aarch64, hours apart at the
  fastest. The first perf event opened on an x86 host, and the last closed, send a few
  IPIs to every CPU: `tracerd --pmu-hold` at boot takes them once.
- **`uncore`, `mesh`, off by default.** Their events are opened at start, on the CPUs the
  hardware names; reading one interrupts its CPU. One on an isolated, tickless or traced
  CPU is not opened. One whose CPU gets a traced thread later is no longer read, and
  stays open. One whose CPU goes offline is read elsewhere by the kernel for up to five
  seconds, then no longer.
- **A ring directory on a disk.** The kernel writes the ring's pages back, and a store
  can wait for it. `trace_attach` warns. Keep rings on tmpfs or hugetlbfs.

"Core not isolated" reads the kernel's `isolcpus` list. A core set apart by a cpuset
partition alone shows red there though it is isolated.

## Options of `tracerd`

| Option | Meaning |
| --- | --- |
| `--listen 0.0.0.0:7070` | Serve the page elsewhere than on localhost |
| `--ring-dir DIR` | Look for rings in DIR as well as `/dev/shm` |
| `--cpu N` | Run on CPU N. Default: the first CPU that is neither isolated nor tickless |
| `--sources a,b` | Run only these sources |
| `--no-NAME` / `--with-NAME` | Turn one source off, or one that is off by default on |
| `--set irq.count.red=500` | Change a threshold: `source.metric.red` or `.amber` |
| `--seconds N` | Exit after N seconds |
| `--any-clock` | Run on a host whose timestamp counter is not invariant |
| `--probe` | Print what this host exposes and exit. With `--pmu`: counters too |
| `--probe --pmu --mib N` | Size of the counter workload, in MiB. Default 128 |
| `--probe --pmu --only a,b` | Probe only these counters: for hosts with few of them |
| `--pmu-hold` | Only hold one perf event open. Start it at boot if counters are wanted |
| `--help` | Print the options and the sources, and exit |

`--probe --pmu` and `--pmu-hold` open perf events. On x86 the first one on a host sends a
few IPIs to every CPU, and so does the last close. `--probe` alone removes nothing and
opens none.

The page can be open six times at once; a seventh says it was refused. `tracerd` follows
the rings of its own user; as root, of every user.

## Sources that are off by default

`numa`, `uncore` and `mesh` read what only a bare-metal, multi-node host has: pages taken
from the wrong node, what the memory controllers move, traffic between sockets. They have
not run on such a host yet. `--with-numa`, `--with-uncore` and `--with-mesh` turn them on;
the page then says they are unproven. `uncore` and `mesh` open perf events for the whole
host and need `kernel.perf_event_paranoid` at 0 or lower. They open them when `tracerd`
starts: start it before the traced program only if no traced thread will run on a CPU
those events are read on (the first CPU of each socket, usually).

## A recording

`curl -N http://127.0.0.1:7070/stream > run.txt` records the stream. To see that run
again, put `run.txt` beside the files of `ui/`, serve that directory with any web server,
and open `index.html?replay=run.txt`. Browsers do not load the page's scripts from a plain
file.
