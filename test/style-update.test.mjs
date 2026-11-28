import assert from 'node:assert/strict';
import { compile, decompile, updateStyles } from '../src/index.js';
import { checked } from '../task_support/check.mjs';

const TEXT = `[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
Style: Alt,Arial,30,&H000000FF,20,20,20
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B{\\rAlt\\fs+2}C
`;

const COLOR_TEXT = `[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\1c&H112233&\\1a&H80&}X{\\c}Y{\\r}Z
`;

const RESET_TEXT = `[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
Style: Alt,Arial,30,&H000000FF,20,20,20
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\rAlt}X{\\r}Y{\\rNope}Z
`;

const fragmentsOf = (doc, dia = 0) => doc.dialogues[dia].slices.flatMap((s) => s.fragments);

checked('defaults', async ({ check }) => {
  // without the option the compiled structure stays exactly as before
  const plain = compile(TEXT);
  check('editableStyles' in plain, false);
  check(plain.dialogues[0].slices[0].fragments.map((f) => f.tag.fs), [undefined, 20]);
  check(plain.dialogues[0].slices[1].fragments[0].tag.fs, 24);
  check(plain.dialogues[0].slices[0].fragments.some((f) => 'origin' in f), false);
  check('origin' in plain.dialogues[0], false);
  assert.throws(() => updateStyles(plain, { Default: { Fontsize: 40 } }), /editableStyles/);

  // with the option every fragment exposes effective values plus an origin
  const doc = compile(TEXT, { editableStyles: true });
  check(doc.editableStyles, true);
  check(fragmentsOf(doc).map((f) => f.tag.fs), [20, 20, 36]);
  check(fragmentsOf(doc).map((f) => f.tag.c1), ['FFFFFF', 'FFFFFF', '0000FF']);
  check(fragmentsOf(doc).map((f) => f.tag.a1), ['00', '00', '00']);
  check(fragmentsOf(doc)[0].origin, {
    fs: { type: 'inherit' },
    c1: { type: 'inherit' },
    a1: { type: 'inherit' },
    projected: { fs: 20, c1: 'FFFFFF', a1: '00' },
  });
  check(fragmentsOf(doc)[2].origin.fs, { type: 'relative', factor: 1.2, base: { type: 'inherit' } });
  check(doc.dialogues[0].origin, {
    margin: { left: 10, right: null, vertical: null },
    projectedMargin: { left: 10, right: 10, vertical: 10 },
  });

  // bad patches are rejected before anything is mutated
  const snapshot = JSON.stringify(doc);
  const badChanges = { Default: { Fontsize: 40 }, Ghost: { Fontsize: 1 } };
  const changesSnapshot = JSON.stringify(badChanges);
  assert.throws(() => updateStyles(doc, badChanges), /unknown style "Ghost"/);
  assert.throws(() => updateStyles(doc, { Default: { Fontname: 'Times' } }), /cannot update field "Fontname"/);
  assert.throws(() => updateStyles(doc, { Default: { Fontsize: 0 } }), /positive finite/);
  assert.throws(() => updateStyles(doc, { Default: { Fontsize: -2 } }), /positive finite/);
  assert.throws(() => updateStyles(doc, { Default: { Fontsize: NaN } }), /positive finite/);
  assert.throws(() => updateStyles(doc, { Default: { Fontsize: Infinity } }), /positive finite/);
  assert.throws(() => updateStyles(doc, { Default: { MarginL: -1 } }), /non-negative integer/);
  assert.throws(() => updateStyles(doc, { Default: { MarginR: 1.5 } }), /non-negative integer/);
  assert.throws(() => updateStyles(doc, { Default: { MarginV: '10' } }), /non-negative integer/);
  assert.throws(() => updateStyles(doc, { Default: { PrimaryColour: 'red' } }), /&HAABBGGRR/);
  assert.throws(() => updateStyles(doc, { Default: { PrimaryColour: '&H1234567' } }), /&HAABBGGRR/);
  assert.throws(() => updateStyles(doc, { Default: { PrimaryColour: '&H00112233FF' } }), /&HAABBGGRR/);
  check(JSON.stringify(doc), snapshot);
  check(JSON.stringify(badChanges), changesSnapshot);

  // valid colour spellings, with and without the trailing &
  const recolored = updateStyles(doc, { Default: { PrimaryColour: '&H7F445566' } });
  check(recolored.styles.Default.tag.c1, '445566');
  check(recolored.styles.Default.tag.a1, '7F');
  const recolored2 = updateStyles(doc, { Default: { PrimaryColour: '&H7F445566&' } });
  check(recolored2.styles.Default.style.PrimaryColour, '&H7F445566&');
});

