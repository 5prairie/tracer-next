#!/bin/bash
# hostrun.sh SPIN_CPU [pmu]: is a traced core disturbed? Spins a thread on SPIN_CPU alone, then with
# tracerd watching from a housekeeping CPU; "pmu" adds a run with counters on. Stops if a
# thread of PROD_USER, when one is named, shares the core. Settings are in the environment.
set -u
[ $# -ge 1 ] || { echo "usage: hostrun.sh SPIN_CPU [pmu]" >&2; exit 2; }
SPIN_CPU=$1
WANT_PMU=${2:-}
PROD_USER=${PROD_USER:-}       # a user whose threads must not share the core; empty: no check
HOUSEKEEPING_CPU=${HOUSEKEEPING_CPU:-0}
SPIN_SECONDS=${SPIN_SECONDS:-10}
MARK_EVERY=${MARK_EVERY:-64}   # loop turns per event: 64 is about 1.3 M events per second
PORT=${PORT:-7979}
TRACERD_OPTIONS=${TRACERD_OPTIONS:-}
cd "$(dirname "$0")/.." || exit 1

SIBLINGS_FILE=/sys/devices/system/cpu/cpu$SPIN_CPU/topology/thread_siblings_list
SIBLINGS=$(tr ',-' '  ' < "$SIBLINGS_FILE")
OUT=$(mktemp -d)  # the runs' output: shown, then removed
hold=""           # the counter hold, when one runs: it must not outlive this script
trap '[ -n "$hold" ] && kill "$hold" 2> /dev/null; rm -rf "$OUT"' EXIT
echo "spin cpu $SPIN_CPU, hyperthread siblings: $SIBLINGS"

prod_threads() {
  [ -n "$PROD_USER" ] || return 0
  for cpu in $SIBLINGS; do
    ps -eLo user:32,psr,comm | awk -v cpu="$cpu" -v user="$PROD_USER" '$1 == user && $2 == cpu'
  done | head -3
}

spin() {  # label, "watched" or "", TRACE_PMU value
  if [ -n "$(prod_threads)" ]; then echo "STOP: a $PROD_USER thread is on cpu $SIBLINGS"; exit 3; fi
  if [ -n "$2" ]; then
    # shellcheck disable=SC2086
    taskset -c "$HOUSEKEEPING_CPU" timeout $((SPIN_SECONDS + 20)) ./build/tracerd \
      --listen "127.0.0.1:$PORT" --seconds $((SPIN_SECONDS + 4)) --cpu "$HOUSEKEEPING_CPU" \
      $TRACERD_OPTIONS > "$OUT/tracerd_$1.txt" 2>&1 &
    tracerd_pid=$!
    sleep 1
  fi
  TRACE_PMU=$3 timeout $((SPIN_SECONDS + 30)) ./build/disturb "$SPIN_CPU" "$SPIN_SECONDS" \
    "$MARK_EVERY" > "$OUT/spin_$1.txt" 2>&1
  [ -n "$2" ] && wait "$tracerd_pid"  # not a bare wait: the counter hold is a job too
  echo "== $1"
  cat "$OUT/spin_$1.txt"
  [ -n "$2" ] && cat "$OUT/tracerd_$1.txt"
}

spin alone "" 0
spin watched watched 0
if [ "$WANT_PMU" = pmu ]; then
  taskset -c "$HOUSEKEEPING_CPU" timeout $((SPIN_SECONDS + 30)) ./build/tracerd --pmu-hold \
    > "$OUT/hold.txt" 2>&1 &
  hold=$!
  sleep 2
  spin watched_counters watched 1
  kill "$hold"
  wait
  hold=""
  cat "$OUT/hold.txt"
fi
exit 0
