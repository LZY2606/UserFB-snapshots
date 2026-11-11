// Checks font.layoutWithSource: every output glyph carries the UTF-16 source
// ranges of the input string it came from. Run via checks/check_source_ranges.sh,
// which builds dist/ and generates the synthetic test font in a temp dir.
//
// usage: node checks/check_source_ranges.mjs <path-to-source-ranges-test.ttf>

import { openSync } from '../dist/node/index.js';
import assert from 'node:assert/strict';

const synthPath = process.argv[2];
if (!synthPath) {
  console.error('usage: node check_source_ranges.mjs <synthetic-font-path>');
  process.exit(1);
}

const continuation = openSync('test/data/lookup-continuation.ttf');
const synth = openSync(synthPath);
const amiri = openSync('test/data/amiri/amiri-regular.ttf');
const cmap14 = openSync('test/data/fonttest/TestCMAP14.otf');
const emoji = openSync('test/data/ss-emoji/ss-emoji-apple.ttf');
const fira = openSync('test/data/FiraSans/FiraSans-Regular.ttf');
const morx = openSync('test/data/fonttest/TestMORXEleven.ttf');
const devanagari = openSync(
  'test/data/NotoSans/NotoSansDevanagari-Regular.ttf',
);

// Prints the character ranges, glyphs, and positions of a run side by side.
function show(label, run) {
  console.log(`\n${label}`);
  console.log(
    '  idx | glyph | source ranges      | xAdvance | xOffset | yOffset',
  );
  for (let i = 0; i < run.glyphs.length; i++) {
    let ranges = JSON.stringify(run.sourceRanges[i]).padEnd(18);
    let p = run.positions[i];
    console.log(
      `  ${String(i).padStart(3)} | ${String(run.glyphs[i].id).padStart(5)} | ${ranges} | ${String(p.xAdvance).padStart(8)} | ${String(p.xOffset).padStart(7)} | ${p.yOffset}`,
    );
  }
}

// Asserts that layoutWithSource produces the same glyphs, positions, and
// paths as layout for the same arguments.
function assertConsistentWithLayout(font, args, label) {
  let tracked = font.layoutWithSource(...args);
  let plain = font.layout(...args);
  assert.deepEqual(
    tracked.glyphs.map((g) => g.id),
    plain.glyphs.map((g) => g.id),
    `${label}: glyph ids differ from layout`,
  );
  assert.deepEqual(
    tracked.positions.map((p) => ({ ...p })),
    plain.positions.map((p) => ({ ...p })),
    `${label}: positions differ from layout`,
  );
  assert.deepEqual(
    tracked.glyphs.map((g) => g.path.toSVG()),
    plain.glyphs.map((g) => g.path.toSVG()),
    `${label}: paths differ from layout`,
  );
  assert.equal(
    plain.sourceRanges,
    undefined,
    `${label}: layout must not return sourceRanges`,
  );
  assert.equal(
    tracked.sourceRanges.length,
    tracked.glyphs.length,
    `${label}: sourceRanges must align with glyphs`,
  );
  return tracked;
}

// --- One-to-many substitution -------------------------------------------------
// lookup-continuation.ttf expands each 'a' to two glyphs (b a).
{
  let run = assertConsistentWithLayout(continuation, ['aa'], 'one-to-many');
  show("one-to-many: 'aa'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [2, 1, 2, 1],
  );
  assert.deepEqual(run.sourceRanges, [[[0, 1]], [[0, 1]], [[1, 2]], [[1, 2]]]);

  // 'i' expands to three glyphs (y acutecomb i), each inheriting its source.
  run = assertConsistentWithLayout(continuation, ['ii'], 'one-to-many-mark');
  show("one-to-many with inserted mark: 'ii'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [9, 12, 11, 9, 12, 11],
  );
  assert.deepEqual(run.sourceRanges, [
    [[0, 1]],
    [[0, 1]],
    [[0, 1]],
    [[1, 2]],
    [[1, 2]],
    [[1, 2]],
  ]);
}

// --- Ligature with a skipped mark ----------------------------------------------
// The synthetic font ligates 'a' + 'b' with an IgnoreMarks lookup flag, so the
// combining acute between them survives with its own source range.
{
  let run = assertConsistentWithLayout(synth, ['áb'], 'ligature-skip-mark');
  show("ligature skipping a mark: 'a\\u0301b'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [4, 3],
  );
  assert.deepEqual(run.sourceRanges, [
    [
      [0, 1],
      [2, 3],
    ],
    [[1, 2]],
  ]);

  // Without the mark, the two adjacent ranges merge into one.
  run = assertConsistentWithLayout(synth, ['ab'], 'ligature');
  show("plain ligature: 'ab'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [4],
  );
  assert.deepEqual(run.sourceRanges, [[[0, 2]]]);
}

// --- Deletion -------------------------------------------------------------------
// lookup-continuation.ttf deletes 'c', and deletes 'h' before another 'h'.
{
  let run = assertConsistentWithLayout(continuation, ['ccb'], 'deletion');
  show("deletion: 'ccb'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [2],
  );
  assert.deepEqual(run.sourceRanges, [[[2, 3]]]);

  run = assertConsistentWithLayout(
    continuation,
    ['hhhh'],
    'deletion-contextual',
  );
  show("contextual deletion: 'hhhh'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [10, 10],
  );
  assert.deepEqual(run.sourceRanges, [[[1, 2]], [[3, 4]]]);
}

