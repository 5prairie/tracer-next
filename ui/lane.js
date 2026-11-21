// lane.js: one thin chart per lane metric of every active source, stacked under the
// latency chart on the same time axis. A lane is a row of bars coloured by the metric's
// thresholds, so a red bar sits exactly under the latency it may explain.

import {
  state, windowOf, seriesOf, indexAt, isShownThread, timeMarks, historyGet, watchCrosshair,
  scheduleDraw,
} from './state.js';
import {
  COLOR, formatValue, stateOf, fitCanvas, drawTimeLines, element, replaceIfChanged, setText,
  setClass,
} from './draw.js';

// The lane instances of the thread shown, and those of the host, in source order.
function laneInstances() {
  return [...state.instances.values()]
    .filter((instance) => instance.metric.widget === 'lane' && instance.source.on &&
                          isShownThread(instance.info.thread, instance.source.scope))
    .sort((left, right) => left.info.source - right.info.source ||
                           left.info.metric - right.info.metric ||
                           left.info.key.localeCompare(right.info.key));
}

// Which lanes are in sight. One scrolled out of the lanes' box, or out of the window, is
// not drawn and its value is not written: there can be more lanes than the box shows. When
// one comes back a draw is asked for: it shows its older picture until that draw, the next
// frame of a page that is drawing.
const laneOfRow = new WeakMap();
const inSight = new IntersectionObserver((entries) => {
  let isBack = false;
  for (const { target, isIntersecting } of entries) {
    const lane = laneOfRow.get(target);
    if (!lane) continue;
    isBack ||= lane.isOut && isIntersecting;
    lane.isOut = !isIntersecting;
  }
  if (isBack) scheduleDraw();
});

// One lane: its name, how fine its values are (the daemon says), its chart, and its value.
// Then what the drawing keeps of it: whether it is out of sight, and the value last written
// with the metric it was judged by.
function buildLane(instance) {
  const row = element('div', 'lane');
  const title = instance.metric.label || instance.source.name;
  const label = [title.startsWith(instance.source.name) ? '' : instance.source.name, title,
    instance.info.key].filter((part) => part).join(' - ');
  const name = element('span', 'name', label);
  const canvas = element('canvas');
  const valueCell = element('span', 'value');
  name.title = `${label} - ${instance.info.resolution_ms} ms`;  // whole, when the cell cuts it
  name.append(element('span', 'period', `${instance.info.resolution_ms} ms`));
  row.append(name, canvas, valueCell);
  return { instance, row, canvas, valueCell, isOut: false, shown: NaN, shownMetric: null };
}

// Rebuild the rows when the set of lanes changed: new instances, or another thread shown.
// On a paused page too: it takes in no hello, but another thread can be picked, and its
// lanes are drawn from what the page had.
export function buildLanes() {
  const instances = laneInstances();
  const signature = instances
    .map(({ info }) => [info.i, info.source, info.metric, info.key, info.resolution_ms].join(':'))
    .join('|');
  replaceIfChanged(document.getElementById('lanes'), `${state.threadIdx}|${signature}`, () => {
    for (const lane of state.lanes) inSight.unobserve(lane.row);  // the rows that go
    state.lanes = instances.map(buildLane);
    for (const lane of state.lanes) {
      laneOfRow.set(lane.row, lane);
      inSight.observe(lane.row);
    }
    return state.lanes.map((lane) => lane.row);
  });
  for (const lane of state.lanes) {  // the instance objects are new after every hello
    lane.instance = state.instances.get(lane.instance.info.i) || lane.instance;
  }
}

// One set of listeners for all lanes: the pointer's position is a fraction of any of them.
export function watchLanes() {
  watchCrosshair(document.getElementById('lanes'), (event) => {
    if (event.target.tagName !== 'CANVAS') return null;  // on a name or a value: off the plot
    return event.offsetX / Math.max(1, event.target.clientWidth - 1);
  });
}

