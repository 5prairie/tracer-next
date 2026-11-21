// draw.js: what every panel shares. Colours, number formats, the state colour of a value
// against its thresholds, a canvas sized to its box at the screen's pixel density, and a
// way to touch the document only when what it shows has changed.

import { state, scheduleDraw } from './state.js';

// The colours and the widths are the stylesheet's: ui.css is the one place they are defined.
const style = getComputedStyle(document.documentElement);
const fromCss = (name) => style.getPropertyValue(`--${name}`).trim();

// A width of the stylesheet. Read once and kept: the panels ask at every draw and at every
// move of the pointer, and a read of a computed style can cost a style recalculation. A
// media query can change one: a box that changes width forgets them all (see `resized`).
const pixels = new Map();

export function cssPixels(name) {
  let value = pixels.get(name);
  if (value === undefined) {
    value = parseFloat(fromCss(name));
    pixels.set(name, value);
  }
  return value;
}

export const COLOR = {
  line: fromCss('line'), text: fromCss('text'), dim: fromCss('dim'), amber: fromCss('amber'),
  red: fromCss('red'), accent: fromCss('accent'), bar: fromCss('bar'), bright: fromCss('bright'),
};

// One colour per site, by its index: --site-0, --site-1, ... as far as the stylesheet
// goes; they repeat past the last.
const SITE_COLOR = [];
for (let i = 0; fromCss(`site-${i}`); i++) SITE_COLOR.push(fromCss(`site-${i}`));

export const siteColor = (siteIdx) => SITE_COLOR[siteIdx % SITE_COLOR.length];

export const FONT = '10px ui-monospace, monospace';  // every label drawn on a canvas

const MAGNITUDE = [[1e9, 'G'], [1e6, 'M'], [1e3, 'k']];
const SPAN = [[3600e3, 'h'], [60e3, 'm'], [1e3, 's']];  // in ms

// Two decimals below ten, one above: decided on the rounded number, so 9.996 is "10.0".
function rounded(value) {
  const fine = value.toFixed(2);
  return Math.abs(Number(fine)) < 10 ? fine : value.toFixed(1);
}

// A number as it will be printed, to four digits: its unit is chosen after that, so that
// 999.96 is "1.00k" and not "1000.0".
const settled = (value) => Number(value.toPrecision(4));

function scaled(value, digits) {
  const exact = settled(value);
  for (const [size, suffix] of MAGNITUDE) {
    if (Math.abs(exact) >= size) return (exact / size).toFixed(digits) + suffix;
  }
  return Number.isInteger(exact) ? String(exact) : rounded(exact);
}

export function formatNs(ns) {
  if (!Number.isFinite(ns)) return '-';
  const exact = settled(ns);
  if (exact < 1e3) return `${Math.round(exact)} ns`;
  if (exact < 1e6) return `${rounded(exact / 1e3)} us`;
  if (exact < 1e9) return `${rounded(exact / 1e6)} ms`;
  return `${(exact / 1e9).toFixed(2)} s`;
}

// How long a window is, as its button says it: "1 m", "10 m", "30 s", "1.5 h".
export function formatSpan(ms) {
  for (const [size, suffix] of SPAN) {
    if (ms >= size) return `${Number((ms / size).toFixed(1))} ${suffix}`;
  }
  return `${ms} ms`;
}

// A wall-clock time as every panel writes it: one formatter, made once.
const CLOCK = new Intl.DateTimeFormat([], {
  hour12: false, hour: 'numeric', minute: 'numeric', second: 'numeric',
});

export const formatClock = (wallMs) => CLOCK.format(wallMs);

// A metric's value with its unit: "1.2k /s", "45 %", "3.1 us", "12" for a bare count.
export function formatValue(value, unit) {
  if (value === undefined || !Number.isFinite(value)) return '-';
  if (unit === 'ns') return formatNs(value);
  if (unit === 'bytes') return `${scaled(value, 1)} B`;
  if (unit === '%') return `${value.toFixed(Number(value.toFixed(1)) < 10 ? 1 : 0)} %`;
  return unit ? `${scaled(value, 1)} ${unit}` : scaled(value, 1);
}

