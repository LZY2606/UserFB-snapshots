// Origin records track where a fragment's effective `fs`/`c1`/`a1` and a
// dialogue's margins come from, so `updateStyles` can re-project them
// against patched styles without replaying the original ASS text.
//
// Font size origin:
//   { type: 'inherit' }                          follow the slice style
//   { type: 'value', value: 30 }                 absolute override (`\fs30`)
//   { type: 'relative', factor: 1.2, base: ... } scaled base (`\fs+2`)
// Color/alpha origin:
//   { type: 'inherit' } | { type: 'value', value: 'BBGGRR' | 'AA' }
//
// All records are plain JSON so they survive `JSON.stringify`/`JSON.parse`.

const MARGIN_SIDES = [
  ['left', 'MarginL'],
  ['right', 'MarginR'],
  ['vertical', 'MarginV'],
];

const COLOR_PATTERN = /^&H([0-9a-fA-F]{8})&?$/;

export function inheritOrigin() {
  return { type: 'inherit' };
}

export function valueOrigin(value) {
  return { type: 'value', value };
}

export function freshOrigins() {
  return { fs: inheritOrigin(), c1: inheritOrigin(), a1: inheritOrigin() };
}

// mirrors the `\fs` rules of compileTag in compiler/tag.js
export function fsOriginFromTag(value, base) {
  if (/^\+|-/.test(value)) {
    const num = value * 1;
    return { type: 'relative', factor: num > -10 ? 1 + num / 10 : 1, base };
  }
  return { type: 'value', value: value * 1 };
}

export function evalFsOrigin(origin, styleTag) {
  if (origin.type === 'relative') {
    // always recompute from the saved dependency and the current style,
    // never scale the previously projected value again
    return origin.factor * evalFsOrigin(origin.base, styleTag);
  }
  return origin.type === 'inherit' ? styleTag.fs : origin.value;
}

export function evalColorOrigin(origin, styleValue) {
  return origin.type === 'inherit' ? styleValue : origin.value;
}

export function originEqual(a, b) {
  if (a === b) return true;
  if (!a || !b || a.type !== b.type) return false;
  if (a.type === 'inherit') return true;
  if (a.type === 'value') return a.value === b.value;
  return a.factor === b.factor && originEqual(a.base, b.base);
}

// tags that transition the effective font size from `current` to `target`
export function fsOriginToTags(current, target) {
  if (target.type === 'value') {
    return `\\fs${target.value}`;
  }
  const prefix = originEqual(current, target.base)
    ? ''
    : fsOriginToTags(current, target.base);
  const n = Math.round((target.factor - 1) * 10);
  return `${prefix}\\fs${n >= 0 ? '+' : ''}${n}`;
}

function pickStyle(styles, name, fallback) {
  if (styles[name]) return styles[name];
  if (styles[fallback]) return styles[fallback];
  return styles.Default;
}

function reconcileFragment(fragment, sliceTag) {
  if (!fragment.origin) {
    // a fragment created by hand: existing tag fields are absolute
    // overrides, missing fields inherit from the slice style
    fragment.origin = { fs: null, c1: null, a1: null, projected: {} };
  }
  const { origin, tag } = fragment;
  ['fs', 'c1', 'a1'].forEach((field) => {
    const current = tag[field];
    if (current === undefined) {
      // a removed field drops both the override and relative dependencies
      origin[field] = inheritOrigin();
    } else if (origin[field] === null || current !== origin.projected[field]) {
      // a field that differs from the last projection is a new
      // absolute override, even if it equals the old style value
      origin[field] = valueOrigin(current);
    }
  });
  const projected = {
    fs: evalFsOrigin(origin.fs, sliceTag),
    c1: evalColorOrigin(origin.c1, sliceTag.c1),
    a1: evalColorOrigin(origin.a1, sliceTag.a1),
  };
  tag.fs = projected.fs;
  tag.c1 = projected.c1;
  tag.a1 = projected.a1;
  origin.projected = projected;
}