// The bars of one lane. Neighbours of one height and colour are filled as one rectangle,
// the union of theirs: a lane that barely moves is a few fills, not one per column. A
// height is a whole number of device pixels, so that "one height" is what the eye sees:
// compared as computed, two values a hundredth of a pixel apart would never join. A column
// with no value has no colour and ends a run; so does the step past the last column. The
// run is ended in the loop, not in a function: its bounds stay plain local numbers.
function drawBars(context, series, metric, view, width, height, low, high) {
  const barWidth = Math.max(1, width / view.count);
  const isFlat = !(high > low);
  const density = window.devicePixelRatio || 1;
  let runLeft = 0, runRight = 0, runHeight = 0, runColor = null;
  let fill = null;  // the colour last set on this lane's context
  for (let i = 0; i <= view.count; i++) {
    const value = i < view.count ? historyGet(series, 0, view.first + i) : NaN;
    let color = null, barHeight = 0;
    if (!Number.isNaN(value)) {
      const level = stateOf(value, metric);
      // The live second, which the low end is not made of, can be under it.
      const share = isFlat ? 0 : Math.max(0, (value - low) / (high - low));
      barHeight = Math.round((1 + (height - 3) * share) * density) / density;
      color = level === 'green' ? (isFlat ? COLOR.line : COLOR.bar) : COLOR[level];
    }
    const left = i * width / view.count;
    if (color !== runColor || barHeight !== runHeight) {
      if (runColor !== null) {
        if (fill !== runColor) context.fillStyle = fill = runColor;
        context.fillRect(runLeft, height - runHeight, runRight - runLeft, runHeight);
      }
      runLeft = left;
      runHeight = barHeight;
      runColor = color;
    }
    if (color !== null) runRight = left + barWidth;
  }
}

// A lane is scaled to its own window, lowest value to highest: what matters is when it
// moves, not how large it is. A lane that does not move is drawn as a faint line. Only the
// lanes in sight are drawn.
export function drawLanes() {
  const view = windowOf();
  const marks = timeMarks();
  for (const { instance, canvas, isOut } of state.lanes) {
    if (isOut) continue;
    const { context, width, height } = fitCanvas(canvas);
    const series = seriesOf(instance);
    const metric = instance.metric;
    let low = Infinity, high = -Infinity;
    for (let index = view.first; index <= view.end; index++) {
      const value = historyGet(series, 0, index);
      if (Number.isNaN(value)) continue;
      high = Math.max(high, value);
      if (index <= view.settled) low = Math.min(low, value);  // state.js windowOf
    }
    drawBars(context, series, metric, view, width, height, low, high);
    drawTimeLines(context, marks, 0, width, height);
  }
}

// The number on the right of every lane: at the pointer, else at a second that was
// clicked, else now. Looked up at every draw, also when only the pointer moved, and written
// when it is another number than the one last written: the same number judged by the same
// metric is the same text in the same colour. Of the lanes in sight only.
export function drawLaneValues() {
  const view = windowOf();
  for (const lane of state.lanes) {
    if (lane.isOut) continue;
    const { instance, valueCell } = lane;
    const series = seriesOf(instance);
    const metric = instance.metric;
    let shown = instance.value;
    if (state.crosshair !== null) {
      shown = historyGet(series, 0, indexAt(view, state.crosshair));
    } else if (state.pickedSecond !== null) {
      shown = historyGet(instance.slow, 0, state.pickedSecond);
    } else if (view.kind === 'slow') {  // the last whole second: the live one is partial
      shown = historyGet(series, 0, view.settled);
    }
    if (Object.is(shown, lane.shown) && metric === lane.shownMetric) continue;
    lane.shown = shown;
    lane.shownMetric = metric;
    const level = stateOf(shown, metric);
    setText(valueCell, formatValue(shown, metric.unit));
    setClass(valueCell, level === 'green' ? 'value' : `value ${level}`);
  }
}
