// state.js: the page's one state object and what every panel asks of it: the window shown,
// the histories behind it, the second picked, the pointer, a redraw. It imports no panel,
// so the panels and main.js, which wires them, can all import it.

export const state = {
  hello: null,
  threads: new Map(),     // thread index -> { info, fast, slow, slowest, lastSecond, idle }
  instances: new Map(),   // instance index -> { info, metric, source, fast, slow, value }
  threadList: [],         // the threads and the instances in hello's order: a frame's "t"
  instanceList: [],       // and "v" name nothing, their numbers are in this order
  tables: -1,             // which threads and instances hello lists: main.js tablesOf
  waiting: [],            // frames built from other tables: ahead of their hello
  // The lanes in page order: { instance, row, canvas, valueCell, isOut, shown, shownMetric }
  lanes: [],
  signatures: new WeakMap(),  // a box of the document -> what it was last built from
  sizes: new WeakMap(),       // a box -> its CSS size, kept by draw.js's ResizeObserver
  cardColumn: new Map(),      // source index -> the column its card is in
  cardColumnCnt: 0,           // the number of columns cardColumn is for
  slot: 0,                // the newest slot received; one lasts hello.slot_ms
  second: 0,              // the newest second received
  liveSecond: -1,         // the second the ticks are in, before its frame: main.js fillLiveSecond
  threadIdx: -1,          // the thread shown
  windowKind: 'fast',     // 'fast': hello.fast_cnt slots. 'slow': hello.slow_cnt seconds
  paused: false,
  crosshair: null,        // 0..1 across the time axis, or null
  pickedSecond: null,     // the second whose slowest trace is shown; null follows the slowest
  costs: [],              // per source: [cost in us, period in ms]
  streamState: '',
  slowVersion: 0,         // bumped when the once-a-second panels have something new
  slowDrawn: -1,
  traceDrawn: '',         // what the trace panel was last drawn from
  draw: null,             // draws every panel; main.js sets it, the panels only ask for it
  drawPending: false,
  plotPending: false,     // the next draw is of the panels too, not of the pointer's line only
  // What rankedSeconds() last returned, and what it was computed from.
  ranked: { thread: null, version: -1, oldest: 0, newest: 0, seconds: [] },
};

export const STAT_COLUMNS = 4;  // p50, p99, max, count: of a site, or of the whole trace
export const P50 = 0, P99 = 1, MAX = 2, COUNT = 3;  // the columns of a stat
export const SECOND_MS = 1000;
const TICK_STEPS_S = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
const TICK_MIN = 5;  // a window shows at least this many wall-clock ticks

// One redraw per animation frame, however many asked for it. A pointer that only moved
// asks for its line and the lanes' values, not for the panels.
export function scheduleDraw(isPointerOnly = false) {
  if (!isPointerOnly) state.plotPending = true;  // before the return: a draw may be pending
  if (state.drawPending) return;
  state.drawPending = true;
  requestAnimationFrame(() => {
    state.drawPending = false;
    state.draw();
  });
}

// A window as indexes: how many columns, the first and the last, the last that is whole,
// and how long one lasts. In the slow window the live second is made of the ticks so far
// (main.js fillLiveSecond): its maximum only grows to the second's, but its median and its
// lowest values are not the second's. What is read from the whole window stops at
// `settled`, or would change at every second's start, and back.
export function windowOf(kind = state.windowKind) {
  const hello = state.hello, isFast = kind === 'fast';
  const count = isFast ? hello.fast_cnt : hello.slow_cnt;
  const end = isFast ? state.slot : Math.max(state.second, state.liveSecond);
  return { kind, count, end, first: end - count + 1, settled: isFast ? end : state.second,
           stepMs: isFast ? hello.slot_ms : SECOND_MS };
}

// The history of a thread or an instance that the current window shows.
export const seriesOf = (owner) => (state.windowKind === 'fast' ? owner.fast : owner.slow);

// The column of a window under a fraction, 0..1, of its time axis.
export const indexAt = (view, fraction) => view.first + Math.round(fraction * (view.count - 1));

// Is something filed under this thread on the page? What belongs to the host (below 0)
// always is. What a source reads once per process or per core is filed under one thread of
// it, and belongs to every thread of that process or on that core.
export function isShownThread(threadIdx, scope) {
  if (threadIdx < 0 || threadIdx === state.threadIdx) return true;
  const owner = state.threads.get(threadIdx), shown = state.threads.get(state.threadIdx);
  if (!owner || !shown) return false;
  if (scope === 'process') return owner.info.pid === shown.info.pid;
  return scope === 'core' && owner.info.cpu === shown.info.cpu;
}

// The second a slot falls in, and the first slot of a second.
export const secondOfSlot = (slot) => Math.floor(slot * state.hello.slot_ms / SECOND_MS);
export const firstSlotOf = (second) => second * SECOND_MS / state.hello.slot_ms;