// Re-derive every projected value of `doc` from its origins and current
// styles, honoring direct edits made since the last projection. Mutates
// `doc`; callers pass a clone when the input must stay untouched.
export function reconcileDocument(doc) {
  const { styles } = doc;
  for (let i = 0; i < doc.dialogues.length; i++) {
    const dia = doc.dialogues[i];
    const diaStyle = pickStyle(styles, dia.style, 'Default');
    if (!dia.origin) {
      dia.origin = {
        margin: { left: null, right: null, vertical: null },
        projectedMargin: {},
      };
    }
    for (let j = 0; j < MARGIN_SIDES.length; j++) {
      const [side, key] = MARGIN_SIDES[j];
      const current = dia.margin[side];
      const projected = dia.origin.projectedMargin[side];
      if (current === undefined || current === null || current === 0) {
        // a zeroed or removed margin falls back to the style
        if (current !== projected) dia.origin.margin[side] = null;
      } else if (current !== projected) {
        dia.origin.margin[side] = current;
      }
      const value = dia.origin.margin[side] === null
        ? diaStyle.style[key]
        : dia.origin.margin[side];
      dia.margin[side] = value;
      dia.origin.projectedMargin[side] = value;
    }
    for (let j = 0; j < dia.slices.length; j++) {
      const slice = dia.slices[j];
      const sliceTag = pickStyle(styles, slice.style, dia.style).tag;
      for (let k = 0; k < slice.fragments.length; k++) {
        reconcileFragment(slice.fragments[k], sliceTag);
      }
    }
  }
  return doc;
}

function validateChanges(styles, changes) {
  if (!changes || typeof changes !== 'object' || Array.isArray(changes)) {
    throw new Error('updateStyles: changes must be a patch object keyed by style name');
  }
  Object.entries(changes).forEach(([name, patch]) => {
    if (!styles[name]) {
      throw new Error(`updateStyles: unknown style "${name}"`);
    }
    if (!patch || typeof patch !== 'object' || Array.isArray(patch)) {
      throw new Error(`updateStyles: patch for style "${name}" must be an object`);
    }
    Object.entries(patch).forEach(([field, value]) => {
      if (field === 'Fontsize') {
        if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) {
          throw new Error(`updateStyles: Fontsize of style "${name}" must be a positive finite number`);
        }
      } else if (field === 'PrimaryColour') {
        if (typeof value !== 'string' || !COLOR_PATTERN.test(value)) {
          throw new Error(`updateStyles: PrimaryColour of style "${name}" must be &HAABBGGRR or &HAABBGGRR&`);
        }
      } else if (field === 'MarginL' || field === 'MarginR' || field === 'MarginV') {
        if (typeof value !== 'number' || !Number.isInteger(value) || value < 0) {
          throw new Error(`updateStyles: ${field} of style "${name}" must be a non-negative integer`);
        }
      } else {
        throw new Error(`updateStyles: cannot update field "${field}" of style "${name}"`);
      }
    });
  });
}

export function updateStyles(document, changes) {
  if (!document || typeof document !== 'object' || !document.editableStyles) {
    throw new Error('updateStyles: document must be compiled with the editableStyles option');
  }
  // validate everything before touching anything, so a bad patch leaves
  // the input document and the changes object unmodified
  validateChanges(document.styles, changes);
  const doc = JSON.parse(JSON.stringify(document));
  Object.entries(changes).forEach(([name, patch]) => {
    const target = doc.styles[name];
    if (patch.Fontsize !== undefined) {
      target.style.Fontsize = patch.Fontsize;
      target.tag.fs = patch.Fontsize;
    }
    if (patch.PrimaryColour !== undefined) {
      const hex = patch.PrimaryColour.match(COLOR_PATTERN)[1].toUpperCase();
      target.style.PrimaryColour = `&H${hex}&`;
      target.tag.a1 = hex.slice(0, 2);
      target.tag.c1 = hex.slice(2);
    }
    MARGIN_SIDES.forEach(([, key]) => {
      if (patch[key] !== undefined) {
        target.style[key] = patch[key];
      }
    });
  });
  return reconcileDocument(doc);
}
