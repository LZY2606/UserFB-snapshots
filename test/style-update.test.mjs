import assert from 'node:assert/strict';
import { compile, decompile, updateStyles } from '../src/index.js';
import { checked } from '../task_support/check.mjs';

const clone = (value) => JSON.parse(JSON.stringify(value));

const head = `[Script Info]
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
Style: Alt,Arial,30,&H000000FF,20,20,20

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
`;

function script(lines) {
  return head + lines.join('\n') + '\n';
}

function fragments(doc, dialogueIndex = 0) {
  return doc.dialogues[dialogueIndex].slices
    .flatMap((slice) => slice.fragments.map((fragment) => ({
      text: fragment.text,
      fs: fragment.tag.fs,
      c1: fragment.tag.c1,
      a1: fragment.tag.a1,
    })));
}

checked('defaults', ({ check }) => {
  const plain = compile(script(['Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B']));
  check(Object.prototype.hasOwnProperty.call(plain, 'editableStyles'), false);
  check(plain.dialogues[0].marginMeta, undefined);
  check(plain.dialogues[0].slices[0].fragments.map((fragment) => fragment.meta), [undefined, undefined]);
  // Without the option compile output is byte-for-byte what it always was.
  const baseline = {
    tagA: plain.dialogues[0].slices[0].fragments[0].tag,
    tagB: plain.dialogues[0].slices[0].fragments[1].tag,
    margin: plain.dialogues[0].margin,
  };
  check(baseline.tagA, {});
  check(baseline.tagB, { fs: 20 });
  check(baseline.margin, { left: 10, right: 10, vertical: 10 });
  check(decompile(plain), decompile(compile(script(['Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B']))));

  // A document compiled without the option cannot be edited.
  assert.throws(() => updateStyles(plain, {}), /editableStyles/);
  assert.throws(() => updateStyles(undefined, {}), /editableStyles/);
  assert.throws(() => updateStyles({}, { Default: { Fontsize: 30 } }), /editableStyles/);

  // \t animations are rejected when the option is on.
  assert.throws(
    () => compile(script(['Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\t(0,100,\\fs30)}A']), { editableStyles: true }),
    /\\t animation/,
  );

  // Editable output still keeps every compiled field.
  const doc = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\pos(10,20)\\fad(100,200)}A{\\b1}B{\\p1}m 0 0 l 1 1',
    'Dialogue: 0,0:00:04.00,0:00:06.00,Alt,,0,0,0,,{\\k20}K{\\kf30}L',
  ]), { editableStyles: true });
  check(doc.editableStyles, true);
  check(doc.dialogues[0].pos, { x: 10, y: 20 });
  check(doc.dialogues[0].fade, { type: 'fad', t1: 100, t2: 200 });
  check(doc.dialogues[0].slices[0].fragments[1].tag.b, 1);
  check(doc.dialogues[1].slices[0].fragments.map((fragment) => fragment.tag.kf), [undefined, 30]);
});

checked('origins', ({ check }) => {
  const doc = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B{\\rAlt\\fs+2}C',
  ]), { editableStyles: true });

  // Every text fragment projects the currently effective editable values.
  check(fragments(doc), [
    { text: 'A', fs: 20, c1: 'FFFFFF', a1: '00' },
    { text: 'B', fs: 20, c1: 'FFFFFF', a1: '00' },
    { text: 'C', fs: 36, c1: '0000FF', a1: '00' },
  ]);
  check(doc.dialogues[0].slices.map((slice) => slice.style), ['Default', 'Alt']);

  // Sources survive a JSON copy.
  const copied = clone(doc);
  check(fragments(copied), fragments(doc));
  check(copied.dialogues[0].slices[1].fragments[0].meta.fs.origin, 'relative');

  // Font size: inherited vs explicit vs relative, with the same numeric value.
  check(doc.dialogues[0].slices[0].fragments[0].meta.fs, { origin: 'style' });
  check(doc.dialogues[0].slices[0].fragments[1].meta.fs, { origin: 'explicit', value: 20 });
  check(doc.dialogues[0].slices[1].fragments[0].meta.fs, {
    origin: 'relative',
    factor: 1.2,
    base: { origin: 'style' },
  });

  // Primary colour and alpha overrides are tracked independently.
  const colors = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\1c&H00FF00&}A{\\1a&H80&}B{\\c}C{\\r}D',
  ]), { editableStyles: true });
  check(fragments(colors), [
    { text: 'A', fs: 20, c1: '00FF00', a1: '00' },
    { text: 'B', fs: 20, c1: '00FF00', a1: '80' },
    { text: 'C', fs: 20, c1: 'FFFFFF', a1: '80' },
    { text: 'D', fs: 20, c1: 'FFFFFF', a1: '00' },
  ]);
  check(colors.dialogues[0].slices[0].fragments[0].meta.c1, { origin: 'explicit', value: '00FF00', kind: 'color' });
  check(colors.dialogues[0].slices[0].fragments[1].meta.a1, { origin: 'explicit', value: '80', kind: 'alpha' });
  check(colors.dialogues[0].slices[0].fragments[2].meta.c1, { origin: 'style' });
  check(colors.dialogues[0].slices[0].fragments[2].meta.a1, { origin: 'explicit', value: '80', kind: 'alpha' });
  check(colors.dialogues[0].slices[1].fragments[0].meta.a1, { origin: 'style' });

  // Dialog margins: zero inherits; non-zero equal to the style is explicit.
  const margins = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,10,0,,X',
  ]), { editableStyles: true });
  check(margins.dialogues[0].margin, { left: 10, right: 10, vertical: 10 });
  check(margins.dialogues[0].marginMeta, { left: true, right: true, vertical: false, style: 'Default' });
});

