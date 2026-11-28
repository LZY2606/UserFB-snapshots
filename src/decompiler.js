import { stringifyInfo, stringifyTime, stringifyEffect } from './stringifier.js';
import { stylesFormat, eventsFormat } from './utils.js';
import { reconcileDocument, originEqual, fsOriginToTags, freshOrigins } from './editable.js';

export function decompileStyle({ style, tag }) {
  const obj = Object.assign({}, style, {
    PrimaryColour: `&H${tag.a1}${tag.c1}`,
    SecondaryColour: `&H${tag.a2}${tag.c2}`,
    OutlineColour: `&H${tag.a3}${tag.c3}`,
    BackColour: `&H${tag.a4}${tag.c4}`,
  });
  return `Style: ${stylesFormat.map((fmt) => obj[fmt]).join()}`;
}

const drawingInstructionMap = {
  M: 'm',
  L: 'l',
  C: 'b',
};

export function decompileDrawing({ instructions }) {
  return instructions.map(({ type, points }) => (
    [drawingInstructionMap[type]]
      .concat(...points.map(({ x, y }) => [x, y]))
      .join(' ')
  )).join(' ');
}

const ca = (x) => (n) => (_) => `${n}${x}&H${_}&`;
const c = ca('c');
const a = ca('a');

const tagDecompiler = {
  c1: c(1),
  c2: c(2),
  c3: c(3),
  c4: c(4),
  a1: a(1),
  a2: a(2),
  a3: a(3),
  a4: a(4),
  pos: (_) => `pos(${[_.x, _.y]})`,
  org: (_) => `org(${[_.x, _.y]})`,
  move: (_) => `move(${[_.x1, _.y1, _.x2, _.y2, _.t1, _.t2]})`,
  fade: (_) => (
    _.type === 'fad'
      ? `fad(${[_.t1, _.t2]})`
      : `fade(${[_.a1, _.a2, _.a3, _.t1, _.t2, _.t3, _.t4]})`
  ),
  clip: (_) => `${_.inverse ? 'i' : ''}clip(${
    _.dots
      ? `${[_.dots.x1, _.dots.y1, _.dots.x2, _.dots.y2]}`
      : `${_.scale === 1 ? '' : `${_.scale},`}${decompileDrawing(_.drawing)}`
  })`,
  // eslint-disable-next-line no-use-before-define
  t: (arr) => arr.map((_) => `t(${[_.t1, _.t2, _.accel, decompileTag(_.tag)]})`).join('\\'),
};

export function decompileTag(tag) {
  return Object.keys(tag).map((key) => {
    const fn = tagDecompiler[key] || ((_) => `${key}${_}`);
    return `\\${fn(tag[key])}`;
  }).join('');
}

export function decompileSlice(slice) {
  return slice.fragments.map(({ tag, text, drawing }) => {
    const tagText = decompileTag(tag);
    return `${tagText ? `{${tagText}}` : ''}${drawing ? decompileDrawing(drawing) : text}`;
  }).join('');
}

export function decompileText(dia, style) {
  return dia.slices
    .filter((slice) => slice.fragments.length)
    .map((slice, idx) => {
      const sliceCopy = JSON.parse(JSON.stringify(slice));
      const tag = {};
      if (idx || slice.style !== dia.style) {
        tag.r = slice.style === dia.style ? '' : slice.style;
      }
      if (!idx) {
        if (style.Alignment !== dia.alignment) {
          tag.an = dia.alignment;
        }
        ['pos', 'org', 'move', 'fade', 'clip'].forEach((key) => {
          if (dia[key]) {
            tag[key] = dia[key];
          }
        });
      }
      // make sure additional tags are first
      sliceCopy.fragments[0].tag = Object.assign(tag, sliceCopy.fragments[0].tag);
      return sliceCopy;
    })
    .map(decompileSlice)
    .join('');
}

function getMargin(margin, styleMargin) {
  return margin === styleMargin ? '0000' : margin;
}

export function decompileDialogue(dia, style) {
  return `Dialogue: ${[
    dia.layer,
    stringifyTime(dia.start),
    stringifyTime(dia.end),
    dia.style,
    dia.name,
    getMargin(dia.margin.left, style.MarginL),
    getMargin(dia.margin.right, style.MarginR),
    getMargin(dia.margin.vertical, style.MarginV),
    stringifyEffect(dia.effect),
    decompileText(dia, style),
  ].join()}`;
}

