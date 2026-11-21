// main.js: the page's entry and its loop. Frames from the stream update the state; a
// change schedules one redraw of every panel. Nothing here knows what a source is about:
// panels are built from the tables the daemon sends in "hello".

import { connect } from './stream.js';
import {
  state, windowOf, shownSecond, history, historyGet, historySet, historyPut, historyDrop,
  secondOfSlot, firstSlotOf, scheduleDraw, STAT_COLUMNS, P50, P99, MAX, COUNT,
} from './state.js';
import { formatSpan, replaceIfChanged } from './draw.js';
import { drawLatency, drawHairline, drawSites, watchLatency } from './latency.js';
import { drawSlowest, drawTrace, watchSlowest } from './traces.js';
import { buildLanes, drawLanes, drawLaneValues, watchLanes } from './lane.js';
import { drawTables } from './table.js';
import { drawChecks } from './check.js';

const CLOCK_COARSE_NS = 10;  // a timestamp step above this is worth a warning
const NO_STAT = [0, 0, 0, 0];  // p50, p99, max, count of what did not run
const WAITING_MAX = 256;       // frames kept for a hello to come: a few can be ahead of it

// Which threads and instances a hello lists, in order, as one number. The daemon computes
// the same from its tables (td_frame.c td_frame_tables) and sends it in every frame as "g".
function tablesOf(hello) {
  let sum = 0;
  const add = (number) => { sum = (Math.imul(sum, 31) + number) | 0; };
  for (const info of hello.threads) { add(info.i); add(info.tid); add(info.sites.length); }
  for (const info of hello.instances) {
    add(info.i); add(info.source); add(info.metric); add(info.thread);
  }
  return sum >>> 0;
}

function onHello(hello) {
  const threads = new Map(), instances = new Map(), kept = new Set();
  if (state.hello && hello.slot < state.slot) {  // the daemon's clock began again: a reboot
    state.slot = 0;
    state.second = 0;
    state.liveSecond = -1;
    state.threads = new Map();
    state.instances = new Map();
  }
  for (const info of hello.threads) {
    const old = state.threads.get(info.i);
    const same = old && old.info.pid === info.pid && old.info.tid === info.tid &&
                 old.info.sites.length === info.sites.length;
    if (same) kept.add(info.i);
    // The histories are of the whole trace: a site is shown for the last second only.
    // `idle` is what a second in which the thread did nothing leaves in lastSecond.
    threads.set(info.i, same ? { ...old, info } : {
      info, slowest: new Map(), lastSecond: null,
      fast: history(hello.fast_cnt, STAT_COLUMNS), slow: history(hello.slow_cnt, STAT_COLUMNS),
      idle: Array(info.sites.length + 1).fill(NO_STAT),
    });
  }
  for (const info of hello.instances) {
    const old = state.instances.get(info.i);
    // The same slot can come to hold another thread: its instances are then not the old ones.
    const same = old && old.info.source === info.source && old.info.metric === info.metric &&
                 old.info.thread === info.thread && old.info.key === info.key &&
                 (info.thread < 0 || kept.has(info.thread));
    const source = hello.sources[info.source];
    instances.set(info.i, same ? { ...old, info } : {
      info, source, metric: source.metrics[info.metric], value: NaN,
      fast: history(hello.fast_cnt, 1), slow: history(hello.slow_cnt, 1),
    });
  }
  if (!kept.has(state.threadIdx)) state.pickedSecond = null;  // the pick was another thread's
  state.hello = hello;
  state.threads = threads;
  state.instances = instances;
  state.threadList = hello.threads.map((info) => threads.get(info.i));
  state.instanceList = hello.instances.map((info) => instances.get(info.i));
  state.tables = tablesOf(hello);
  if (!threads.has(state.threadIdx)) {
    const live = hello.threads.find((info) => !info.gone) || hello.threads[0];
    state.threadIdx = live ? live.i : -1;
  }
  state.slowVersion++;
  buildHeader();
  buildLanes();
}

// Forget the slowest traces at or before a second: they left the slow window.
function forgetSlowest(oldest) {
  for (const thread of state.threads.values()) {
    for (const second of thread.slowest.keys()) {
      if (second > oldest) continue;
      thread.slowest.delete(second);
      const isPicked = thread.info.i === state.threadIdx && second === state.pickedSecond;
      if (isPicked) state.pickedSecond = null;  // back to following the slowest
    }
  }
}