checked('live-edits', ({ check }) => {
  const text = script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B{\\rAlt\\fs+2}C',
  ]);
  const doc = compile(text, { editableStyles: true });
  const first = updateStyles(doc, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragments(first), [
    { text: 'A', fs: 40, c1: 'FFFFFF', a1: '00' },
    { text: 'B', fs: 20, c1: 'FFFFFF', a1: '00' },
    { text: 'C', fs: 60, c1: '0000FF', a1: '00' },
  ]);
  // Each update recomputes from the source, so relative sizes do not compound.
  const second = updateStyles(first, { Alt: { Fontsize: 25 } });
  check(fragments(second).map((fragment) => fragment.fs), [40, 20, 30]);
  const third = updateStyles(second, { Alt: { Fontsize: 50 } });
  check(fragments(third).map((fragment) => fragment.fs), [40, 20, 60]);

  // Named and bare resets.
  const resets = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\rAlt\\fs+2}A{\\r}B{\\rNope}C',
  ]), { editableStyles: true });
  check(fragments(resets), [
    { text: 'A', fs: 36, c1: '0000FF', a1: '00' },
    { text: 'B', fs: 20, c1: 'FFFFFF', a1: '00' },
    { text: 'C', fs: 20, c1: 'FFFFFF', a1: '00' },
  ]);
  const resetUpdate = updateStyles(resets, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragments(resetUpdate).map((fragment) => fragment.fs), [60, 40, 40]);

  // \fs30 cuts the font-size dependency on the style.
  const cut = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\fs30}A{\\fs+2}B{\\fs-2}C',
  ]), { editableStyles: true });
  const cutUpdate = updateStyles(cut, { Default: { Fontsize: 100 } });
  check(fragments(cutUpdate).map((fragment) => Math.round(fragment.fs * 10) / 10), [30, 36, 28.8]);

  // Colour-only reset keeps an explicit alpha; \r clears it too.
  const colors = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\1c&H00FF00&\\1a&H80&}A{\\c}B{\\r}C',
  ]), { editableStyles: true });
  const colorUpdate = updateStyles(colors, { Default: { PrimaryColour: '&HFF112233&' } });
  check(fragments(colorUpdate), [
    { text: 'A', fs: 20, c1: '00FF00', a1: '80' },
    { text: 'B', fs: 20, c1: '112233', a1: '80' },
    { text: 'C', fs: 20, c1: '112233', a1: 'FF' },
  ]);
  // The same numeric colour with a different source stays explicitly overridden.
  const same = updateStyles(colors, { Default: { PrimaryColour: '&HFF00FF00&' } });
  check(fragments(same).map((fragment) => fragment.c1), ['00FF00', '00FF00', '00FF00']);
  check(same.dialogues[0].slices[0].fragments[0].meta.c1, { origin: 'explicit', value: '00FF00', kind: 'color' });
  check(same.dialogues[0].slices[0].fragments[1].meta.c1, { origin: 'style' });

  // Explicit margin equal to the old style value stays explicit.
  const margins = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,10,0,,X',
  ]), { editableStyles: true });
  const marginUpdate = updateStyles(margins, { Default: { MarginL: 10, MarginR: 15, MarginV: 15 } });
  check(marginUpdate.dialogues[0].margin, { left: 10, right: 10, vertical: 15 });
  check(marginUpdate.dialogues[0].marginMeta, { left: true, right: true, vertical: false, style: 'Default' });
  const marginAgain = updateStyles(marginUpdate, { Default: { MarginR: 25 } });
  check(marginAgain.dialogues[0].margin, { left: 10, right: 10, vertical: 15 });
});

