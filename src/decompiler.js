import { stringifyInfo, stringifyTime, stringifyEffect } from './stringifier.js';
import { stylesFormat, eventsFormat } from './utils.js';
import { createMeta, editableTagText, isValidMeta } from './compiler/editable.js';

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

const editableKeys = ['r', 'fs', 'c1', 'a1'];

function marginText(dialogue, key, prop, style) {
  const value = dialogue.margin ? dialogue.margin[key] : 0;
  if (!value) return '0000';
  const explicit = dialogue.marginMeta
    ? !!dialogue.marginMeta[key]
    : value !== style[prop];
  return explicit ? value : '0000';
}

/**
 * Decompile a dialogue whose fragments carry editable styles. Font sizes,
 * primary colors and alpha are emitted from their saved sources, so relative
 * sizes, `\c` color-only resets and explicit margins equal to the style value
 * all survive without private annotations.
 */
export function decompileTextEditable(dia, style) {
  let inherited = createMeta();
  let currentStyle = dia.style;
  let emitted = 0;
  const chunks = [];
  dia.slices.forEach((slice) => {
    if (!slice.fragments.length) return;
    const firstSlice = emitted === 0;
    emitted++;
    const baseTags = {};
    if (slice.style !== currentStyle) {
      baseTags.r = slice.style === dia.style ? '' : slice.style;
      currentStyle = slice.style;
      inherited = createMeta();
    }
    if (firstSlice) {
      if (style.Alignment !== dia.alignment) {
        baseTags.an = dia.alignment;
      }
      ['pos', 'org', 'move', 'fade', 'clip'].forEach((key) => {
        if (dia[key]) baseTags[key] = dia[key];
      });
    }
    let firstFragment = true;
    slice.fragments.forEach((fragment) => {
      const source = isValidMeta(fragment.meta) ? fragment.meta : createMeta();
      const fragmentTag = {};
      if (firstFragment) Object.assign(fragmentTag, baseTags);
      Object.keys(fragment.tag).forEach((key) => {
        if (!~editableKeys.indexOf(key)) fragmentTag[key] = fragment.tag[key];
      });
      let editable = editableTagText(source, inherited, currentStyle, !!fragment.alphaShorthand);
      if (editable.reset && fragmentTag.r === undefined) {
        // A bare `\r` may be followed by a style name; turn the reset into the
        // style switch it needs to reproduce the saved source.
        fragmentTag.r = currentStyle === dia.style ? '' : currentStyle;
        inherited = createMeta();
        editable = editableTagText(source, inherited, currentStyle, !!fragment.alphaShorthand);
      }
      firstFragment = false;
      const tagText = Object.keys(fragmentTag).map((key) => {
        const fn = tagDecompiler[key] || ((_) => `${key}${_}`);
        return `\\${fn(fragmentTag[key])}`;
      }).concat(editable.tags).join('');
      chunks.push(`${tagText ? `{${tagText}}` : ''}${
        fragment.drawing ? decompileDrawing(fragment.drawing) : fragment.text
      }`);
      inherited = source;
    });
  });
  return chunks.join('');
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
  const editable = dia.marginMeta !== undefined
    || dia.slices.some((slice) => slice.fragments.some((fragment) => fragment.meta !== undefined));
  return `Dialogue: ${[
    dia.layer,
    stringifyTime(dia.start),
    stringifyTime(dia.end),
    dia.style,
    dia.name,
    editable ? marginText(dia, 'left', 'MarginL', style) : getMargin(dia.margin.left, style.MarginL),
    editable ? marginText(dia, 'right', 'MarginR', style) : getMargin(dia.margin.right, style.MarginR),
    editable ? marginText(dia, 'vertical', 'MarginV', style) : getMargin(dia.margin.vertical, style.MarginV),
    stringifyEffect(dia.effect),
    editable ? decompileTextEditable(dia, style) : decompileText(dia, style),
  ].join()}`;
}

export function decompile({ info, width, height, collisions, styles, dialogues }) {
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
      .slice()
      .sort((x, y) => x.start - y.start || x.end - y.end)
      .map((dia) => decompileDialogue(dia, styles[dia.style].style)),
    '',
  ].join('\n');
}