// The threads of a frame. A tick has "t": per thread in hello's order, the whole trace's
// [p50, p99, max, count], or null when the thread did nothing. A second has "threads":
// each with its index, the stats of every site then the trace's, and its slowest trace; a
// thread it does not list did nothing. A recording of before "t" has "threads" in its
// ticks too. Nothing is written for a thread that did nothing: its cell reads as empty.
function onThreads(frame, isFast, isNewest) {
  const index = frame.index;
  if (frame.t) {
    const threads = state.threadList, stats = frame.t;
    for (let i = 0; i < threads.length; i++) {
      if (stats[i]) historySet(threads[i].fast, index, stats[i]);
    }
    return;
  }
  const oldest = Math.max(index, state.second) - state.hello.slow_cnt;  // the window's edge
  if (!isFast && isNewest) {  // the sites table is of this second: also for who is not in it
    for (const thread of state.threads.values()) thread.lastSecond = thread.idle;
  }
  for (let i = 0; i < frame.threads.length; i++) {
    const sample = frame.threads[i], thread = state.threads.get(sample.i);
    if (!thread) continue;
    const lat = sample.lat;
    historySet(isFast ? thread.fast : thread.slow, index, lat[lat.length - 1]);
    if (isFast) continue;
    if (sample.slowest && index > oldest) thread.slowest.set(index, sample.slowest);
    if (isNewest) thread.lastSecond = lat;
  }
  if (!isFast && index > state.second) forgetSlowest(oldest);  // a new second moves the window
}

// The values of a frame. "v" is one per instance in hello's order, null where there is
// none. A recording of before "v" has "values": [instance index, value] pairs.
function onValues(frame, isFast, isLive) {
  const index = frame.index;
  if (frame.v) {
    const instances = state.instanceList, values = frame.v;
    for (let i = 0; i < instances.length; i++) {
      const instance = instances[i], value = values[i];
      if (value !== null) historyPut(isFast ? instance.fast : instance.slow, index, value);
      if (isLive) instance.value = value === null ? NaN : value;
    }
    return;
  }
  if (isLive) {
    for (const instance of state.instances.values()) instance.value = NaN;
  }
  for (let i = 0; i < frame.values.length; i++) {
    const pair = frame.values[i], instance = state.instances.get(pair[0]);
    if (!instance) continue;
    historyPut(isFast ? instance.fast : instance.slow, index, pair[1]);
    if (isLive) instance.value = pair[1];
  }
}

// What fillLiveSecond works in, made once: it runs at every tick. A second has 20 slots.
const live = { p50s: [], counts: [], stat: new Array(STAT_COLUMNS) };

// The least of the first `length` values with half the counts at or under it. Of 20 values
// at most: comparing each with each makes no array, as a sort would.
function weightedMedian(values, counts, length, total) {
  let median = NaN;
  for (let i = 0; i < length; i++) {
    let under = 0;
    for (let j = 0; j < length; j++) if (values[j] <= values[i]) under += counts[j];
    if (2 * under >= total && !(values[i] >= median)) median = values[i];
  }
  return median;
}

