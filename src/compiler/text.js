import { compileDrawing } from './drawing.js';
import { compileTag } from './tag.js';
import { pickSampleKeys } from './timeline.js';

const a2an = [
  null, 1, 2, 3,
  null, 7, 8, 9,
  null, 4, 5, 6,
];

const globalTags = ['r', 'a', 'an', 'pos', 'org', 'move', 'fade', 'fad', 'clip'];

function inheritTag(pTag) {
  return JSON.parse(JSON.stringify(Object.assign({}, pTag, {
    k: undefined,
    kf: undefined,
    ko: undefined,
    kt: undefined,
  })));
}

export function compileText({ styles, style, parsed, start, end, timeline = false }) {
  let alignment;
  let q = { q: styles[style].tag.q };
  let pos;
  let org;
  let move;
  let fade;
  let clip;
  const slices = [];
  let slice = { style, fragments: [] };
  let prevTag = {};
  // Ordered static assignments and transforms for offline style sampling.
  // `\r` is recorded as a reset operation so the clock keeps running while
  // all accumulated style state is discarded.
  const operations = timeline ? [] : null;
  for (let i = 0; i < parsed.length; i++) {
    const { tags, text, drawing } = parsed[i];
    let reset;
    for (let j = 0; j < tags.length; j++) {
      const tag = tags[j];
      reset = tag.r === undefined ? reset : tag.r;
    }
    const fragment = {
      tag: reset === undefined ? inheritTag(prevTag) : {},
      text,
      drawing: drawing.length ? compileDrawing(drawing) : null,
    };
    if (timeline && reset !== undefined) {
      // Same fallback as the slice style below: an unknown `\rStyle` keeps
      // the current style.
      operations.push({ type: 'reset', style: styles[reset] ? reset : style });
    }
    for (let j = 0; j < tags.length; j++) {
      const tag = tags[j];
      alignment = alignment || a2an[tag.a || 0] || tag.an;
      q = compileTag(tag, 'q') || q;
      if (!move) {
        pos = pos || compileTag(tag, 'pos');
      }
      org = org || compileTag(tag, 'org');
      if (!pos) {
        move = move || compileTag(tag, 'move');
      }
      fade = fade || compileTag(tag, 'fade') || compileTag(tag, 'fad');
      clip = compileTag(tag, 'clip') || clip;
      const key = Object.keys(tag)[0];
      if (key && !~globalTags.indexOf(key)) {
        const sliceTag = styles[style].tag;
        const { c1, c2, c3, c4 } = sliceTag;
        const fs = prevTag.fs || sliceTag.fs;
        const compiledTag = compileTag(tag, key, {
          start, end, c1, c2, c3, c4, fs, timeline,
        });
        if (key === 't') {
          fragment.tag.t = fragment.tag.t || [];
          fragment.tag.t.push(compiledTag.t);
          if (timeline) {
            // `bord`/`shad`/`fr`/`alpha` and relative `fs` have already been
            // expanded by compileTag, so the operation only carries final
            // samplable keys and absolute values.
            operations.push({
              type: 't',
              t1: compiledTag.t.t1,
              t2: compiledTag.t.t2,
              accel: compiledTag.t.accel,
              tag: pickSampleKeys(compiledTag.t.tag),
            });
          }
        } else {
          Object.assign(fragment.tag, compiledTag);
          if (timeline) {
            const keys = Object.keys(compiledTag);
            for (let k = 0; k < keys.length; k++) {
              const sampleKey = keys[k];
              const sampleValue = compiledTag[sampleKey];
              if (typeof sampleValue === 'number' && !Number.isFinite(sampleValue)) {
                throw new RangeError(
                  `[ass-compiler] tag \\${sampleKey} needs a finite numeric value`,
                );
              }
            }
            // Karaoke, drawing mode and other non-style tags leave nothing to
            // sample, so they don't clutter the ordered operation list.
            const sampleTag = pickSampleKeys(compiledTag);
            if (Object.keys(sampleTag).length) {
              operations.push({ type: 'set', tag: sampleTag });
            }
          }
        }
      }
    }
    if (timeline && (fragment.text || fragment.drawing)) {
      // Operations before this index describe everything up to and including
      // the fragment's own override block.
      fragment.timelineIndex = operations.length;
    }
    prevTag = fragment.tag;
    if (reset !== undefined) {
      slices.push(slice);
      slice = { style: styles[reset] ? reset : style, fragments: [] };
    }
    if (fragment.text || fragment.drawing) {
      const prev = slice.fragments[slice.fragments.length - 1] || {};
      if (prev.text && fragment.text && !Object.keys(fragment.tag).length) {
        // merge fragment to previous if its tag is empty
        prev.text += fragment.text;
      } else {
        slice.fragments.push(fragment);
      }
    }
  }
  slices.push(slice);

  return Object.assign(
    { alignment, slices },
    q, pos, org, move, fade, clip,
    timeline ? { animationTimeline: operations } : null,
  );
}
