// latency.js: the latency chart and the table of sites. The chart is the whole trace over
// time: median, 99th percentile and maximum per column, on a log scale because a spike is
// often a hundred times the median. The table is every site over the last second.

import {
  state, windowOf, seriesOf, shownSecond, rankedSeconds, fractionOfSecond, timeMarks,
  historyGet, watchCrosshair, P50, P99, MAX, COUNT,
} from './state.js';
import {
  COLOR, FONT, cssPixels, siteColor, formatNs, formatValue, formatClock, fitCanvas, sizeOf,
  drawTimeLines, element, replaceIfChanged, setText,
} from './draw.js';
import { LIST_ROWS } from './traces.js';

const SITE_COLUMNS = ['site', 'p50', 'p99', 'max', 'calls'];
const PAD_TOP = 12, PAD_BOTTOM = 16;
const LABEL_GAP = 58;        // from a scale label's left edge to the plot
const TICK_LABEL_HALF = 22;  // half the width of a time label, to centre it on its tick
const MARK_SIZE = 5;         // the triangles that mark the seconds of the slowest traces
const FLOOR_NS = 10;         // the bottom of the log axis when there is no data to set it

// The chart's sides, in a chart `width` wide: left the lanes' name column, right what is
// right of a lane's chart (its value, the lanes' scrollbar), so that a column of the chart
// and the bar of a lane under it are the same instant.
function padOf(width) {
  const left = cssPixels('lane-name'), lane = state.lanes[0];
  const right = lane ? width - left - sizeOf(lane.canvas).width : cssPixels('lane-value');
  return { left, right, top: PAD_TOP, bottom: PAD_BOTTOM };
}

// One stat of the trace as a whole, at a history index: a thread's history is of nothing else.
function traceStat(thread, series, stat, index) {
  return historyGet(series, stat, index);
}

// The log scale that holds the window's medians and maxima, or null when it is empty.
function scaleOf(thread, series, view) {
  let low = Infinity, high = 0;
  for (let index = view.first; index <= view.end; index++) {
    const p50 = traceStat(thread, series, P50, index);
    const max = traceStat(thread, series, MAX, index);
    if (p50 > 0 && index <= view.settled) low = Math.min(low, p50);  // state.js windowOf
    if (max > 0) high = Math.max(high, max);
  }
  if (!high) return null;
  low = Number.isFinite(low) ? Math.max(1, Math.min(low, high) / 2) : FLOOR_NS;
  return { low, high: high * 2 };
}

// A grid line and a label per decade, the time ticks with theirs, and a triangle over
// the second of each trace in the slowest list.
function drawGrid(context, thread, plot, pad, yOf) {
  context.font = FONT;
  context.textBaseline = 'middle';
  for (let ns = Math.pow(10, Math.ceil(Math.log10(plot.low))); ns < plot.high; ns *= 10) {
    context.fillStyle = COLOR.line;
    context.fillRect(pad.left, Math.round(yOf(ns)), plot.width, 1);
    context.fillStyle = COLOR.dim;
    context.fillText(formatNs(ns), pad.left - LABEL_GAP, yOf(ns));
  }
  const marks = timeMarks();
  drawTimeLines(context, marks, pad.left, plot.width, plot.height + pad.top);
  context.fillStyle = COLOR.dim;
  context.textBaseline = 'bottom';
  for (const tick of marks.ticks) {
    const tickLeft = pad.left + tick.fraction * plot.width - TICK_LABEL_HALF;
    context.fillText(formatClock(tick.wallMs), tickLeft, plot.height + pad.top + pad.bottom);
  }
  const shown = shownSecond();
  for (const second of rankedSeconds(thread).slice(0, LIST_ROWS)) {  // those of the list
    const fraction = fractionOfSecond(second);
    if (fraction < 0 || fraction > 1) continue;
    const tip = pad.left + fraction * plot.width;
    context.fillStyle = second === shown ? COLOR.accent : COLOR.dim;
    context.beginPath();
    context.moveTo(tip - MARK_SIZE, 0);
    context.lineTo(tip + MARK_SIZE, 0);
    context.lineTo(tip, MARK_SIZE * 1.6);
    context.fill();
  }
}

