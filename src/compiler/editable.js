import { compileDrawing } from './drawing.js';
import { compileTag } from './tag.js';

const globalTags = ['r', 'a', 'an', 'pos', 'org', 'move', 'fade', 'fad', 'clip'];
const a2an = [
  null, 1, 2, 3,
  null, 7, 8, 9,
  null, 4, 5, 6,
];
const marginFields = [
  ['left', 'MarginL'],
  ['right', 'MarginR'],
  ['vertical', 'MarginV'],
];
const changeFields = ['Fontsize', 'PrimaryColour', 'MarginL', 'MarginR', 'MarginV'];

export function clone(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function inheritTag(pTag) {
  return JSON.parse(JSON.stringify(Object.assign({}, pTag, {
    k: undefined,
    kf: undefined,
    ko: undefined,
    kt: undefined,
  })));
}

function isFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function isColor(value) {
  return typeof value === 'string' && /^[0-9a-fA-F]{6}$/.test(value);
}

function isAlpha(value) {
  return typeof value === 'string' && /^[0-9a-fA-F]{2}$/.test(value);
}

// A font size source is one of:
//   { origin: 'style' }                              - follow the current slice style
//   { origin: 'explicit', value }                    - absolute override
//   { origin: 'relative', factor, base }             - current base multiplied by factor
// The base is itself a font size source, so relative chains keep their real base.
// Color sources only have `style` and `explicit` origins.
export function createMeta() {
  return {
    fs: { origin: 'style' },
    c1: { origin: 'style' },
    a1: { origin: 'style' },
  };
}

function fsSourceValid(fs) {
  if (!fs || typeof fs !== 'object') return false;
  if (fs.origin === 'style') return true;
  if (fs.origin === 'explicit') return isFiniteNumber(fs.value);
  if (fs.origin === 'relative') {
    return isFiniteNumber(fs.factor) && fsSourceValid(fs.base);
  }
  return false;
}

function colorSourceValid(color) {
  if (!color || typeof color !== 'object') return false;
  if (color.origin === 'style') return true;
  if (color.origin !== 'explicit') return false;
  if (color.kind === 'alpha') return isAlpha(color.value);
  return isColor(color.value);
}

export function isValidMeta(meta) {
  return !!meta
    && typeof meta === 'object'
    && !Array.isArray(meta)
    && fsSourceValid(meta.fs)
    && colorSourceValid(meta.c1)
    && colorSourceValid(meta.a1);
}

export function pickStyleName(name, styles, fallback) {
  if (name && styles[name]) return name;
  if (fallback && styles[fallback]) return fallback;
  const keys = Object.keys(styles);
  return keys.length ? keys[0] : null;
}

/**
 * The style hash a fragment is resolved against. A fragment with a `\r`
 * applies it to its own scope, so `slice.style` (or `\rName`) wins; otherwise
 * the containing slice decides, so retargeting `slice.style` re-evaluates
 * copied sources.
 */
export function fragmentStyle(fragment, slice, dialogue, styles) {
  const nested = fragment.tag && fragment.tag.r;
  if (nested !== undefined) {
    const name = nested === '' ? dialogue.style : nested;
    return pickStyleName(name, styles, dialogue.style);
  }
  return pickStyleName(slice.style, styles, dialogue.style);
}

export function resolveFs(fs, styleName, styles) {
  if (fs.origin === 'explicit') return fs.value;
  if (fs.origin === 'relative') {
    const base = resolveFs(fs.base, styleName, styles);
    return base === undefined ? undefined : base * fs.factor;
  }
  const name = pickStyleName(styleName, styles);
  return name === null ? undefined : styles[name].tag.fs;
}

function resolveColor(color, styleName, styles) {
  if (color.origin === 'explicit') return color.value;
  const name = pickStyleName(styleName, styles);
  return name === null ? undefined : styles[name].tag.c1;
}

function resolveAlpha(a1, styleName, styles) {
  if (a1.origin === 'explicit') return a1.value;
  const name = pickStyleName(styleName, styles);
  return name === null ? undefined : styles[name].tag.a1;
}

export function projectMeta(fragment, meta, styleName, styles) {
  if (!meta) {
    delete fragment.tag.fs;
    delete fragment.tag.c1;
    delete fragment.tag.a1;
    delete fragment.meta;
    return;
  }
  fragment.tag.fs = resolveFs(meta.fs, styleName, styles);
  fragment.tag.c1 = resolveColor(meta.c1, styleName, styles);
  fragment.tag.a1 = resolveAlpha(meta.a1, styleName, styles);
  fragment.meta = {
    fs: clone(meta.fs),
    c1: clone(meta.c1),
    a1: clone(meta.a1),
    projected: {
      fs: fragment.tag.fs,
      c1: fragment.tag.c1,
      a1: fragment.tag.a1,
    },
  };
}

function fsFromTagValue(value, base) {
  if (isFiniteNumber(value)) {
    return { origin: 'explicit', value };
  }
  if (typeof value === 'string') {
    if (/^[+-]\d+(?:\.\d+)?$/.test(value)) {
      const n = value * 1;
      return { origin: 'relative', factor: n > -10 ? 1 + n / 10 : 1, base };
    }
    const n = value * 1;
    if (Number.isFinite(n)) return { origin: 'explicit', value: n };
  }
  return { origin: 'style' };
}

function colorFromTagValue(value, kind) {
  if (kind === 'alpha' ? isAlpha(value) : isColor(value)) {
    return { origin: 'explicit', value, kind: kind === 'alpha' ? 'alpha' : 'color' };
  }
  return { origin: 'style' };
}

function sameSource(a, b) {
  if (!a || !b || a.origin !== b.origin) return false;
  if (a.origin === 'explicit') return a.value === b.value;
  if (a.origin === 'style') return true;
  if (a.origin === 'relative') return a.factor === b.factor && sameSource(a.base, b.base);
  return false;
}

function styleSource(origin) {
  return origin && origin.origin === 'style';
}

/**
 * Work out the source of each editable property after possible direct edits to
 * `fragment.tag`. The previous projection is stored so this is idempotent: a
 * value that still equals the old projection keeps its original source.
 */
function syncFragmentMeta(fragment, styleName, oldStyles, fallbackStyle) {
  const tag = fragment.tag || {};
  const meta = isValidMeta(fragment.meta) ? fragment.meta : null;
  const projected = meta && meta.projected;
  const style = pickStyleName(styleName, oldStyles, fallbackStyle);

  function previous(source, key, kind) {
    if (!meta) return undefined;
    if (projected) return projected[key];
    if (kind === 'fs') return resolveFs(source, style, oldStyles);
    if (kind === 'c1') return resolveColor(source, style, oldStyles);
    return resolveAlpha(source, style, oldStyles);
  }

  function decide(key, kind) {
    const has = Object.prototype.hasOwnProperty.call(tag, key);
    const value = tag[key];
    if (!meta) {
      if (!has) return { origin: 'style' };
      return kind === 'fs'
        ? fsFromTagValue(value, { origin: 'style' })
        : colorFromTagValue(value, kind);
    }
    if (!has) return { origin: 'style' };
    const source = meta[key];
    const old = previous(source, key, kind);
    if (value === old) return clone(source);
    return kind === 'fs'
      ? fsFromTagValue(value, source && source.origin ? clone(source) : { origin: 'style' })
      : colorFromTagValue(value, kind);
  }

  const next = {
    fs: decide('fs', 'fs'),
    c1: decide('c1', 'c1'),
    a1: decide('a1', 'a1'),
  };
  // Keep the alpha marker so a reload can validate the string shape.
  next.a1 = Object.assign({}, next.a1, { kind: next.a1.origin === 'explicit' ? 'alpha' : undefined });
  if (next.a1.kind === undefined) delete next.a1.kind;
  return next;
}

function fmtFactor(factor) {
  const n = Math.round((factor - 1) * 10);
  const sign = n >= 0 ? '+' : '-';
  return `\\fs${sign}${Math.abs(n)}`;
}

function fsChain(fs) {
  if (!fs) return null;
  const chain = [];
  let node = fs;
  while (node && node.origin === 'relative') {
    chain.unshift({ type: 'relative', factor: node.factor });
    node = node.base;
  }
  if (node && node.origin === 'explicit') {
    chain.unshift({ type: 'explicit', value: node.value });
  } else {
    chain.unshift({ type: 'style' });
  }
  return chain;
}

function nodeEquals(a, b) {
  if (!a || !b || a.type !== b.type) return false;
  if (a.type === 'explicit') return a.value === b.value;
  if (a.type === 'relative') return a.factor === b.factor;
  return true;
}

function chainEquals(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  return a.every((node, i) => nodeEquals(node, b[i]));
}

function chainTags(chain, from) {
  const tags = [];
  for (let i = from; i < chain.length; i++) {
    tags.push(chain[i].type === 'explicit'
      ? `\\fs${chain[i].value}`
      : fmtFactor(chain[i].factor));
  }
  return tags;
}

/**
 * Express a font size source as plain ASS tags relative to the inherited one.
 * Returns `{ reset, tags }`. `reset` means a bare/named `\r` is required
 * because the inherited font size cannot be turned back into a style source.
 */
function planFs(meta, inherited) {
  const chain = fsChain(meta);
  if (chain === null) return { reset: false, tags: [] };
  const inheritedChain = fsChain(inherited);
  if (chainEquals(chain, inheritedChain)) return { reset: false, tags: [] };
  if (chain[0].type === 'explicit') {
    return { reset: false, tags: chainTags(chain, 0) };
  }
  // Style-rooted source: it can only continue from an inherited source that is
  // an exact prefix of its chain.
  if (inheritedChain) {
    let k = 0;
    while (k < inheritedChain.length && k < chain.length
      && nodeEquals(inheritedChain[k], chain[k])) {
      k++;
    }
    if (k === inheritedChain.length && k <= chain.length && k > 0) {
      return { reset: false, tags: chainTags(chain, k) };
    }
  }
  return { reset: true, tags: chainTags(chain, 1) };
}

/**
 * Build the editable part of a fragment's ASS tag text.
 * @param {Object} meta      source of the current fragment
 * @param {Object|null} inherited source inherited from the previous fragment
 * @param {String} resetName style to return to with `\r` when a reset is needed
 * @param {Boolean} alphaCombined emit the alpha override as `\alpha` again
 */
export function editableTagText(meta, inherited, resetName, alphaCombined) {
  const combined = alphaCombined === undefined
    ? !!(meta.a1 && meta.a1.origin === 'explicit' && meta.a1.shorthand)
    : alphaCombined;
  const inheritedMeta = inherited === null ? null : (inherited || createMeta());
  let fsPlan = planFs(meta.fs, inheritedMeta && inheritedMeta.fs);
  const resetForAlpha = !styleSource(meta.a1)
    ? false
    : !(inheritedMeta && styleSource(inheritedMeta.a1));
  const reset = fsPlan.reset || resetForAlpha;
  const parts = [];
  if (reset) {
    parts.push(`\\r${resetName || ''}`);
    fsPlan = planFs(meta.fs, { origin: 'style' });
  }
  parts.push(...fsPlan.tags);

  // RGB: an empty `\c` clears an explicit override and re-inherits the style
  // color while keeping any explicit alpha override.
  if (!styleSource(meta.c1)) {
    if (reset || !inheritedMeta || !styleSource(inheritedMeta.c1)
      || inheritedMeta.c1.value !== meta.c1.value) {
      parts.push(`\\1c&H${meta.c1.value}&`);
    }
  } else if (!reset && inheritedMeta && !styleSource(inheritedMeta.c1)) {
    parts.push('\\c');
  }

  if (!styleSource(meta.a1)) {
    if (reset || !inheritedMeta || !styleSource(inheritedMeta.a1)
      || inheritedMeta.a1.value !== meta.a1.value) {
      parts.push(alphaCombined ? `\\alpha&H${meta.a1.value}&` : `\\1a&H${meta.a1.value}&`);
    }
  }
  return { reset, tags: parts, alphaShorthand: alphaCombined && !styleSource(meta.a1) };
}

// ---------------------------------------------------------------------------
// Compiling with editable style sources
// ---------------------------------------------------------------------------

function assertNoAnimation(parsed) {
  for (let i = 0; i < parsed.length; i++) {
    const tags = parsed[i].tags || [];
    for (let j = 0; j < tags.length; j++) {
      if (tags[j] && tags[j].t !== undefined) {
        throw new Error('editableStyles does not support \\t animation tags');
      }
    }
  }
}

export function compileTextEditable({ styles, style, parsed, start, end }) {
  assertNoAnimation(parsed);
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
  let prevMeta = createMeta();
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
    // A `\r`/`\rName` applies to the fragment it appears in, so resolve that
    // fragment's tags against the style it switches to.
    const sliceStyle = reset === undefined
      ? slice.style
      : (styles[reset] ? reset : style);
    const inheritedMeta = reset === undefined ? clone(prevMeta) : createMeta();
    let meta = clone(inheritedMeta);
    let alphaShorthand = false;
    let semanticTag = false;
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
        semanticTag = true;
        const sliceTag = styles[sliceStyle].tag;
        const presetFs = resolveFs(meta.fs, sliceStyle, styles);
        if (key === 'fs') {
          meta.fs = fsFromTagValue(tag.fs, clone(meta.fs));
        } else if (key === 'c1') {
          meta.c1 = colorFromTagValue(tag.c1, 'color');
        } else if (key === 'a1') {
          meta.a1 = colorFromTagValue(tag.a1, 'alpha');
        } else if (key === 'alpha') {
          meta.a1 = colorFromTagValue(tag.alpha, 'alpha');
          alphaShorthand = meta.a1.origin === 'explicit';
          if (alphaShorthand) meta.a1.shorthand = true;
          Object.assign(fragment.tag, compileTag(tag, key, {
            start, end, c1: sliceTag.c1, c2: sliceTag.c2, c3: sliceTag.c3, c4: sliceTag.c4, fs: presetFs,
          }));
        } else {
          Object.assign(fragment.tag, compileTag(tag, key, {
            start, end, c1: sliceTag.c1, c2: sliceTag.c2, c3: sliceTag.c3, c4: sliceTag.c4, fs: presetFs,
          }));
        }
      }
    }
    projectMeta(fragment, meta, sliceStyle, styles);
    // Keep the older "merge untagged text" behavior: only an actual tag (or a
    // real change of an editable source) makes a separate fragment.
    const metaChanged = !sameSource(meta.fs, inheritedMeta.fs)
      || !sameSource(meta.c1, inheritedMeta.c1)
      || !sameSource(meta.a1, inheritedMeta.a1);
    const keepsSeparate = semanticTag || metaChanged;
    prevTag = fragment.tag;
    prevMeta = meta;
    if (reset !== undefined) {
      slices.push(slice);
      slice = { style: styles[reset] ? reset : style, fragments: [] };
    }
    if (fragment.text || fragment.drawing) {
      const prev = slice.fragments[slice.fragments.length - 1] || {};
      if (prev.text && fragment.text && !keepsSeparate) {
        prev.text += fragment.text;
      } else {
        fragment.alphaShorthand = alphaShorthand;
        slice.fragments.push(fragment);
      }
    }
  }
  slices.push(slice);

  return Object.assign({ alignment, slices }, q, pos, org, move, fade, clip);
}