// The second a column of the current window falls in.
const secondOf = (index) => (state.windowKind === 'slow' ? index : secondOfSlot(index));

// Where a second sits across the current window, 0..1; outside the window it is outside 0..1.
export function fractionOfSecond(second) {
  const view = windowOf();
  const index = view.kind === 'slow' ? second : (second + 0.5) * SECOND_MS / view.stepMs;
  return (index - view.first) / (view.count - 1);
}

// The seconds of the current window that have a slowest trace, slowest first. The ranking
// is kept until what it is made from changes: every write to thread.slowest bumps
// slowVersion (onSample for a second, onHello). Callers only read it.
export function rankedSeconds(thread) {
  const view = windowOf(), kept = state.ranked;
  const newest = secondOf(view.end), oldest = secondOf(view.first);
  if (kept.thread !== thread || kept.version !== state.slowVersion ||
      kept.oldest !== oldest || kept.newest !== newest) {
    const seconds = [...thread.slowest.keys()]
      .filter((second) => second >= oldest && second <= newest)
      .sort((left, right) => thread.slowest.get(right).ns - thread.slowest.get(left).ns);
    state.ranked = { thread, version: state.slowVersion, oldest, newest, seconds };
  }
  return state.ranked.seconds;
}

// The second whose slowest trace is shown: the one picked, else the slowest of the window.
export function shownSecond() {
  const thread = state.threads.get(state.threadIdx);
  if (!thread) return null;
  if (state.pickedSecond !== null && thread.slowest.has(state.pickedSecond)) {
    return state.pickedSecond;
  }
  const ranked = rankedSeconds(thread);
  return ranked.length ? ranked[0] : null;
}

// How far apart the wall-clock ticks of a window are: the longest round step that still
// leaves TICK_MIN of them.
function tickStepMs(view) {
  const spanS = view.count * view.stepMs / SECOND_MS;
  const fitting = TICK_STEPS_S.filter((stepS) => spanS >= TICK_MIN * stepS);
  return (fitting.length ? fitting[fitting.length - 1] : TICK_STEPS_S[0]) * SECOND_MS;
}

// The vertical marks of the current window: wall-clock ticks and the picked second.
export function timeMarks() {
  const view = windowOf(), hello = state.hello, stepMs = tickStepMs(view);
  const wallOf = (index) => hello.wall_ms + index * view.stepMs - hello.slot * hello.slot_ms;
  const firstMs = wallOf(view.first), spanMs = (view.count - 1) * view.stepMs, ticks = [];
  for (let wallMs = Math.ceil(firstMs / stepMs) * stepMs; wallMs <= wallOf(view.end);
    wallMs += stepMs) {
    ticks.push({ fraction: (wallMs - firstMs) / spanMs, wallMs });
  }
  const shown = shownSecond();
  const picked = shown === null ? null : fractionOfSecond(shown);
  return { ticks, picked };
}

// The pointer over a time axis. `fractionOf(event)` says where across it the pointer is:
// 0..1, null off the plot, undefined when the event is not over an axis at all. A click
// picks the second under the pointer, whose slowest trace is then shown.
export function watchCrosshair(box, fractionOf) {
  box.addEventListener('mousemove', (event) => {
    const fraction = fractionOf(event);
    if (fraction === undefined) return;
    state.crosshair = fraction;
    scheduleDraw(true);
  });
  box.addEventListener('mouseleave', () => { state.crosshair = null; scheduleDraw(true); });
  box.addEventListener('click', () => {
    if (state.crosshair === null) return;
    const second = secondOf(indexAt(windowOf(), state.crosshair));
    const thread = state.threads.get(state.threadIdx);
    state.pickedSecond = thread && thread.slowest.has(second) ? second : null;  // no trace: no pick
    scheduleDraw();
  });
}

// A fixed-length history addressed by slot or second index. `at` remembers which number
// each cell holds, so that a cell left from a lap ago reads as empty.
export function history(count, columns) {
  const cells = [];
  for (let i = 0; i < columns; i++) cells.push(new Float32Array(count));
  return { count, at: new Float64Array(count).fill(-1), cells };
}

export function historyGet(series, column, index) {
  const rest = index % series.count;
  const cell = rest < 0 ? rest + series.count : rest;
  return series.at[cell] === index ? series.cells[column][cell] : NaN;
}

export function historySet(series, index, values) {
  const cell = index % series.count;
  series.at[cell] = index;
  for (let i = 0; i < values.length; i++) series.cells[i][cell] = values[i];
}

// Empty a cell, if it holds this index.
export function historyDrop(series, index) {
  const cell = index % series.count;
  if (series.at[cell] === index) series.at[cell] = -1;
}

// One value into a history of one column: no array is made to carry it.
export function historyPut(series, index, value) {
  const cell = index % series.count;
  series.at[cell] = index;
  series.cells[0][cell] = value;
}