checked('persistence', ({ check }) => {
  const text = script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,{\\1a&H80&}A{\\1c&H00FF00&}B{\\c}C{\\rAlt\\fs+2}D{E',
    'Dialogue: 0,0:00:04.00,0:00:06.00,Alt,,0,15,0,,X{\\fs30}Y',
  ]);
  const doc = compile(text, { editableStyles: true });
  const edited = updateStyles(doc, {
    Default: { Fontsize: 40, PrimaryColour: '&H80112233&', MarginL: 10 },
    Alt: { Fontsize: 50, MarginR: 15 },
  });

  const inputSnapshot = JSON.stringify(edited);
  const output = decompile(edited);
  check(JSON.stringify(edited), inputSnapshot);
  check(output, decompile(edited));

  // Relative dependencies, empty colour resets and explicit equal margins are
  // preserved as ordinary ASS, without private annotations.
  check(output.includes('\\fs+2'), true);
  check(output.includes('\\c'), true);
  check(output.includes('\\1c&H00FF00&'), true);
  check(output.includes('\\1a&H80&'), true);
  check(output.includes(',10,0000,0000,,'), true);
  check(output.includes(',0000,15,0000,,'), true);
  check(/\\\\|\bmeta\b|__editable/.test(output), false);

  // Saving and reading back must continue exactly like the unsaved branch.
  const reloaded = compile(output, { editableStyles: true });
  const changes = {
    Default: { PrimaryColour: '&H00010203&' },
    Alt: { Fontsize: 25, MarginR: 15 },
  };
  const fromMemory = updateStyles(edited, changes);
  const fromDisk = updateStyles(reloaded, changes);
  check(fromDisk.dialogues, fromMemory.dialogues);
  Object.keys(fromMemory.styles).forEach((name) => {
    check(fromDisk.styles[name].tag, fromMemory.styles[name].tag);
    check(fromDisk.styles[name].style.Fontsize, fromMemory.styles[name].style.Fontsize);
    check(
      [fromDisk.styles[name].style.MarginL, fromDisk.styles[name].style.MarginR,
        fromDisk.styles[name].style.MarginV],
      [fromMemory.styles[name].style.MarginL, fromMemory.styles[name].style.MarginR,
        fromMemory.styles[name].style.MarginV],
    );
  });
  check(fragments(fromDisk, 0), fragments(fromMemory, 0));
  check(fragments(fromDisk, 1), fragments(fromMemory, 1));
  check(fromDisk.dialogues[0].margin, { left: 10, right: 10, vertical: 10 });
  check(fromDisk.dialogues[1].margin, { left: 20, right: 15, vertical: 20 });

  // Decompiling does not touch its input, not even its dialogue order.
  const before = JSON.stringify(reloaded);
  decompile(reloaded);
  check(JSON.stringify(reloaded), before);
});