export function drawLatency() {
  const canvas = document.getElementById('latency');
  const { context, width, height } = fitCanvas(canvas);
  const thread = state.threads.get(state.threadIdx);
  const view = windowOf();
  const note = document.getElementById('latency-note');
  if (!thread) { setText(note, 'no traced thread yet'); return; }
  setText(note, thread.info.sites.some((site) => site.root)
    ? `a trace, root to last end, per ${view.stepMs} ms - white p50, blue p99, dots max`
    : `${thread.info.name} has no root site: see the sites table`);

  const series = seriesOf(thread);
  const scale = scaleOf(thread, series, view);
  if (!scale) return;
  const pad = padOf(width);
  const plot = { ...scale, width: width - pad.left - pad.right,
    height: height - pad.top - pad.bottom };
  if (plot.width <= 0) return;  // too narrow, or the lanes not laid out yet
  const columnLeft = (i) => pad.left + i * plot.width / (view.count - 1);
  const logSpan = Math.log(plot.high / plot.low);
  // The live second's median, which the scale is not made of, can be under it: kept in.
  const yOf = (ns) => pad.top + plot.height * (1 - Math.log(Math.max(1, ns / plot.low)) / logSpan);
  drawGrid(context, thread, plot, pad, yOf);

  const drawLine = (stat, color) => {
    let isOpen = false;
    context.strokeStyle = color;
    context.beginPath();
    for (let i = 0; i < view.count; i++) {
      const value = traceStat(thread, series, stat, view.first + i);
      if (!(value > 0)) { isOpen = false; continue; }
      if (isOpen) context.lineTo(columnLeft(i), yOf(value));
      else context.moveTo(columnLeft(i), yOf(value));
      isOpen = true;
    }
    context.stroke();
  };
  context.fillStyle = COLOR.dim;
  for (let i = 0; i < view.count; i++) {  // the maximum as dots: it is a single trace each
    const max = traceStat(thread, series, MAX, view.first + i);
    if (max > 0) context.fillRect(columnLeft(i) - 0.5, yOf(max) - 0.5, 1.5, 1.5);
  }
  context.lineWidth = 1;
  drawLine(P99, COLOR.accent);
  context.lineWidth = 1.5;  // the median must stay visible under the 99th percentile
  drawLine(P50, COLOR.bright);
}

// The pointer's line: one element over the chart and the lanes, moved and not drawn, at
// the x the canvases draw their vertical lines at. It is asked for at every draw and
// touched when it shows, hides or moves a pixel: `hairlineShown` is what was last written,
// and begins as index.html has the element.
const hairlineShown = { isHidden: true, left: null };

export function drawHairline() {
  const hairline = document.getElementById('hairline');
  const { width } = sizeOf(document.getElementById('latency')), pad = padOf(width);
  const plotWidth = width - pad.left - pad.right, fraction = state.crosshair;
  const isHidden = fraction === null || fraction < 0 || fraction > 1 || plotWidth <= 0;
  if (isHidden !== hairlineShown.isHidden) {
    hairlineShown.isHidden = isHidden;
    hairline.hidden = isHidden;
  }
  if (isHidden) return;
  const left = Math.round(pad.left + fraction * (plotWidth - 1));
  if (left === hairlineShown.left) return;
  hairlineShown.left = left;
  hairline.style.transform = `translateX(${left}px)`;
}

export function drawSites() {
  const table = document.getElementById('sites');
  const thread = state.threads.get(state.threadIdx);
  if (!thread) { replaceIfChanged(table, '', () => []); return; }

  // Before its first second the thread's rows are there, empty: the table keeps its height.
  const sites = thread.info.sites, stats = thread.lastSecond || thread.idle;
  const calls = (stat) => (thread.lastSecond ? formatValue(stat[COUNT], '/s') : '-');
  const rows = [];  // [name, colour, stat]; a site that carries a gauge has a lane instead
  if (sites.some((site) => site.root)) rows.push(['whole trace', COLOR.text, stats[sites.length]]);
  sites.forEach((site, siteIdx) => {
    const name = site.root ? `${site.name} (root)` : site.name;
    if (!site.gauge) rows.push([name, siteColor(siteIdx), stats[siteIdx]]);
  });
  replaceIfChanged(table, JSON.stringify(rows), () => {
    const head = element('tr');
    for (const title of SITE_COLUMNS) head.append(element('th', '', title));
    return [head, ...rows.map(([name, color, stat]) => {
      const row = element('tr'), label = element('td'), swatch = element('span', 'swatch');
      const cell = (value) => element('td', '', stat[COUNT] ? formatNs(value) : '-');
      swatch.style.background = color;
      label.append(swatch, name);
      row.append(label, cell(stat[P50]), cell(stat[P99]), cell(stat[MAX]),
        element('td', '', calls(stat)));
      return row;
    })];
  });
}

// The chart shares the lanes' crosshair, and a click on it picks that second's slowest trace.
export function watchLatency() {
  const canvas = document.getElementById('latency');
  watchCrosshair(canvas, (event) => {
    const { width } = sizeOf(canvas), pad = padOf(width);
    const fraction = (event.offsetX - pad.left) / (width - pad.left - pad.right);
    return fraction >= 0 && fraction <= 1 ? fraction : null;
  });
}