// --- Supplementary plane characters ----------------------------------------------
{
  let run = assertConsistentWithLayout(
    emoji,
    ['\u{1F600}', [], 'latn'],
    'supplementary-plane',
  );
  show("supplementary plane: '\\u{1F600}'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [57],
  );
  // A surrogate pair occupies two UTF-16 code units.
  assert.deepEqual(run.sourceRanges, [[[0, 2]]]);
}

// --- Variation sequences -----------------------------------------------------------
{
  let run = assertConsistentWithLayout(
    cmap14,
    ['\u{82a6}\u{E0101}', [], 'latn'],
    'variation-sequence',
  );
  show("variation sequence: '\\u{82a6}\\u{E0101}'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [2],
  );
  // The variation selector and its base character share one cmap mapping.
  assert.deepEqual(run.sourceRanges, [[[0, 3]]]);

  run = cmap14.layoutWithSource('\u{82a6}', [], 'latn');
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [1],
  );
  assert.deepEqual(run.sourceRanges, [[[0, 1]]]);
}

// --- Arabic: mark reordering and RTL reversal ----------------------------------------
{
  let run = assertConsistentWithLayout(
    amiri,
    ['١٢٣', 'arab', 'ARA ', 'rtl'],
    'arabic-rtl',
  );
  show("arabic RTL: '١٢٣'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [448, 447, 446],
  );
  assert.deepEqual(run.sourceRanges, [[[2, 3]], [[1, 2]], [[0, 1]]]);

  // The hamza (U+0654) is reordered before the fatha and composes with the
  // alef; the composed glyph keeps two disjoint source ranges rather than a
  // single [0, 3] span. Output is then reversed for RTL.
  run = assertConsistentWithLayout(amiri, ['أَ'], 'arabic-marks');
  show("arabic mark reordering: '\\u{0627}\\u{064E}\\u{0654}'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [5977, 384],
  );
  assert.deepEqual(run.sourceRanges, [
    [[1, 2]],
    [
      [0, 1],
      [2, 3],
    ],
  ]);
}

// --- Default ignorables --------------------------------------------------------------
{
  let run = assertConsistentWithLayout(fira, ['a­b'], 'default-ignorable');
  show("soft hyphen hidden as zero-advance space: 'a\\u00ADb'", run);
  assert.deepEqual(
    run.glyphs.map((g) => g.id),
    [340, fira.glyphForCodePoint(0x20).id, 377],
  );
  assert.equal(run.positions[1].xAdvance, 0);
  // The hidden soft hyphen keeps its own source range.
  assert.deepEqual(run.sourceRanges, [[[0, 1]], [[1, 2]], [[2, 3]]]);
}

// --- Empty string -----------------------------------------------------------------------
{
  let run = continuation.layoutWithSource('');
  assert.deepEqual(run.glyphs, []);
  assert.deepEqual(run.positions, []);
  assert.deepEqual(run.sourceRanges, []);
}

// --- Rejected inputs ----------------------------------------------------------------------
{
  assert.throws(
    () => continuation.layoutWithSource([continuation.glyphForCodePoint(97)]),
    /requires a string/,
    'glyph arrays must be rejected',
  );
  assert.throws(
    () => continuation.layoutWithSource('a\uD800'),
    /lone surrogates/,
    'trailing lone high surrogate must be rejected',
  );
  assert.throws(
    () => continuation.layoutWithSource('\uDC00ab'),
    /lone surrogates/,
    'lone low surrogate must be rejected',
  );
  assert.throws(
    () => morx.layoutWithSource('abc'),
    /morx/,
    'AAT (morx) fonts must be rejected',
  );
  assert.throws(
    () => devanagari.layoutWithSource('क'),
    /script/,
    'auto-detected unsupported script must be rejected',
  );
  assert.throws(
    () => continuation.layoutWithSource('aa', [], 'thai'),
    /script/,
    'explicitly passed unsupported script must be rejected',
  );
}

// --- Result isolation ------------------------------------------------------------------------
{
  // Mutating a returned sourceRanges array must not affect later runs.
  let first = continuation.layoutWithSource('aa');
  first.sourceRanges[0][0] = 99;
  first.sourceRanges.length = 0;

  let second = continuation.layoutWithSource('aa');
  assert.deepEqual(second.sourceRanges, [
    [[0, 1]],
    [[0, 1]],
    [[1, 2]],
    [[1, 2]],
  ]);

  // Source ranges live on the run, never on the shared cached Glyph objects.
  assert.equal(second.glyphs[0].sourceRanges, undefined);

  // Interleaving different strings on the same font must not mix up sources.
  let a = continuation.layoutWithSource('aa');
  let b = continuation.layoutWithSource('ccb');
  let c = continuation.layoutWithSource('aa');
  assert.deepEqual(a.sourceRanges, c.sourceRanges);
  assert.deepEqual(b.sourceRanges, [[[2, 3]]]);
}

console.log('\nall source range checks passed');