checked('compatibility', ({ check }) => {
  // Current-document edits are respected: text, timing, fragment order/deletion
  // and copied fragments are all preserved.
  const doc = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\fs+2}A',
  ]), { editableStyles: true });

  const edited = clone(doc);
  const copied = clone(edited.dialogues[0].slices[0].fragments[0]);
  edited.dialogues[0].slices.unshift({ style: 'Alt', fragments: [copied] });
  edited.dialogues[0].start = 2;
  edited.dialogues[0].margin.vertical = 7;
  const moved = updateStyles(edited, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(moved.dialogues[0].start, 2);
  check(moved.dialogues[0].end, 3);
  check(moved.dialogues[0].margin.vertical, 7);
  check(moved.dialogues[0].slices.map((slice) => slice.style), ['Alt', 'Default']);
  check(fragments(moved), [
    { text: 'A', fs: 60, c1: '0000FF', a1: '00' },
    { text: 'A', fs: 48, c1: 'FFFFFF', a1: '00' },
  ]);
  check(doc.dialogues[0].start, 1);
  check(fragments(doc).map((fragment) => fragment.fs), [24]);

  // Direct tag edits: same projection keeps the source, a new value is an
  // absolute override, deletion falls back to inheritance.
  const live = clone(doc);
  const [a] = live.dialogues[0].slices[0].fragments;
  check(a.text, 'A');
  // add two fragments to exercise same/edited/deleted
  live.dialogues[0].slices[0].fragments = [
    { tag: { fs: 24 }, text: 'same', drawing: null, meta: clone(a.meta) },
    { tag: { fs: 31 }, text: 'changed', drawing: null, meta: clone(a.meta) },
    { tag: {}, text: 'deleted', drawing: null, meta: clone(a.meta) },
  ];
  const direct = updateStyles(live, { Default: { Fontsize: 40 } });
  check(fragments(direct), [
    { text: 'same', fs: 48, c1: 'FFFFFF', a1: '00' },
    { text: 'changed', fs: 31, c1: 'FFFFFF', a1: '00' },
    { text: 'deleted', fs: 40, c1: 'FFFFFF', a1: '00' },
  ]);
  check(direct.dialogues[0].slices[0].fragments[1].meta.fs, { origin: 'explicit', value: 31 });
  check(direct.dialogues[0].slices[0].fragments[2].meta.fs, { origin: 'style' });

  // New fragments without a source: present tags are absolute overrides,
  // missing ones inherit from the slice style.
  const withNew = clone(doc);
  withNew.dialogues[0].slices.push({
    style: 'Alt',
    fragments: [
      { tag: { fs: 12 }, text: 'N1', drawing: null },
      { tag: { c1: 'ABCDEF' }, text: 'N2', drawing: null },
      { tag: {}, text: 'N3', drawing: null },
    ],
  });
  const fresh = updateStyles(withNew, { Alt: { Fontsize: 50, PrimaryColour: '&HFF00FF00&' } });
  check(fragments(fresh).slice(1), [
    { text: 'N1', fs: 12, c1: '00FF00', a1: 'FF' },
    { text: 'N2', fs: 50, c1: 'ABCDEF', a1: 'FF' },
    { text: 'N3', fs: 50, c1: '00FF00', a1: 'FF' },
  ]);
  check(fresh.dialogues[0].slices[1].fragments.map((fragment) => fragment.meta.fs), [
    { origin: 'explicit', value: 12 },
    { origin: 'style' },
    { origin: 'style' },
  ]);

  // Retargeting a slice style re-evaluates the sources it contains.
  const retarget = clone(doc);
  retarget.dialogues[0].slices[0].style = 'Alt';
  const retargeted = updateStyles(retarget, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
  check(fragments(retargeted).map((fragment) => fragment.fs), [60]);

  // Direct margin edits: zero / deletion re-inherit, non-zero is explicit.
  const marginDoc = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,X',
    'Dialogue: 0,0:00:04.00,0:00:06.00,Default,,10,10,10,,Y',
  ]), { editableStyles: true });
  marginDoc.dialogues[0].margin.left = 0;
  marginDoc.dialogues[0].margin.right = 25;
  delete marginDoc.dialogues[0].margin.vertical;
  const marginDirect = updateStyles(marginDoc, { Default: { MarginL: 30, MarginR: 40, MarginV: 50 } });
  check(marginDirect.dialogues[0].margin, { left: 30, right: 25, vertical: 50 });
  check(marginDirect.dialogues[0].marginMeta, { left: false, right: true, vertical: false, style: 'Default' });
  check(marginDirect.dialogues[1].margin, { left: 10, right: 10, vertical: 10 });

  // Retargeting the dialogue style makes untouched margins follow the new
  // style, while an explicitly edited margin stays put.
  const styleRetarget = compile(script([
    'Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,X',
  ]), { editableStyles: true });
  styleRetarget.dialogues[0].style = 'Alt';
  styleRetarget.dialogues[0].margin.right = 60;
  const styleRetargeted = updateStyles(styleRetarget, { Alt: { MarginL: 33 } });
  check(styleRetargeted.dialogues[0].margin, { left: 33, right: 60, vertical: 20 });
  check(styleRetargeted.dialogues[0].marginMeta, { left: false, right: true, vertical: false, style: 'Alt' });

  // Results are complete independent copies.
  const snapshot = JSON.stringify(doc);
  const updated = updateStyles(doc, { Default: { Fontsize: 77 } });
  check(JSON.stringify(doc), snapshot);
  updated.styles.Default.tag.fs = 5;
  updated.dialogues[0].slices[0].fragments[0].text = 'mutated';
  check(doc.styles.Default.tag.fs, 20);
  check(doc.dialogues[0].slices[0].fragments[0].text, 'A');
  check(doc.dialogues[0].slices[0].fragments[0].meta !== updated.dialogues[0].slices[0].fragments[0].meta, true);

  // Bad patches throw and leave the input untouched.
  const beforeBad = JSON.stringify(doc);
  const bad = [
    { Missing: { Fontsize: 30 } },
    { Default: { Name: 'Nope' } },
    { Default: { Fontsize: 0 } },
    { Default: { Fontsize: -4 } },
    { Default: { Fontsize: Infinity } },
    { Default: { Fontsize: '20' } },
    { Default: { MarginL: -1 } },
    { Default: { MarginL: 1.5 } },
    { Default: { PrimaryColour: '&H123456' } },
    { Default: { PrimaryColour: '&HZZ000000' } },
    { Default: null },
  ];
  bad.forEach((changes) => assert.throws(() => updateStyles(doc, changes)));
  check(JSON.stringify(doc), beforeBad);
});