// The second the ticks are in has no frame until it ends: the slow window would move once a
// second. Its column is made from its ticks so far, at every tick, and its frame replaces
// it. A thread's maximum and count are exact. Its median is the median of its slots'
// medians, weighted by their counts, and its 99th percentile the geometric mean of its
// slots' 99th, weighted the same: both close to the second's on the demo. The slots' 99th
// percentile of their 99th would not be: it is the one bad slot's, several times the
// second's. An instance's value is its slots' largest, as the daemon makes a second's
// (td_store_close_second). Every slot of the second is read again at every tick: a tick
// that comes late or twice changes nothing.
function fillLiveSecond() {
  const second = secondOfSlot(state.slot);
  if (second <= state.second) return;
  if (second !== state.liveSecond) dropLiveSecond();
  state.liveSecond = second;
  const first = firstSlotOf(second), last = state.slot;
  const { p50s, counts, stat } = live;
  for (const thread of state.threadList) {
    let length = 0, max = 0, count = 0, logP99 = 0;
    for (let slot = first; slot <= last; slot++) {
      const slotCount = historyGet(thread.fast, COUNT, slot);
      if (!(slotCount > 0)) continue;
      p50s[length] = historyGet(thread.fast, P50, slot);
      counts[length++] = slotCount;
      logP99 += Math.log(Math.max(1, historyGet(thread.fast, P99, slot))) * slotCount;
      max = Math.max(max, historyGet(thread.fast, MAX, slot));
      count += slotCount;
    }
    if (!count) { historyDrop(thread.slow, second); continue; }
    stat[P50] = weightedMedian(p50s, counts, length, count);
    stat[P99] = Math.exp(logP99 / count);
    stat[MAX] = max;
    stat[COUNT] = count;
    historySet(thread.slow, second, stat);
  }
  for (const instance of state.instanceList) {
    let max = NaN;
    for (let slot = first; slot <= last; slot++) {
      const value = historyGet(instance.fast, 0, slot);
      if (!Number.isNaN(value) && !(value <= max)) max = value;
    }
    if (Number.isNaN(max)) historyDrop(instance.slow, second);
    else historyPut(instance.slow, second, max);
  }
}

// What fillLiveSecond wrote is not the second's: its frame, or nothing, replaces it.
function dropLiveSecond() {
  const second = state.liveSecond;
  if (second < 0) return;
  state.liveSecond = -1;
  for (const thread of state.threads.values()) historyDrop(thread.slow, second);
  for (const instance of state.instances.values()) historyDrop(instance.slow, second);
}

// A tick carries one slot, a second one second: into the fast history, or the slow one.
// What a frame says in hello's order ("t", "v") is of the tables its "g" names. A frame
// that is ahead of its hello waits for it: frames may come in any order.
function onSample(frame, isFast) {
  if (frame.v) {
    if (!state.hello || frame.g !== state.tables) {
      if (state.waiting.length < WAITING_MAX) state.waiting.push(frame);
      return;
    }
    const isWhole = frame.v.length === state.instanceList.length &&
                    (!frame.t || frame.t.length === state.threadList.length);
    if (!isWhole) return;
  }
  if (!state.hello) return;
  const isNewest = frame.index >= (isFast ? state.slot : state.second);
  if (!isFast && frame.index >= state.liveSecond) dropLiveSecond();
  onThreads(frame, isFast, isNewest);
  onValues(frame, isFast, isFast && isNewest);  // a live tick: what it lacks has no value
  if (isFast) state.slot = Math.max(state.slot, frame.index);
  else state.second = Math.max(state.second, frame.index);
  if (isFast && isNewest) fillLiveSecond();
  if (frame.sources) state.costs = frame.sources;
  if (!isFast) state.slowVersion++;
}

// The stream's switches: close() and open(). null for a recording, which has no stream.
let stream = null;

function onFrame(frame) {
  // A paused page is frozen: its stream is closed, and a frame that still comes changes
  // nothing. A recording comes once: it is taken in, and drawn when the page is resumed.
  if (state.paused && stream) return;
  if (frame.type === 'hello') {
    onHello(frame);
    const waiting = state.waiting;  // those that were ahead of this hello are now read
    state.waiting = [];
    for (const held of waiting) onSample(held, held.type === 'tick');
    state.waiting = [];  // what is still of other tables is of no hello to come
  } else {
    onSample(frame, frame.type === 'tick');
  }
  // Every frame is drawn, in either window: a tick moves the slow window's live second too.
  // No frame makes a paused page draw: it draws when the pointer or a pick asks.
  if (!state.paused) scheduleDraw();
}

// What drawAll keeps from one draw of the panels to the next: the slowest script since it
// last told one, and when and under which stream state it told it.
const SCRIPT_TOLD_MS = 1000;
const told = { worstMs: 0, atMs: 0, streamState: null };

