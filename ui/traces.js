// traces.js: the slowest traces and what one of them was made of. One trace is kept per
// second, the slowest. The list ranks those of the window; the waterfall draws the spans
// of the picked one, which is the slowest of all until a row, a lane or the chart is clicked.

import {
  state, windowOf, seriesOf, isShownThread, shownSecond, rankedSeconds, historyGet, scheduleDraw,
} from './state.js';
import {
  COLOR, FONT, siteColor, formatNs, formatValue, formatClock, stateOf, fitCanvas, element,
  replaceIfChanged, setText,
} from './draw.js';

export const LIST_ROWS = 8;
const ROW_HEIGHT = 16;
const LABEL_WIDTH = 96;
const LENGTH_WIDTH = 60;     // room right of a bar for its length
const DURING_MAX = 8;        // lines of "what else happened" under a trace: ui.css sizes for it
const STANDING_SHARE = 0.8;  // past a threshold this much of the window: a standing condition

export function drawSlowest() {
  const table = document.getElementById('slowest');
  const thread = state.threads.get(state.threadIdx);
  const seconds = thread ? rankedSeconds(thread).slice(0, LIST_ROWS) : [];
  const picked = shownSecond();
  // Always LIST_ROWS rows: an empty one holds its place, so that the panel keeps its height.
  replaceIfChanged(table, `${state.threadIdx}|${seconds.join(',')}|${picked}`, () => {
    const head = element('tr');
    for (const title of ['at', 'length', 'trace']) head.append(element('th', '', title));
    const rows = seconds.map((second) => {
      const trace = thread.slowest.get(second);
      const row = element('tr', second === picked ? 'pick picked' : 'pick');
      row.dataset.second = second;
      row.append(element('td', '', formatClock(trace.wall_ms)),
        element('td', '', formatNs(trace.ns)), element('td', '', `#${trace.value}`));
      return row;
    });
    while (rows.length < LIST_ROWS) {
      const row = element('tr');
      row.append(element('td', '', '\u00a0'), element('td'), element('td'));
      rows.push(row);
    }
    return [head, ...rows];
  });
}

// One listener for the whole list: rows come and go.
export function watchSlowest() {
  document.getElementById('slowest').addEventListener('click', (event) => {
    const row = event.target.closest('tr.pick');
    if (!row) return;
    state.pickedSecond = Number(row.dataset.second);
    scheduleDraw();
  });
}

// The trace's box is a row per site of the thread high, whatever the trace: the panel keeps
// its height from one trace to the next. A trace of more spans scrolls in it.
const boxHeightShown = { height: null };

export function drawTrace() {
  const canvas = document.getElementById('trace');
  const note = document.getElementById('trace-note');
  const thread = state.threads.get(state.threadIdx);
  const second = shownSecond();
  const heightOf = (rows) => Math.max(2, rows) * ROW_HEIGHT + 4;
  const boxHeight = heightOf(thread ? thread.info.sites.length : 0);
  if (boxHeight !== boxHeightShown.height) {
    boxHeightShown.height = boxHeight;
    document.getElementById('trace-box').style.height = `${boxHeight}px`;
  }
  drawDuring(second);
  if (second === null) {  // an empty canvas as high as its box: nothing to scroll
    canvas.style.height = `${boxHeight}px`;
    fitCanvas(canvas, boxHeight);
    setText(note, 'none in the window');
    return;
  }

  const trace = thread.slowest.get(second);
  const height = heightOf(Math.max(thread.info.sites.length, trace.spans.length));
  canvas.style.height = `${height}px`;
  const { context, width } = fitCanvas(canvas, height);
  const scale = (width - LABEL_WIDTH - LENGTH_WIDTH) / trace.ns;
  const markKind = state.hello.mark_kind;  // a mark is an instant: a tick
  setText(note, `#${trace.value} - ${formatNs(trace.ns)} - ` +
    (state.pickedSecond === null ? 'the slowest in the window' : 'picked'));

  context.font = FONT;
  context.textBaseline = 'middle';
  trace.spans.forEach(([siteIdx, kind, startNs, lengthNs], row) => {
    const site = thread.info.sites[siteIdx];
    const rowTop = 2 + row * ROW_HEIGHT, barLeft = LABEL_WIDTH + startNs * scale;
    const barWidth = kind === markKind ? 1 : Math.max(1, lengthNs * scale);
    context.fillStyle = COLOR.dim;
    context.fillText(site ? site.name : `site ${siteIdx}`, 0, rowTop + ROW_HEIGHT / 2);
    context.fillStyle = siteColor(siteIdx);
    context.fillRect(barLeft, rowTop + 2, barWidth, ROW_HEIGHT - 5);
    context.fillStyle = COLOR.text;
    context.fillText(kind === markKind ? 'mark' : formatNs(lengthNs), barLeft + barWidth + 4,
      rowTop + ROW_HEIGHT / 2);
  });
}

// Was the instance past a threshold in most of the window's whole columns? Then it is a
// standing condition of this host, not what happened to one trace.
function isStanding(instance) {
  const view = windowOf(), metric = instance.metric, series = seriesOf(instance);
  let past = 0, seen = 0;
  for (let index = view.first; index <= view.settled; index++) {
    const value = historyGet(series, 0, index);
    if (Number.isNaN(value)) continue;
    seen++;
    if (stateOf(value, metric) !== 'green') past++;
  }
  return seen > 0 && past > STANDING_SHARE * seen;
}

// The last value an instance had before a second. When it had none, 0: a count that first
// shows in a second grew by all of it then. Unless the page's history begins there: then
// what came before is not known. The history ends at the live second, when there is one:
// its cell is that of the second a window before it.
function valueBefore(series, second) {
  const oldest = Math.max(state.second, state.liveSecond) - series.count + 1;
  for (let index = second - 1; index >= oldest; index--) {
    const value = historyGet(series, 0, index);
    if (!Number.isNaN(value)) return value;
  }
  return second > oldest ? 0 : NaN;
}

// What the machine did in the second of the picked trace. First what happened then: counts
// that grew, levels past a threshold in that second but not all along. Then, in one dim
// line, what is past a threshold the whole window. As far as a second can tell.
function drawDuring(second) {
  const box = document.getElementById('trace-during');
  const events = [], standing = [];
  for (const instance of second === null ? [] : state.instances.values()) {
    const info = instance.info, metric = instance.metric;
    if (!isShownThread(info.thread, instance.source.scope)) continue;
    const name = [instance.source.name, metric.label, info.key].filter((part) => part).join(' ');
    const value = historyGet(instance.slow, 0, second);
    if (metric.kind === 'total') {
      const grown = value - valueBefore(instance.slow, second);
      const text = `${name}: +${formatValue(grown, metric.unit)}`;
      if (grown > 0) events.push({ level: 'red', text });
      continue;
    }
    const level = stateOf(value, metric);
    if (level === 'green') continue;
    if (isStanding(instance)) standing.push(name);
    else events.push({ level, text: `${name}: ${formatValue(value, metric.unit)}` });
  }
  const shown = events.slice(0, DURING_MAX);
  replaceIfChanged(box, `${second}|${JSON.stringify(shown)}|${standing.join(',')}`, () => {
    if (second === null) return [];
    const title = shown.length ? 'in that second:' : 'in that second: nothing stands out';
    const nodes = [element('div', 'dim', title),
      ...shown.map((line) => element('div', line.level, line.text))];
    if (standing.length) {
      nodes.push(element('div', 'dim', `past a threshold all along: ${standing.join(', ')}`));
    }
    return nodes;
  });
}
