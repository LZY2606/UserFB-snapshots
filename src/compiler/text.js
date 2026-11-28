import { compileDrawing } from './drawing.js';
import { compileTag } from './tag.js';
import {
  freshOrigins,
  inheritOrigin,
  fsOriginFromTag,
  valueOrigin,
  evalFsOrigin,
  evalColorOrigin,
} from '../editable.js';

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

export function compileText({ styles, style, parsed, start, end, editable }) {
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
  let origins = editable ? freshOrigins() : null;
  for (let i = 0; i < parsed.length; i++) {
    const { tags, text, drawing } = parsed[i];
    let reset;
    for (let j = 0; j < tags.length; j++) {
      const tag = tags[j];
      reset = tag.r === undefined ? reset : tag.r;
    }
    if (editable && reset !== undefined) {
      // `\r` drops all overrides and relative dependencies, the new slice
      // style becomes the base style of everything that follows
      origins = freshOrigins();
    }
    const fragment = {
      tag: reset === undefined ? inheritTag(prevTag) : {},
      text,
      drawing: drawing.length ? compileDrawing(drawing) : null,
    };
    for (let j = 0; j < tags.length; j++) {
      const tag = tags[j];
      if (editable) {
        const editableKey = Object.keys(tag)[0];
        if (editableKey === 't') {
          throw new Error('editableStyles: \\t (transform) tags are not supported');
        }
        if (editableKey === 'fs') {
          origins.fs = fsOriginFromTag(tag.fs, origins.fs);
        } else if (editableKey === 'c1') {
          // `\c`/`\1c` only clears the RGB override, alpha stays untouched
          origins.c1 = tag.c1 ? valueOrigin(tag.c1) : inheritOrigin();
        } else if (editableKey === 'a1') {
          origins.a1 = valueOrigin(tag.a1);
        } else if (editableKey === 'alpha') {
          origins.a1 = valueOrigin(tag.alpha);
        }
      }
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
        const compiledTag = compileTag(tag, key, { start, end, c1, c2, c3, c4, fs });
        if (key === 't') {
          fragment.tag.t = fragment.tag.t || [];
          fragment.tag.t.push(compiledTag.t);
        } else {
          Object.assign(fragment.tag, compiledTag);
        }
      }
    }
    prevTag = fragment.tag;
    if (editable) {
      fragment.origin = {
        fs: origins.fs,
        c1: origins.c1,
        a1: origins.a1,
      };
    }
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

  if (editable) {
    // project every fragment against its slice style, so each one exposes
    // the effective fs/c1/a1 next to the origin they were derived from
    for (let i = 0; i < slices.length; i++) {
      const sliceTag = (styles[slices[i].style] || styles[style]).tag;
      for (let j = 0; j < slices[i].fragments.length; j++) {
        const fragment = slices[i].fragments[j];
        const projected = {
          fs: evalFsOrigin(fragment.origin.fs, sliceTag),
          c1: evalColorOrigin(fragment.origin.c1, sliceTag.c1),
          a1: evalColorOrigin(fragment.origin.a1, sliceTag.a1),
        };
        fragment.tag.fs = projected.fs;
        fragment.tag.c1 = projected.c1;
        fragment.tag.a1 = projected.a1;
        fragment.origin.projected = projected;
      }
    }
  }

  return Object.assign({ alignment, slices }, q, pos, org, move, fade, clip);
}