function drawAll() {
  const streamCell = document.getElementById('stream-state');
  // All a page has to say before its first frame. A paused page draws like any other, from
  // what it had when it was paused: the pointer's line, a pick and its trace still work.
  if (!state.hello) {
    if (streamCell.textContent !== state.streamState) streamCell.textContent = state.streamState;
    return;
  }
  if (state.plotPending) {  // not when only the pointer moved
    state.plotPending = false;
    const began = performance.now();
    drawLatency();
    drawLanes();
    drawSlowest();
    // The trace panel depends on the trace shown and on what a second brings.
    const traceKey = [shownSecond(), state.pickedSecond, state.slowVersion, state.windowKind,
      state.threadIdx].join('|');
    if (traceKey !== state.traceDrawn) {
      state.traceDrawn = traceKey;
      drawTrace();
    }
    if (state.slowDrawn !== state.slowVersion) {  // these change once a second at most
      state.slowDrawn = state.slowVersion;
      drawSites();
      drawTables();
      drawChecks();
    }
    // How long the panels' script took: the slowest draw of the last second, told once a
    // second, and at once when the stream's state changes. It is not what a frame costs:
    // the browser's layout, its paint and the canvases' rasterisation come after this, and
    // are several times it. Told at every tick, the text would itself be a layout and a
    // repaint at every tick. Paused or not, a draw of the panels counts the same.
    const now = performance.now();
    told.worstMs = Math.max(told.worstMs, now - began);
    if (now - told.atMs >= SCRIPT_TOLD_MS || state.streamState !== told.streamState) {
      streamCell.textContent = `${state.streamState} - script ${told.worstMs.toFixed(1)} ms`;
      told.atMs = now;
      told.streamState = state.streamState;
      told.worstMs = 0;
    }
  }
  drawHairline();
  drawLaneValues();
}

// The host, the threads to choose from, and how long each of the two windows is.
function buildHeader() {
  const hello = state.hello, select = document.getElementById('thread');
  const coarse = hello.tsc_ns > CLOCK_COARSE_NS ? ' (coarse)' : '';
  document.getElementById('host').textContent = `${hello.host} - ${hello.arch} - ` +
    `${hello.kernel} - clock ${hello.tsc_ns.toFixed(2)} ns${coarse}`;
  const labels = hello.threads.map((info) =>
    `${info.name} (cpu ${info.cpu < 0 ? '?' : info.cpu}${info.gone ? ', gone' : ''})`);
  replaceIfChanged(select, `${labels.join('|')}|${state.threadIdx}`, () =>
    hello.threads.map((info, i) =>
      new Option(labels[i], info.i, false, info.i === state.threadIdx)));
  for (const button of document.getElementById('window').children) {
    const view = windowOf(button.dataset.window);
    button.textContent = formatSpan(view.count * view.stepMs);
  }
}

function start() {
  const pause = document.getElementById('pause');
  // Paused, the page is frozen at that instant: the stream is closed, so nothing comes in
  // and nothing is overwritten, and the pointer, a pick, another thread, the other window
  // all read what is there. Resumed, it opens the stream again and the daemon sends its
  // history as to a new page: that fills the time the pause lasted. Nothing is emptied
  // first: a cell of the window is written again with what it held or was empty, and what
  // is older reads as empty by its index. A recording has no stream: its pause freezes
  // nothing more than it already is, and resuming brings nothing in.
  const setPaused = (paused) => {
    if (paused === state.paused) return;
    state.paused = paused;
    pause.classList.toggle('on', paused);
    if (stream && paused) {
      stream.close();
      state.streamState = 'paused';
    } else if (stream) {
      state.waiting = [];
      state.streamState = 'reconnecting';
      stream.open();
    }
    scheduleDraw();
  };
  document.getElementById('thread').addEventListener('change', (event) => {
    state.threadIdx = Number(event.target.value);
    state.pickedSecond = null;
    state.slowVersion++;
    buildLanes();    // paused too: the other thread at the same instant
    scheduleDraw();
  });
  document.getElementById('window').addEventListener('click', (event) => {
    if (!event.target.dataset.window) return;
    state.windowKind = event.target.dataset.window;
    for (const button of event.currentTarget.children) {
      button.classList.toggle('on', button === event.target);
    }
    scheduleDraw();  // paused too: the other window of the same instant
  });
  pause.addEventListener('click', () => setPaused(!state.paused));
  state.draw = drawAll;
  watchLatency();
  watchSlowest();
  watchLanes();
  stream = connect(onFrame, (text) => { state.streamState = text; scheduleDraw(); });
}

start();