// ---------------------------------------------------------------------------
// updateStyles
// ---------------------------------------------------------------------------

function normalizeChanges(changes, styles) {
  if (!changes || typeof changes !== 'object' || Array.isArray(changes)) {
    throw new Error('changes must be an object keyed by existing style names');
  }
  const plan = {};
  Object.keys(changes).forEach((name) => {
    if (!Object.prototype.hasOwnProperty.call(styles, name)) {
      throw new Error(`Unknown style: ${name}`);
    }
    const patch = changes[name];
    if (!patch || typeof patch !== 'object' || Array.isArray(patch)) {
      throw new Error(`Style changes for "${name}" must be an object`);
    }
    const out = {};
    Object.keys(patch).forEach((field) => {
      const value = patch[field];
      if (!~changeFields.indexOf(field)) {
        throw new Error(`Unknown style field: ${field}`);
      }
      if (field === 'Fontsize') {
        if (!isFiniteNumber(value) || value <= 0) {
          throw new Error('Fontsize must be a positive finite number');
        }
        out.Fontsize = value;
      } else if (field === 'PrimaryColour') {
        const match = typeof value === 'string' && value.match(/^&H([0-9a-fA-F]{8})&?$/);
        if (!match) {
          throw new Error('PrimaryColour must look like &HAABBGGRR or &HAABBGGRR&');
        }
        out.PrimaryColour = {
          a: match[1].slice(0, 2).toUpperCase(),
          c: match[1].slice(2).toUpperCase(),
        };
      } else {
        if (!Number.isInteger(value) || value < 0) {
          throw new Error(`${field} must be a non-negative integer`);
        }
        out[field] = value;
      }
    });
    plan[name] = out;
  });
  return plan;
}

