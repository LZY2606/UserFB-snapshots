// Offline style sampling for animated dialogues.
// No DOM, Canvas or font engine is involved: every value is derived from the
// ordered static assignments and `\t` transforms collected at compile time.

// Properties that can appear in a sampled style tag. `frx`/`fry`/`fax`/`fay`
// and `blur` are not part of the `[V4+ Styles]` line, their implicit ASS
// defaults are all 0.
export const SAMPLE_KEYS = [
  'fn', 'fs',
  'c1', 'a1', 'c2', 'a2', 'c3', 'a3', 'c4', 'a4',
  'b', 'i', 'u', 's',
  'fscx', 'fscy', 'fsp', 'frz',
  'frx', 'fry', 'fax', 'fay', 'blur',
  'xbord', 'ybord', 'xshad', 'yshad',
  'fe', 'q',
];

const COLOR_KEYS = ['c1', 'c2', 'c3', 'c4'];
const ALPHA_KEYS = ['a1', 'a2', 'a3', 'a4'];

const IMPLICIT_DEFAULTS = {
  frx: 0,
  fry: 0,
  fax: 0,
  fay: 0,
  blur: 0,
};

// Keep only properties that describe a samplable text style.
export function pickSampleKeys(tag) {
  const result = {};
  for (let i = 0; i < SAMPLE_KEYS.length; i++) {
    const key = SAMPLE_KEYS[i];
    if (tag[key] !== undefined) {
      result[key] = tag[key];
    }
  }
  return result;
}

function createBaseState(styleTag) {
  return Object.assign({}, styleTag, IMPLICIT_DEFAULTS);
}

// Round to the nearest integer, halves go up (towards +Infinity), then clamp
// to a byte. ASS colors/alphas are always non-negative so this is the
// "half up" rule the spec asks for.
function mixByte(from, to, progress) {
  const value = Math.floor(from + (to - from) * progress + 0.5);
  return Math.min(255, Math.max(0, value));
}

function toHexByte(value) {
  return value.toString(16).padStart(2, '0').toUpperCase();
}

// Colors are BBGGRR strings: each of the three bytes is interpolated on its
// own, never as one 24-bit integer, and no gamma conversion is applied.
function mixColor(from, to, progress) {
  let result = '';
  for (let i = 0; i < 6; i += 2) {
    const fromByte = parseInt(from.slice(i, i + 2), 16);
    const toByte = parseInt(to.slice(i, i + 2), 16);
    result += toHexByte(mixByte(fromByte, toByte, progress));
  }
  return result;
}

// Alphas are single transparency bytes.
function mixAlpha(from, to, progress) {
  return toHexByte(mixByte(parseInt(from, 16), parseInt(to, 16), progress));
}

function mixValue(key, from, to, progress) {
  if (COLOR_KEYS.indexOf(key) !== -1) {
    return mixColor(from, to, progress);
  }
  if (ALPHA_KEYS.indexOf(key) !== -1) {
    return mixAlpha(from, to, progress);
  }
  return from + (to - from) * progress;
}

function transformProgress(time, op) {
  const { t1, t2, accel } = op;
  if (t1 === t2) {
    // A zero-length transform is a step at t1.
    return time >= t1 ? 1 : 0;
  }
  const linear = Math.min(1, Math.max(0, (time - t1) / (t2 - t1)));
  return linear ** accel;
}

function sampleFragment(styles, styleName, operations, fragment, time) {
  if (!Number.isInteger(fragment.timelineIndex)) {
    throw new Error('[ass-compiler] fragment is missing animation timeline information');
  }
  let state = createBaseState(styles[styleName].tag);
  for (let i = 0; i < fragment.timelineIndex; i++) {
    const op = operations[i];
    if (op.type === 'reset') {
      // `\r` wipes both static and animated state and switches the style.
      state = createBaseState(styles[op.style].tag);
    } else if (op.type === 'set') {
      Object.assign(state, op.tag);
    } else {
      const progress = transformProgress(time, op);
      const keys = Object.keys(op.tag);
      for (let j = 0; j < keys.length; j++) {
        const key = keys[j];
        // The start value is whatever earlier operations have accumulated,
        // so chained transforms stack instead of restarting from the default.
        state[key] = mixValue(key, state[key], op.tag[key], progress);
      }
    }
  }
  return state;
}

/**
 * Sample the concrete text style of every fragment of one dialogue at a given
 * absolute time. The compiled object, its styles and the returned values are
 * never mutated.
 * @param {Object} compiled result of `compile(text, { animationTimeline: true })`
 * @param {Number} dialogueIndex index in `compiled.dialogues`
 * @param {Number} absoluteSeconds seconds on the video timeline
 * @returns {Array<Object>|null} fragments in slice/fragment order, or `null`
 *   when the time is outside of the dialogue's `[start, end)` interval
 */
export function sampleDialogue(compiled, dialogueIndex, absoluteSeconds) {
  if (!compiled || !Array.isArray(compiled.dialogues) || !compiled.styles) {
    throw new Error('[ass-compiler] sampleDialogue expects a compiled ASS object');
  }
  if (!Number.isInteger(dialogueIndex)
    || dialogueIndex < 0
    || dialogueIndex >= compiled.dialogues.length) {
    throw new RangeError(
      `[ass-compiler] invalid dialogue index: ${dialogueIndex}`,
    );
  }
  if (!Number.isFinite(absoluteSeconds)) {
    throw new RangeError(
      `[ass-compiler] sample time must be a finite number, got ${absoluteSeconds}`,
    );
  }
  const dialogue = compiled.dialogues[dialogueIndex];
  if (!Array.isArray(dialogue.animationTimeline)) {
    throw new Error(
      '[ass-compiler] dialogue has no animation timeline, compile with `{ animationTimeline: true }`',
    );
  }
  if (absoluteSeconds < dialogue.start || absoluteSeconds >= dialogue.end) {
    return null;
  }
  // Transform clocks are milliseconds relative to the dialogue start.
  const time = (absoluteSeconds - dialogue.start) * 1000;
  const result = [];
  for (let s = 0; s < dialogue.slices.length; s++) {
    const slice = dialogue.slices[s];
    for (let f = 0; f < slice.fragments.length; f++) {
      const fragment = slice.fragments[f];
      const tag = sampleFragment(
        compiled.styles,
        slice.style,
        dialogue.animationTimeline,
        fragment,
        time,
      );
      const entry = {
        text: fragment.text,
        tag,
        drawing: null,
      };
      if (fragment.drawing) {
        // An independent copy: drawings are JSON-safe compiled objects.
        entry.drawing = JSON.parse(JSON.stringify(fragment.drawing));
      }
      result.push(entry);
    }
  }
  return result;
}