// --- editable documents -------------------------------------------------

// Emit the tags that turn the current origins into the fragment's origins.
// Relative font sizes, empty color resets and explicit equal-value margins
// are written out as plain ASS, so a recompile recovers the same origins
// without any private annotations.
function decompileEditableText(dia, style) {
  return dia.slices
    .filter((slice) => slice.fragments.length)
    .map((slice, idx) => {
      let head = '';
      if (idx || slice.style !== dia.style) {
        head += slice.style === dia.style ? '\\r' : `\\r${slice.style}`;
      }
      if (!idx) {
        if (style.Alignment !== dia.alignment) {
          head += `\\an${dia.alignment}`;
        }
        ['pos', 'org', 'move', 'fade', 'clip'].forEach((key) => {
          if (dia[key]) {
            head += decompileTag({ [key]: dia[key] });
          }
        });
      }
      const current = freshOrigins();
      return slice.fragments.map((fragment, fragmentIdx) => {
        const { origin } = fragment;
        let tags = fragmentIdx === 0 ? head : '';
        const fsReset = origin.fs.type === 'inherit' && current.fs.type !== 'inherit';
        const a1Reset = origin.a1.type === 'inherit' && current.a1.type !== 'inherit';
        if (fsReset || a1Reset) {
          // no standalone tag resets the font size or alpha back to the
          // style, so fall back to `\r` and re-apply what is still needed
          tags += slice.style === dia.style ? '\\r' : `\\r${slice.style}`;
          Object.assign(current, freshOrigins());
        }
        const { fs, c1, a1, ...rest } = fragment.tag;
        tags += decompileTag(rest);
        if (!originEqual(origin.fs, current.fs)) {
          tags += fsOriginToTags(current.fs, origin.fs);
        }
        if (!originEqual(origin.c1, current.c1)) {
          tags += origin.c1.type === 'inherit' ? '\\c' : `\\1c&H${origin.c1.value}&`;
        }
        if (!originEqual(origin.a1, current.a1)) {
          tags += `\\1a&H${origin.a1.value}&`;
        }
        current.fs = origin.fs;
        current.c1 = origin.c1;
        current.a1 = origin.a1;
        const content = fragment.drawing ? decompileDrawing(fragment.drawing) : fragment.text;
        return `${tags ? `{${tags}}` : ''}${content}`;
      }).join('');
    })
    .join('');
}

function decompileEditableDialogue(dia, styles) {
  const style = (styles[dia.style] || styles.Default).style;
  const { margin } = dia.origin;
  return `Dialogue: ${[
    dia.layer,
    stringifyTime(dia.start),
    stringifyTime(dia.end),
    dia.style,
    dia.name,
    margin.left === null ? '0000' : dia.margin.left,
    margin.right === null ? '0000' : dia.margin.right,
    margin.vertical === null ? '0000' : dia.margin.vertical,
    stringifyEffect(dia.effect),
    decompileEditableText(dia, style),
  ].join()}`;
}

export function decompile(input) {
  const editable = !!(input && input.editableStyles);
  // reconcile on a clone so direct edits are honored without mutating the input
  const { info, width, height, collisions, styles, dialogues } = editable
    ? reconcileDocument(JSON.parse(JSON.stringify(input)))
    : input;
  return [
    '[Script Info]',
    stringifyInfo(Object.assign({}, info, {
      PlayResX: width,
      PlayResY: height,
      Collisions: collisions,
    })),
    '',
    '[V4+ Styles]',
    `Format: ${stylesFormat.join(', ')}`,
    ...Object.keys(styles).map((name) => decompileStyle(styles[name])),
    '',
    '[Events]',
    `Format: ${eventsFormat.join(', ')}`,
    ...dialogues
      .sort((x, y) => x.start - y.start || x.end - y.end)
      .map((dia) => (editable
        ? decompileEditableDialogue(dia, styles)
        : decompileDialogue(dia, styles[dia.style].style))),
    '',
  ].join('\n');
}
