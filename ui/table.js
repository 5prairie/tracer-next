// table.js: one card per active source that has table metrics. Rows are the instances'
// keys (a queue, a socket, a CPU) in order, columns the source's table metrics. The card
// also says what reading the source costs the daemon. A card's shape (its rows, its
// columns, its footer line) follows from what instances there are, never from their
// values: a value that moves does not move the page. Rows quiet a while are dimmed.

import { state, isShownThread, historyGet } from './state.js';
import { formatValue, stateOf, element, setChildren, cssPixels, sizeOf } from './draw.js';

const ROW_MAX = 16;     // rows shown per card; past it, which ones: see shownKeys
const RECENT_S = 10;    // a row is quiet this long after its last value above zero
const byKey = new Intl.Collator(undefined, { numeric: true }).compare;  // eth0/2 before eth0/10

// The cards on the page: source index -> { html, card }, the card's node and what it was
// built as. A card is its own signature.
const cardShown = new Map();

// The keys of the rows each card shows, when it has more than ROW_MAX: source index -> Set.
const keysShown = new Map();

// The table instances of one source for the thread shown: key -> { metric id -> instance }.
function rowsOf(sourceIdx) {
  const rows = new Map();
  for (const instance of state.instances.values()) {
    const info = instance.info;
    if (info.source !== sourceIdx || instance.metric.widget !== 'table') continue;
    if (!isShownThread(info.thread, instance.source.scope)) continue;
    if (!rows.has(info.key)) rows.set(info.key, {});
    rows.get(info.key)[instance.metric.id] = instance;
  }
  return rows;
}

function buildRow(key, cells, metrics) {
  const row = element('tr');
  row.append(element('td', '', key));
  for (const metric of metrics) {
    const value = cells[metric.id] ? cells[metric.id].value : NaN;
    const level = stateOf(value, metric);
    row.append(element('td', level === 'green' ? '' : level, formatValue(value, metric.unit)));
  }
  return row;
}

// The highest value of a row's cells over the last seconds: a row that has just gone quiet
// keeps its place, rather than leaving and coming back.
function recentPeak(cells) {
  let peak = 0;
  for (const cell of Object.values(cells)) {
    for (let second = state.second - RECENT_S + 1; second <= state.second; second++) {
      peak = Math.max(peak, historyGet(cell.slow, 0, second) || 0);
    }
  }
  return peak;
}

// Which ROW_MAX rows of a card with more are shown. Those shown last time stay; one that
// has gone quiet gives its place to a hidden one that is not, loudest first. Then, while
// there is room, the loudest, then in key order. A row changes only for a louder one.
function shownKeys(sourceIdx, rows, peaks) {
  if (rows.size <= ROW_MAX) { keysShown.delete(sourceIdx); return [...rows.keys()]; }
  const shown = new Set([...(keysShown.get(sourceIdx) || [])].filter((key) => rows.has(key)));
  const loudest = [...rows.keys()].sort((left, right) =>
    peaks.get(right) - peaks.get(left) || byKey(left, right));
  for (const key of loudest) {
    if (shown.has(key) || !(peaks.get(key) > 0)) continue;
    if (shown.size >= ROW_MAX) {  // full: only in place of a quiet one
      const quiet = [...shown].find((other) => !(peaks.get(other) > 0));
      if (quiet === undefined) break;
      shown.delete(quiet);
    }
    shown.add(key);
  }
  for (const key of loudest) {
    if (shown.size >= ROW_MAX) break;
    shown.add(key);
  }
  keysShown.set(sourceIdx, shown);
  return [...shown];
}

// The card of one source and how many lines it has, or null when it has nothing to say
// for this thread. A column is a table metric some row has an instance of, read or not yet.
function buildCard(source, sourceIdx) {
  const rows = source.on ? rowsOf(sourceIdx) : new Map();
  const isFilled = (metric) => [...rows.values()].some((cells) => cells[metric.id]);
  const metrics = source.metrics.filter((metric) => metric.widget === 'table' && isFilled(metric));
  if (!rows.size || !metrics.length) {
    keysShown.delete(sourceIdx);
    return null;
  }

  const peaks = new Map([...rows].map(([key, cells]) => [key, recentPeak(cells)]));
  const keys = shownKeys(sourceIdx, rows, peaks).sort(byKey);
  const quiet = [...peaks.values()].filter((peak) => !(peak > 0)).length;
  const hidden = rows.size - keys.length;
  const lines = keys.length + 3;  // the title, the head, the rows, the footer

  const card = element('section', 'card');
  const title = element('h2', '', source.name);
  const cost = state.costs[sourceIdx];
  card.append(title);
  if (cost) title.append(element('span', 'dim', `read ${cost[0]} us every ${cost[1]} ms`));
  const table = element('table'), head = element('tr');
  head.append(element('th', '', ''));
  for (const metric of metrics) head.append(element('th', '', metric.label));
  table.append(head);
  for (const key of keys) {
    const row = buildRow(key, rows.get(key), metrics);
    if (!(peaks.get(key) > 0)) row.className = 'quiet';
    table.append(row);
  }
  card.append(table);
  // Always one line, empty or not.
  const footer = [hidden ? `${hidden} more` : '', quiet ? `${quiet} at zero` : '']
    .filter((part) => part).join(', ');
  card.append(element('div', 'dim', footer || '\u00a0'));
  return { sourceIdx, card, lines };
}

// As many columns as cards --card-min wide fit. A card goes in the shortest column, by the
// lines it may come to have, when it first shows; then it stays there. Within a column the
// cards are in source order from the start, so that none changes place later. A new count
// of columns places them all again. The cards are cheap to build and what a card shows is
// its own signature: one that is built the same as the last time keeps the node it has on
// the page, with a selection made in it and its scroll, and only the others are replaced.
export function drawTables() {
  const box = document.getElementById('sources'), columnOf = state.cardColumn;
  const count = Math.max(1, Math.floor(sizeOf(box).width / cssPixels('card-min')));
  if (count !== state.cardColumnCnt) { columnOf.clear(); state.cardColumnCnt = count; }
  const shown = state.hello.sources.map(buildCard).filter((entry) => entry);
  const lines = Array(count).fill(0);
  for (const entry of shown.filter(({ sourceIdx }) => columnOf.has(sourceIdx))) {
    lines[columnOf.get(entry.sourceIdx)] += entry.lines;
  }
  for (const entry of shown.filter(({ sourceIdx }) => !columnOf.has(sourceIdx))) {
    const column = lines.indexOf(Math.min(...lines));
    columnOf.set(entry.sourceIdx, column);
    lines[column] += entry.lines;
  }
  for (const entry of shown) {
    const html = entry.card.outerHTML, old = cardShown.get(entry.sourceIdx);
    if (old && old.html === html) entry.card = old.card;
    else cardShown.set(entry.sourceIdx, { html, card: entry.card });
  }
  for (const sourceIdx of [...cardShown.keys()]) {  // a card that no longer shows
    if (!shown.some((entry) => entry.sourceIdx === sourceIdx)) cardShown.delete(sourceIdx);
  }
  if (box.childNodes.length !== count) {
    box.replaceChildren(...Array.from({ length: count }, () => element('div')));
  }
  for (let column = 0; column < count; column++) {
    setChildren(box.childNodes[column],
      shown.filter(({ sourceIdx }) => columnOf.get(sourceIdx) === column).map(({ card }) => card));
  }
}
