// check.js: the sanitized environment as a row of cells under the header. A check is a
// metric that should stay under its red threshold. A cell keeps its place: its colour says
// when it fails. Below the cards, what a source could not read on this host, and why.

import { state, isShownThread } from './state.js';
import { formatValue, stateOf, element, replaceIfChanged } from './draw.js';

export function drawChecks() {
  const checks = [...state.instances.values()]
    .filter((instance) => instance.metric.widget === 'check' && instance.source.on &&
                          isShownThread(instance.info.thread, instance.source.scope))
    .sort((left, right) => left.info.source - right.info.source ||
                           left.info.metric - right.info.metric);
  const cells = checks.map((instance) => {
    const label = [instance.metric.label, instance.info.key].filter((part) => part).join(' ');
    const value = formatValue(instance.value, instance.metric.unit);
    return { level: stateOf(instance.value, instance.metric), label, value };
  });

  // The value has a box of its own, as wide as most values (ui.css .reading): a cell is as
  // wide as its label makes it, and the row does not wrap again when a value changes.
  replaceIfChanged(document.getElementById('checks'), JSON.stringify(cells), () =>
    [element('h2', '', 'checks'), ...cells.map((cell) => {
      const node = element('span', `check ${cell.level}`, cell.label);
      node.append(element('span', 'reading', cell.value));
      return node;
    })]);
  drawUnavailable();
}

// What a source said of this host: why it cannot read something, or what reading it costs.
// And the sources that are on though they have never run on a host of their kind.
function drawUnavailable() {
  const lines = [];
  for (const entry of state.hello.unavailable) {
    if (!isShownThread(entry.thread, entry.scope)) continue;
    const what = entry.metric ? `${entry.source}, ${entry.metric}` : entry.source;
    lines.push(`${what}: ${entry.why}`);
  }
  for (const source of state.hello.sources) {
    if (source.on && source.unproven) {
      lines.push(`${source.name}: not run on a host of its kind yet; read its numbers with care`);
    }
  }
  replaceIfChanged(document.getElementById('unavailable'), lines.join('\n'), () =>
    (lines.length ? [element('h2', '', 'not available here, and notes'),
      ...lines.map((line) => element('div', 'dim', line))] : []));
}