checked('origins', async ({ check }) => {
  const doc = compile(TEXT, { editableStyles: true });

  // same value, different sources: A inherits Default (20), B pins \fs20
  const updated = updateStyles(doc, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragmentsOf(updated).map((f) => f.tag.fs), [40, 20, 60]);
  check(fragmentsOf(updated)[0].origin.fs, { type: 'inherit' });
  check(fragmentsOf(updated)[1].origin.fs, { type: 'value', value: 20 });

  // named reset switches the base style, bare and unknown resets fall
  // back to the dialogue style
  const resets = compile(RESET_TEXT, { editableStyles: true });
  check(resets.dialogues[0].slices.map((s) => s.style), ['Default', 'Alt', 'Default', 'Default']);
  check(fragmentsOf(resets).map((f) => f.tag.fs), [30, 20, 20]);
  const resetsUpdated = updateStyles(resets, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragmentsOf(resetsUpdated).map((f) => f.tag.fs), [50, 40, 40]);

  // relative font sizes are recomputed from the saved dependency on every
  // update, never multiplied on top of the previous result
  const step1 = updateStyles(doc, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragmentsOf(step1)[2].tag.fs, 60);
  const step2 = updateStyles(step1, { Alt: { Fontsize: 25 } });
  check(fragmentsOf(step2)[2].tag.fs, 30);
  check(fragmentsOf(step2)[2].origin.fs, { type: 'relative', factor: 1.2, base: { type: 'inherit' } });

  // `\c` only drops the RGB override and keeps the explicit alpha,
  // `\r` drops the alpha override as well
  const colors = compile(COLOR_TEXT, { editableStyles: true });
  check(fragmentsOf(colors).map((f) => f.tag.c1), ['112233', 'FFFFFF', 'FFFFFF']);
  check(fragmentsOf(colors).map((f) => f.tag.a1), ['80', '80', '00']);
  const recolored = updateStyles(colors, { Default: { PrimaryColour: '&H7F445566&' } });
  check(fragmentsOf(recolored).map((f) => f.tag.c1), ['112233', '445566', '445566']);
  check(fragmentsOf(recolored).map((f) => f.tag.a1), ['80', '80', '7F']);
  check(decompile(recolored).includes('{\\c}Y'), true);
});