function updateMargins(dialogue, oldStyles, newStyles) {
  if (!dialogue.marginMeta) {
    throw new Error('updateStyles requires a document compiled with { editableStyles: true }');
  }
  dialogue.margin = dialogue.margin || {};
  const meta = dialogue.marginMeta;
  // The style the margins inherited from last time; empty for a legacy object
  // without a saved projection.
  const oldStyleName = meta.style && oldStyles[meta.style] ? meta.style : null;
  const newStyle = newStyles[dialogue.style] || newStyles.Default;
  marginFields.forEach(([key, prop]) => {
    const value = dialogue.margin[key];
    let explicit;
    let inheritedFrom;
    if (!isFiniteNumber(value) || value === 0) {
      explicit = false;
    } else if (!oldStyleName) {
      // No previous projection: compare against the old style values.
      const oldStyle = oldStyles[dialogue.style] || oldStyles.Default;
      inheritedFrom = oldStyle ? oldStyle.style[prop] : undefined;
      explicit = value !== inheritedFrom;
    } else {
      inheritedFrom = oldStyles[oldStyleName].style[prop];
      explicit = meta[key] || value !== inheritedFrom;
    }
    dialogue.margin[key] = explicit ? value : newStyle.style[prop];
    meta[key] = explicit;
  });
  meta.style = dialogue.style;
}