// 'red', 'amber' or 'green': where a value stands against a metric's thresholds. A
// threshold of 0 means there is none.
export function stateOf(value, metric) {
  if (!Number.isFinite(value)) return 'green';
  if (metric.red > 0 && value >= metric.red) return 'red';
  if (metric.amber > 0 && value >= metric.amber) return 'amber';
  return 'green';
}

// Box sizes, kept by one ResizeObserver: reading clientWidth while drawing would force a
// layout after every change the draw made to the document. A box that becomes wider or
// narrower is drawn again, with the once-a-second panels, whose columns depend on it.
const resized = new ResizeObserver((entries) => {
  let isWidthChanged = false;
  for (const { target, contentRect } of entries) {
    if (!target.isConnected) { resized.unobserve(target); continue; }
    isWidthChanged ||= state.sizes.get(target).width !== contentRect.width;
    state.sizes.set(target, { width: contentRect.width, height: contentRect.height });
  }
  if (!isWidthChanged) return;
  pixels.clear();  // the stylesheet's widths may be other ones at this width
  state.slowVersion++;
  scheduleDraw();
});

// A box's CSS size as last laid out. A box not seen before is measured, once: drawn at no
// size, a new lane and the chart above it would be blank for a frame.
export function sizeOf(box) {
  if (!state.sizes.has(box)) {
    state.sizes.set(box, { width: box.clientWidth, height: box.clientHeight });
    resized.observe(box);
  }
  return state.sizes.get(box);
}

// Size a canvas's bitmap to its CSS box and return a context scaled so that one unit is
// one CSS pixel. Clears it. A canvas whose height was just set passes it: the box has not
// been laid out with it yet.
export function fitCanvas(canvas, height = sizeOf(canvas).height) {
  const density = window.devicePixelRatio || 1;
  const { width } = sizeOf(canvas);
  // Whole pixels: a fractional density would never compare equal, and the bitmap would be
  // allocated again on every draw.
  const pixelWidth = Math.round(width * density), pixelHeight = Math.round(height * density);
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth;
    canvas.height = pixelHeight;
  }
  const context = canvas.getContext('2d');
  context.setTransform(density, 0, 0, density, 0, 0);
  context.clearRect(0, 0, width, height);
  return { context, width, height };
}

// The vertical lines every chart shares, over a plot that starts at `left` and is `width`
// wide: faint ones at the time ticks, and the picked second. The pointer's line is not
// drawn: it is an element of the page (latency.js drawHairline).
export function drawTimeLines(context, marks, left, width, height) {
  const lineAt = (fraction, color, thickness) => {
    if (fraction === null || fraction < 0 || fraction > 1) return;
    context.fillStyle = color;
    context.fillRect(Math.round(left + fraction * (width - 1)), 0, thickness, height);
  };
  for (const tick of marks.ticks) lineAt(tick.fraction, COLOR.line, 1);
  lineAt(marks.picked, COLOR.accent, 2);
}

export function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

// A node's text and its class, written only when they differ from what was last written
// here. The browser does not make that check: the same string in textContent is a new text
// node, hence a layout and a repaint, and the same className is still a mutation. For the
// nodes that are written at every draw, and that nothing else writes.
const textWritten = new WeakMap(), classWritten = new WeakMap();

export function setText(node, text) {
  if (textWritten.get(node) === text) return;
  textWritten.set(node, text);
  node.textContent = text;
}

export function setClass(node, className) {
  if (classWritten.get(node) === className) return;
  classWritten.set(node, className);
  node.className = className;
}

// Replace a box's children, all of them, but only when `signature` differs from the last
// time: frames arrive every slot, and rebuilding a table that has not changed would swallow
// clicks and waste the browser's time. One signature is one box: a box whose parts change
// apart, as the source cards do, keeps a signature per part and places them with setChildren.
export function replaceIfChanged(box, signature, build) {
  if (state.signatures.get(box) === signature) return;
  state.signatures.set(box, signature);
  box.replaceChildren(...build());
}

// Make a box's children these nodes, in this order. A node already in its place is not
// touched: it keeps its layout, a selection made in it, its scroll.
export function setChildren(box, nodes) {
  nodes.forEach((node, i) => {
    const old = box.childNodes[i];
    if (old === node) return;
    if (old && !nodes.includes(old)) old.replaceWith(node);
    else box.insertBefore(node, old || null);
  });
  while (box.childNodes.length > nodes.length) box.lastChild.remove();
}