checked('live-edits', async ({ check }) => {
  // fragments copied through JSON, reordered and restyled keep working,
  // and copied origins follow the style of the slice they land in
  const doc = compile(TEXT, { editableStyles: true });
  const dia = doc.dialogues[0];
  const copied = JSON.parse(JSON.stringify(dia.slices[1].fragments[0]));
  copied.text = 'C2';
  dia.slices[0].fragments.push(copied);
  const [a, b, c2] = dia.slices[0].fragments;
  dia.slices[0].fragments = [b, c2, a];
  dia.slices[1].style = 'Default';
  dia.slices[0].fragments[2].text = 'A-edited';
  dia.start = 2;
  dia.end = 5;
  const updated = updateStyles(doc, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  const uf = updated.dialogues[0].slices[0].fragments;
  check(uf.map((f) => f.text), ['B', 'C2', 'A-edited']);
  check(uf.map((f) => f.tag.fs), [20, 48, 40]);
  check(updated.dialogues[0].slices[1].fragments[0].tag.fs, 48);
  check([updated.dialogues[0].start, updated.dialogues[0].end], [2, 5]);
  // the input document was not mutated by the update
  check(doc.dialogues[0].slices[0].fragments[0].tag.fs, 20);

  // direct tag edits: changed fields become absolute overrides, removed
  // fields fall back to inheriting, new fragments are picked up too
  const doc2 = compile(TEXT, { editableStyles: true });
  const dia2 = doc2.dialogues[0];
  const [fa, fb] = dia2.slices[0].fragments;
  fa.tag.fs = 99;
  delete fb.tag.fs;
  dia2.slices[0].fragments.push({ tag: { fs: 33 }, text: 'N', drawing: null });
  dia2.slices[0].fragments.push({ tag: {}, text: 'M', drawing: null });
  const fc = dia2.slices[1].fragments[0];
  delete fc.tag.c1;
  fc.tag.a1 = '55';
  dia2.margin.left = 0;
  delete dia2.margin.right;
  dia2.margin.vertical = 30;
  const updated2 = updateStyles(doc2, {
    Default: { Fontsize: 40, MarginL: 15, MarginR: 16, MarginV: 17 },
    Alt: { PrimaryColour: '&H11223344' },
  });
  check(updated2.dialogues[0].slices[0].fragments.map((f) => f.tag.fs), [99, 40, 33, 40]);
  check(updated2.dialogues[0].margin, { left: 15, right: 16, vertical: 30 });
  check(updated2.dialogues[0].slices[1].fragments[0].tag.c1, '223344');
  check(updated2.dialogues[0].slices[1].fragments[0].tag.a1, '55');
});

checked('persistence', async ({ check }) => {
  const doc = compile(TEXT, { editableStyles: true });
  const branch1 = updateStyles(doc, {
    Default: { Fontsize: 40, PrimaryColour: '&H00445566', MarginL: 15, MarginR: 15, MarginV: 15 },
    Alt: { Fontsize: 50 },
  });
  check(fragmentsOf(branch1).map((f) => f.tag.fs), [40, 20, 60]);
  check(fragmentsOf(branch1).map((f) => f.tag.c1), ['445566', '445566', '0000FF']);
  // the explicit margin that equals the old style margin does not move
  check(branch1.dialogues[0].margin, { left: 10, right: 15, vertical: 15 });

  // decompile emits plain ASS that keeps relative font sizes, empty color
  // resets and explicit equal-value margins without private annotations
  const saved = decompile(branch1);
  check(saved, `[Script Info]
PlayResX: 640
PlayResY: 360
Collisions: Normal

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,40,&H00445566,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,15,15,15,1
Style: Alt,Arial,50,&H000000FF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0000,0000,,A{\\fs20}B{\\rAlt\\fs+2}C
`);

  // saving and reloading continues exactly like the unsaved branch
  const reloaded = compile(saved, { editableStyles: true });
  check(reloaded.dialogues[0].slices[1].fragments[0].origin.fs, {
    type: 'relative',
    factor: 1.2,
    base: { type: 'inherit' },
  });
  check(reloaded.dialogues[0].origin.margin, { left: 10, right: null, vertical: null });
  const continued = updateStyles(reloaded, { Alt: { Fontsize: 25 } });
  const direct = updateStyles(branch1, { Alt: { Fontsize: 25 } });
  check(fragmentsOf(continued).map((f) => f.tag.fs), [40, 20, 30]);
  check(continued.dialogues[0].margin, { left: 10, right: 15, vertical: 15 });
  check(decompile(continued), decompile(direct));

  // decompile does not mutate its input
  const snapshot = JSON.stringify(branch1);
  decompile(branch1);
  check(JSON.stringify(branch1), snapshot);
});

checked('compatibility', async ({ check }) => {
  // production compile and decompile output are unchanged
  const plain = compile(TEXT);
  check(decompile(plain), `[Script Info]
PlayResX: 640
PlayResY: 360
Collisions: Normal

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1
Style: Alt,Arial,30,&H000000FF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0000,0000,0000,,A{\\fs20}B{\\rAlt\\fs24}C
`);

  // \t animations are rejected only when the option is on
  const ttext = TEXT.replace('A{\\fs20}B{\\rAlt\\fs+2}C', '{\\t(\\fs30)}A');
  assert.throws(() => compile(ttext, { editableStyles: true }), /\\t \(transform\) tags are not supported/);
  check(compile(ttext).dialogues[0].slices[0].fragments[0].tag.t, [
    { t1: 0, t2: 2000, accel: 1, tag: { fs: 30 } },
  ]);

  // updateStyles returns a fully independent document
  const doc = compile(TEXT, { editableStyles: true });
  const snapshot = JSON.stringify(doc);
  const updated = updateStyles(doc, { Default: { Fontsize: 40 } });
  check(JSON.stringify(doc), snapshot);
  updated.dialogues[0].slices[0].fragments[0].tag.fs = 12345;
  updated.styles.Default.tag.fs = 12345;
  check(JSON.stringify(doc), snapshot);
  check(fragmentsOf(updated).map((f) => f.tag.fs), [12345, 20, 36]);
});