export function updateStyles(document, changes) {
  if (!document || typeof document !== 'object' || document.editableStyles !== true) {
    throw new Error('updateStyles requires a document compiled with { editableStyles: true }');
  }
  const plan = normalizeChanges(changes, document.styles);
  const oldStyles = document.styles;
  const next = clone(document);
  const newStyles = next.styles;

  Object.keys(plan).forEach((name) => {
    const patch = plan[name];
    const style = newStyles[name];
    if (patch.Fontsize !== undefined) {
      style.style.Fontsize = patch.Fontsize;
      style.tag.fs = patch.Fontsize;
    }
    if (patch.PrimaryColour) {
      style.style.PrimaryColour = `&H${patch.PrimaryColour.a}${patch.PrimaryColour.c}`;
      style.tag.a1 = patch.PrimaryColour.a;
      style.tag.c1 = patch.PrimaryColour.c;
    }
    ['MarginL', 'MarginR', 'MarginV'].forEach((field) => {
      if (patch[field] !== undefined) style.style[field] = patch[field];
    });
  });

  next.dialogues.forEach((dialogue) => {
    updateMargins(dialogue, oldStyles, newStyles);
    (dialogue.slices || []).forEach((slice) => {
      (slice.fragments || []).forEach((fragment) => {
        const sliceStyle = fragmentStyle(fragment, slice, dialogue, newStyles);
        const meta = syncFragmentMeta(fragment, sliceStyle, oldStyles, dialogue.style);
        projectMeta(fragment, meta, sliceStyle, newStyles);
      });
    });
  });

  return next;
}
