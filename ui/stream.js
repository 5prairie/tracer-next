// stream.js: where frames come from. Live: the daemon's event stream. For a screenshot or
// a look at a past run: a recording of that stream (curl -N .../stream > file), given as
// ?replay=file: what the page can show of it is fed in at once.

const EVENT_PREFIX = 'data: ';  // how a frame's line begins in a recording of the stream

// Calls onFrame(frame) for every frame and onState(text) when the connection changes.
// Returns the stream's switches, for a pause: close() ends it and open() begins it again,
// and the daemon then sends its history anew, as to any new page. A recording has no
// stream: null.
export function connect(onFrame, onState) {
  const replay = new URLSearchParams(location.search).get('replay');
  if (replay) {
    replayFile(replay, onFrame, onState);
    return null;
  }
  let source = null;
  const close = () => {
    if (source) source.close();
    source = null;
  };
  const open = () => {
    close();
    const opened = new EventSource('/stream');
    opened.onopen = () => onState('live');
    // A refusal (too many pages open) closes the source for good; anything else is retried.
    opened.onerror = () => onState(opened.readyState === EventSource.CLOSED
      ? 'refused: too many pages are open' : 'reconnecting');
    opened.onmessage = (message) => onFrame(JSON.parse(message.data));
    source = opened;
  };
  open();
  return { open, close };
}

// How the line of a tick or a second begins, as the daemon writes it: its kind and its index
// are read from there, without parsing the frame.
const SAMPLE_LINE = /^data: \{"type":"(tick|second)","index":(\d+),/;

function parsed(line) {
  try {
    return JSON.parse(line.slice(EVENT_PREFIX.length));
  } catch {
    return null;  // a recording cut by a timeout ends in half a frame: skip it
  }
}

// What of a recording the page can show: every hello, and for each cell of the two
// histories the newest tick or second that falls in it. A recording is hours long and the
// histories hold a minute of ticks and ten of seconds: the rest would be parsed only to be
// overwritten. Ticks are kept as their lines and parsed at the end, if they are still
// there. A kept line keeps the piece of the file it was cut from: the newest ticks are the
// file's end, but the seconds reach ten minutes back, so those are parsed as they come.
function recordingKeeper() {
  const hellos = [];         // { order, frame }
  const rings = [[], []];    // ticks, seconds: cell -> { order, index, line, frame }
  let counts = null, order = 0, newestSlot = -1;
  const keep = (kind, index, line, frame) => {
    if (!counts) return;  // before any hello: of tables the page never learns
    const cell = index % counts[kind], old = rings[kind][cell];
    if (old && index < old.index) return;
    if (!kind) newestSlot = Math.max(newestSlot, index);
    else if (!frame) frame = parsed(line);
    if (kind && !frame) return;
    rings[kind][cell] = { order: order++, index, line: frame ? null : line, frame };
  };
  return {
    take(line) {
      if (!line.startsWith(EVENT_PREFIX)) return;
      const found = SAMPLE_LINE.exec(line);
      if (found) { keep(found[1] === 'tick' ? 0 : 1, Number(found[2]), line, null); return; }
      const frame = parsed(line);  // a hello, or a frame that begins another way
      if (!frame) return;
      if (frame.type === 'tick' || frame.type === 'second') {
        const kind = frame.type === 'tick' ? 0 : 1;
        if (Number.isInteger(frame.index)) keep(kind, frame.index, null, frame);
        return;
      }
      if (frame.type !== 'hello') return;
      if (frame.slot < newestSlot) {  // the daemon's clock began again: as main.js onHello
        rings[0].length = 0;
        rings[1].length = 0;
        newestSlot = -1;
      }
      counts = [frame.fast_cnt, frame.slow_cnt];
      hellos.push({ order: order++, frame });
    },
    // The frames kept, in the order they came in: each after the hello it was sent under.
    frames() {
      const kept = hellos.concat(rings[0].filter(Boolean), rings[1].filter(Boolean));
      kept.sort((left, right) => left.order - right.order);
      return kept.map((entry) => entry.frame || parsed(entry.line)).filter(Boolean);
    },
  };
}

// The recording is read as it downloads, a piece at a time, and is never whole in memory:
// as one string, a file of over 512 MB comes back empty and without an error.
async function replayFile(path, onFrame, onState) {
  onState('loading the recording');
  const keeper = recordingKeeper();
  try {
    const answer = await fetch(path);
    if (!answer.ok) throw new Error(answer.statusText);
    const reader = answer.body.pipeThrough(new TextDecoderStream()).getReader();
    let rest = '';
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      const text = rest + value;
      let start = 0;
      for (let end = text.indexOf('\n'); end >= 0; end = text.indexOf('\n', start)) {
        if (end > start) keeper.take(text.slice(start, end));
        start = end + 1;
      }
      rest = text.slice(start);
    }
    keeper.take(rest);
  } catch {
    onState('the recording cannot be read');
    return;
  }
  for (const frame of keeper.frames()) onFrame(frame);
  onState('recording');
}
