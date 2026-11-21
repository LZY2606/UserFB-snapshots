'use strict';
// Regression tests for ASS-to-ASS cue copy/paste with style migration.
// Groups are executed through task_support/check.cjs so each one produces the
// TASK_GROUP_DONE marker the task runner verifies.
const { checked } = require('../task_support/check.cjs');
const { parseAss, serializeAss } = require('../.task-build/formats/ass.js');
const { copyCueClipboard, pasteCueClipboard, pasteCues } = require('../.task-build/edit-ops.js');

// --- hand-written ASS fixtures ---------------------------------------------------------
// Build a document from the pieces so every fixture stays readable and explicit.
function assDoc({ info = [], styleFormat, styles, eventFormat, events, trailing = [] }) {
  const lines = ["[Script Info]", "ScriptType: v4.00+", ...info, "", "[V4+ Styles]"];
  lines.push(`Format: ${styleFormat.join(", ")}`);
  for (const s of styles) lines.push(s);
  lines.push("", "[Events]", `Format: ${eventFormat.join(", ")}`);
  lines.push(...events);
  lines.push(...trailing);
  return lines.join("\n") + "\n";
}

const EV = ["Layer", "Start", "End", "Style", "Text"];
const ST = ["Name", "Fontname", "Fontsize", "PrimaryColour", "Bold"];

// Source: Default 72 bold, Title 60, Sign 40. One event referencing Title via a named
// reset, one via the Style field, plus text full of `\r` look-alikes that must not count.
const SOURCE = assDoc({
  info: ["PlayResX: 1920", "PlayResY: 1080"],
  styleFormat: ST,
  styles: [
    "Style: Default,Arial,72,&H00FFFFFF,0",
    "Style: Title,Georgia,60,&H0000FFFF,-1",
    "Style: Sign,Consolas,40,&H00FF0000,0",
    "Style: Unused,Impact,30,&H00FFFFFF,0",
  ],
  eventFormat: EV,
  events: [
    "Dialogue: 0,0:00:01.00,0:00:03.50,Default,{\\rTitle\\b1}Hello",
    "Dialogue: 0,0:00:04.00,0:00:06.00,Sign,Plain sign",
    "Comment: 0,0:00:07.00,0:00:08.00,Default,Disabled line",
  ],
});

// Target: same names as the source but different definitions, so a name-only paste would
// silently restyle. A custom target field and a different Format order/casing are present.
const TARGET = assDoc({
  info: ["PlayResX: 1280", "PlayResY: 720"],
  styleFormat: ["Name", "fontsize", "Fontname", "Bold", "Custom"],
  styles: [
    "Style: Default,24,Arial,0,T0",
    "Style: Title,48,Georgia,-1,T1",
    "Style: Sign,20,Consolas,0,T2",
  ],
  eventFormat: ["Layer", "Start", "End", "Style", "Actor", "Text"],
  events: ["Dialogue: 0,0:00:00.00,0:00:01.00,Default,,Target first line"],
  trailing: ["", "; trailing comment stays"],
});

function ids(doc, indices) {
  return indices.map((i) => doc.cues[i].id);
}

function styleDefs(doc) {
  return Object.fromEntries(doc.styles.map((s) => [s.name, s.fields]));
}

// --- closure --------------------------------------------------------------------------
checked('closure', ({ check }) => {
  const src = parseAss(SOURCE);
  const clip = copyCueClipboard(src, ids(src, [0, 1]));

  // Cues keep source order and only the referenced styles travel, in stylesheet order.
  check(clip.cues.map((c) => c.text), ["{\\rTitle\\b1}Hello", "Plain sign"]);
  check(clip.styles.map((s) => s.name), ["Default", "Title", "Sign"]);
  check(clip.styles.find((s) => s.name === "Title").fields.Fontsize, "60");

  // A comment event copies too; its kind and fields survive.
  check(clip.cues.map((c) => c.assKind), ["Dialogue", "Dialogue"]);
  const withComment = copyCueClipboard(src, ids(src, [2]));
  check(withComment.cues[0].assKind, "Comment");

  // `\r` look-alikes: literal text outside braces, a font name and a \t(...) argument.
  const tricky = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Default,Arial,72,&H00FFFFFF,0", "Style: Title,Georgia,60,&H0000FFFF,-1"],
      eventFormat: EV,
      events: [
        String.raw`Dialogue: 0,0:00:01.00,0:00:02.00,Default,literal \rTitle here`,
        String.raw`Dialogue: 0,0:00:02.00,0:00:03.00,Default,{\fnArial\r\t(0,500,\rTitle)}x`,
        String.raw`Dialogue: 0,0:00:03.00,0:00:04.00,Default,{\rTitle}A{\r}bare`,
      ],
    }),
  );
  check(copyCueClipboard(tricky, ids(tricky, [0])).styles.map((s) => s.name), ["Default"]);
  check(copyCueClipboard(tricky, ids(tricky, [1])).styles.map((s) => s.name), ["Default"]);
  check(copyCueClipboard(tricky, ids(tricky, [2])).styles.map((s) => s.name), ["Default", "Title"]);

  // Style names are case-sensitive and may contain spaces; the name keeps its inner spaces.
  check(copyCueClipboard(src, ids(src, [1])).styles.map((s) => s.name), ["Sign"]);
  const spaced = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        "Style: Default,Arial,72,&H00FFFFFF,0",
        "Style: Big Title,Georgia,80,&H0000FFFF,-1",
      ],
      eventFormat: EV,
      events: [String.raw`Dialogue: 0,0:00:01.00,0:00:02.00,Default,{\rBig Title\b1}x`],
    }),
  );
  const spacedClip = copyCueClipboard(spaced, ids(spaced, [0]));
  check(spacedClip.styles.map((s) => s.name), ["Default", "Big Title"]);
  // Lowercased name is a different style and must be reported as missing, not matched.
  const lower = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Default,Arial,72,&H00FFFFFF,0", "Style: Big Title,Georgia,80,&H0000FFFF,-1"],
      eventFormat: EV,
      events: [String.raw`Dialogue: 0,0:00:01.00,0:00:02.00,Default,{\rbig title}x`],
    }),
  );
  let lowerError = '';
  try {
    copyCueClipboard(lower, ids(lower, [0]));
  } catch (err) {
    lowerError = err.message;
  }
  check(/big title/.test(lowerError), true);

  // The snapshot is independent: editing the source after copying leaves it unchanged.
  const live = parseAss(SOURCE);
  const snapshot = copyCueClipboard(live, ids(live, [0]));
  live.styles.find((s) => s.name === "Title").fields.Fontsize = "999";
  live.cues[0].text = "changed";
  live.cues[0].assFields.Style = "Sign";
  check(snapshot.styles.find((s) => s.name === "Title").fields.Fontsize, "60");
  check(snapshot.cues[0].text, "{\\rTitle\\b1}Hello");
  check(snapshot.cues[0].assFields.Style, "Default");

  // A missing reference (Style field or named reset) throws and leaves the source untouched.
  const broken = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Default,Arial,72,&H00FFFFFF,0"],
      eventFormat: EV,
      events: ["Dialogue: 0,0:00:01.00,0:00:02.00,Ghost,no style"],
    }),
  );
  const brokenBefore = JSON.stringify(broken);
  let threw = '';
  try {
    copyCueClipboard(broken, ids(broken, [0]));
  } catch (err) {
    threw = err.message;
  }
  check(/Ghost/.test(threw), true);
  check(JSON.stringify(broken), brokenBefore);

  const brokenReset = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Default,Arial,72,&H00FFFFFF,0"],
      eventFormat: EV,
      events: [String.raw`Dialogue: 0,0:00:01.00,0:00:02.00,Default,{\rNope}x`],
    }),
  );
  let threwReset = false;
  try {
    copyCueClipboard(brokenReset, ids(brokenReset, [0]));
  } catch {
    threwReset = true;
  }
  check(threwReset, true);
});

// --- collisions -----------------------------------------------------------------------
checked('collisions', ({ check }) => {
  const src = parseAss(SOURCE);
  const dst = parseAss(TARGET);
  const clip = copyCueClipboard(src, ids(src, [0, 1]));
  const dstBefore = JSON.stringify(dst);

  const out = pasteCueClipboard(dst, clip, 0);
  // Same name, different definition -> renamed with the (paste N) suffix.
  check(out.styleMap, {
    Default: 'Default (paste 2)',
    Title: 'Title (paste 2)',
    Sign: 'Sign (paste 2)',
  });
  check(out.newIds.length, 2);

  // The target's own styles and references are untouched.
  check(styleDefs(out.doc).Default, dst.styles.find((s) => s.name === 'Default').fields);
  check(out.doc.cues[0].assFields.Style, 'Default');
  check(out.doc.cues[0].text, 'Target first line');
  check(JSON.stringify(dst), dstBefore);

  // Pasted references point at the migrated definitions (source sizes, not target's).
  const pasted = out.doc.cues.slice(1, 3);
  check(pasted.map((c) => c.assFields.Style), ['Default (paste 2)', 'Sign (paste 2)']);
  check(pasted[0].text, '{\\rTitle (paste 2)\\b1}Hello');
  check(styleDefs(out.doc)['Title (paste 2)'].fontsize, '60');
  check(styleDefs(out.doc)['Default (paste 2)'].fontsize, '72');
  check(out.doc.cues.length, 3);

  // Pasting the same clipboard again reuses the (paste 2) definitions, adding no styles.
  const again = pasteCueClipboard(out.doc, clip, 0);
  check(again.doc.styles.length, out.doc.styles.length);
  check(again.styleMap, out.styleMap);

  // Identical definitions are reused instead of duplicated (same name or a candidate).
  const sameDefSrc = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Shared,Arial,72,&H00FFFFFF,0"],
      eventFormat: EV,
      events: ["Dialogue: 0,0:00:01.00,0:00:02.00,Shared,x"],
    }),
  );
  const sameDefDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ["Style: Shared,Arial,72,&H00FFFFFF,0"],
      eventFormat: EV,
      events: ["Dialogue: 0,0:00:00.00,0:00:01.00,Shared,y"],
    }),
  );
  const sameClip = copyCueClipboard(sameDefSrc, ids(sameDefSrc, [0]));
  const sameOut = pasteCueClipboard(sameDefDst, sameClip, 0);
  check(sameOut.styleMap, { Shared: 'Shared' });
  check(sameOut.doc.styles.length, 1);

  // A conflicting original is renamed; an existing identical (paste 2) is then reused.
  const reuseDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        "Style: Shared,Impact,10,&H00FFFFFF,0",
        "Style: Shared (paste 2),Arial,72,&H00FFFFFF,0",
      ],
      eventFormat: EV,
      events: ["Dialogue: 0,0:00:00.00,0:00:01.00,Shared,y"],
    }),
  );
  const reuseOut = pasteCueClipboard(reuseDst, sameClip, 0);
  check(reuseOut.styleMap, { Shared: 'Shared (paste 2)' });
  check(reuseOut.doc.styles.length, 2);

  // Global reservation: "A" renames to "A (paste 2)" and must not take "B (paste 2)",
  // which B itself needs as its collision name.
  const reservSrc = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        "Style: A,Arial,10,&H00FFFFFF,0",
        "Style: B,Arial,20,&H00FFFFFF,0",
      ],
      eventFormat: EV,
      events: [
        "Dialogue: 0,0:00:01.00,0:00:02.00,A,a",
        "Dialogue: 0,0:00:02.00,0:00:03.00,B,b",
      ],
    }),
  );
  const reservDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        "Style: A,Arial,99,&H00FFFFFF,0",
        "Style: B,Arial,98,&H00FFFFFF,0",
        "Style: B (paste 2),Arial,97,&H00FFFFFF,0",
      ],
      eventFormat: EV,
      events: ["Dialogue: 0,0:00:00.00,0:00:01.00,A,z"],
    }),
  );
  const reservClip = copyCueClipboard(reservSrc, ids(reservSrc, [1, 0])); // selected out of order
  const reservOut = pasteCueClipboard(reservDst, reservClip, 0);
  // Allocation follows the source stylesheet order, not the selection order.
  check(reservOut.styleMap, { A: 'A (paste 2)', B: 'B (paste 3)' });
  check(Object.values(reservOut.styleMap), ['A (paste 2)', 'B (paste 3)']);
  check(styleDefs(reservOut.doc)["B (paste 3)"].Fontsize, "20");
  check(styleDefs(reservOut.doc)["B (paste 2)"].Fontsize, "97");});

// --- formats --------------------------------------------------------------------------
checked('formats', ({ check }) => {
  const src = parseAss(SOURCE);
  const dst = parseAss(TARGET);
  const clip = copyCueClipboard(src, ids(src, [0]));
  const out = pasteCueClipboard(dst, clip, 0);

  // The target's Format text stays authoritative and reflects the appended fields.
  check(out.doc.assStyleFormat, ['Name', 'fontsize', 'Fontname', 'Bold', 'Custom', 'PrimaryColour']);
  check(out.doc.assScriptInfo.includes('Format: Name, fontsize, Fontname, Bold, Custom, PrimaryColour'), true);
  check(out.doc.assFormat, ['Layer', 'Start', 'End', 'Style', 'Actor', 'Text']);
  check(out.doc.assStylesTail.includes('Format: Layer, Start, End, Style, Actor, Text'), true);

  // Existing target fields keep their spelling and order; missing source fields follow in
  // the source Format's order (Fontsize, PrimaryColour, Bold -> PrimaryColour is missing).
  const readBack = parseAss(serializeAss(out.doc));
  const targetDefault = readBack.styles.find((s) => s.name === 'Default');
  check(Object.keys(targetDefault.fields), ['fontsize', 'Fontname', 'Bold', 'Custom', 'PrimaryColour']);
  check(targetDefault.fields.fontsize, '24');
  check(targetDefault.fields.PrimaryColour, ''); // missing in the target Format, now present and empty
  const migrated = readBack.styles.find((s) => s.name === 'Default (paste 2)');
  check(migrated.fields.fontsize, '72');
  check(migrated.fields.Fontname, 'Arial');
  check(migrated.fields.Bold, '0');
  check(migrated.fields.Custom, '');
  check(migrated.fields.PrimaryColour, '&H00FFFFFF');

  // Event fields: the target-only Actor and the source's fields all survive; Text stays last.
  check(out.doc.cues[1].assFields, {
    Layer: '0',
    Style: 'Default (paste 2)',
    Actor: '',
  });
  check(out.doc.assFormat[out.doc.assFormat.length - 1], 'Text');

  // A source-only event field is inserted before Text.
  const richSrc = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ['Style: Default,Arial,72,&H00FFFFFF,0'],
      eventFormat: ['Layer', 'Start', 'End', 'Style', 'Effect', 'Text'],
      events: ['Dialogue: 0,0:00:01.00,0:00:02.00,Default,fadein,styled text'],
    }),
  );
  const simpleDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: ['Style: Default,Arial,10,&H00FFFFFF,0'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:00.00,0:00:01.00,Default,plain'],
    }),
  );
  const richOut = pasteCueClipboard(simpleDst, copyCueClipboard(richSrc, ids(richSrc, [0])), 0);
  check(richOut.doc.assFormat, ['Layer', 'Start', 'End', 'Style', 'Effect', 'Text']);
  check(richOut.doc.assStylesTail.includes('Format: Layer, Start, End, Style, Effect, Text'), true);
  const richRead = parseAss(serializeAss(richOut.doc));
  check(richRead.cues[1].assFields.Effect, 'fadein');
  check(richRead.cues[1].text, 'styled text');

  // A custom Style field survives save -> parse and can be edited again.
  const customSrc = parseAss(
    assDoc({
      styleFormat: ['Name', 'Fontname', 'Fontsize', 'MyCustom'],
      styles: ['Style: Default,Arial,72,special'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:01.00,0:00:02.00,Default,x'],
    }),
  );
  const customDst = parseAss(
    assDoc({
      styleFormat: ['Name', 'Fontname', 'Fontsize'],
      styles: ['Style: Default,Arial,10'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:00.00,0:00:01.00,Default,y'],
    }),
  );
  const customOut = pasteCueClipboard(customDst, copyCueClipboard(customSrc, ids(customSrc, [0])), 0);
  const customRead = parseAss(serializeAss(customOut.doc));
  check(customRead.assStyleFormat, ['Name', 'Fontname', 'Fontsize', 'MyCustom']);
  const customStyle = customRead.styles.find((s) => s.name === 'Default (paste 2)');
  check(customStyle.fields.MyCustom, 'special');
  // Editing the reloaded custom field and saving again keeps it.
  customStyle.fields.MyCustom = 'edited';
  check(parseAss(serializeAss(customRead)).styles.find((s) => s.name === 'Default (paste 2)').fields.MyCustom, 'edited');

  // Style comparison: case-insensitive attribute names, missing = "", values verbatim.
  const cmpSrc = parseAss(
    assDoc({
      styleFormat: ['Name', 'FontName', 'FontSize'],
      styles: ['Style: Shared,Arial,72'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:01.00,0:00:02.00,Shared,x'],
    }),
  );
  const cmpDst = parseAss(
    assDoc({
      styleFormat: ['Name', 'fontsize', 'fontname', 'Shadow'],
      styles: ['Style: Shared,72,Arial,0'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:00.00,0:00:01.00,Shared,y'],
    }),
  );
  const cmpOut = pasteCueClipboard(cmpDst, copyCueClipboard(cmpSrc, ids(cmpSrc, [0])), 0);
  // Shadow is absent on the source, hence "" — not equal to the target's "0".
  check(cmpOut.styleMap, { Shared: 'Shared (paste 2)' });
  // Values are not normalised: "72" and "72.0" differ.
  const valueDst = parseAss(
    assDoc({
      styleFormat: ['Name', 'Fontname', 'Fontsize'],
      styles: ['Style: Shared,Arial,72.0'],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:00.00,0:00:01.00,Shared,y'],
    }),
  );
  const valueOut = pasteCueClipboard(valueDst, copyCueClipboard(cmpSrc, ids(cmpSrc, [0])), 0);
  check(valueOut.styleMap, { Shared: 'Shared (paste 2)' });
});

// --- persistence ----------------------------------------------------------------------
checked('persistence', ({ check }) => {
  const src = parseAss(SOURCE);
  const dst = parseAss(TARGET);
  const clip = copyCueClipboard(src, ids(src, [0, 1]));

  // The clipboard is JSON-serializable and reloads to the same thing.
  const reloaded = JSON.parse(JSON.stringify(clip));
  check(reloaded, clip);

  const out = pasteCueClipboard(dst, reloaded, 0);
  const text = serializeAss(out.doc);
  const read = parseAss(text);

  // Every migrated reference resolves to its planned definition after save + reopen.
  const fontSizeOf = (doc, name) => {
    const style = doc.styles.find((s) => s.name === name);
    const key = Object.keys(style.fields).find((k) => k.toLowerCase() === 'fontsize');
    return style.fields[key];
  };
  for (const [sourceName, targetName] of Object.entries(out.styleMap)) {
    check(fontSizeOf(read, targetName), fontSizeOf(clip, sourceName));
  }
  check(read.cues[1].assFields.Style, 'Default (paste 2)');
  check(read.cues[1].text, '{\\rTitle (paste 2)\\b1}Hello');
  check(read.cues[2].assFields.Style, 'Sign (paste 2)');

  // BOM, EOL, Script Info, comments and trailing content all survive.
  const bomDst = parseAss('﻿' + TARGET.replace(/\n/g, '\r\n'));
  check(bomDst.bom, true);
  check(bomDst.eol, '\r\n');
  const bomOut = pasteCueClipboard(bomDst, clip, 0);
  const bomText = serializeAss(bomOut.doc);
  check(bomText.charCodeAt(0), 0xfeff);
  check(bomText.includes('\r\n'), true);
  check(bomText.includes('; trailing comment stays'), true);
  check(bomText.includes('PlayResX: 1280'), true);
  const bomRead = parseAss(bomText);
  check(bomRead.cues.length, 3);
  check(bomRead.cues[1].assFields.Style, 'Default (paste 2)');

  // Style/event notesBefore are not carried from the source; the target's stay in place.
  const notedSrc = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [String.raw`; src style note\nStyle: Default,Arial,72,&H00FFFFFF,0`.replace(/\\n/g, '\n')],
      eventFormat: EV,
      events: [String.raw`; src event note\nDialogue: 0,0:00:01.00,0:00:02.00,Default,x`.replace(/\\n/g, '\n')],
    }),
  );
  const notedDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [String.raw`; dst style note\nStyle: Default,Arial,10,&H00FFFFFF,0`.replace(/\\n/g, '\n')],
      eventFormat: EV,
      events: [String.raw`; dst event note\nDialogue: 0,0:00:00.00,0:00:01.00,Default,y`.replace(/\\n/g, '\n')],
    }),
  );
  const notedClip = copyCueClipboard(notedSrc, ids(notedSrc, [0]));
  check(notedClip.cues[0].notesBefore, '; src event note'); // snapshotted with the cue
  check(notedClip.styles[0].notesBefore, undefined);
  const notedOut = pasteCueClipboard(notedDst, notedClip, 0);
  const notedText = serializeAss(notedOut.doc);
  check(notedText.includes('; dst style note'), true);
  check(notedText.includes('; dst event note'), true);
  check(notedText.includes('; src style note'), false);
  check(notedText.includes('; src event note'), false);

  // The source and target inputs are never modified.
  const srcBefore = JSON.stringify(src);
  const dstBefore = JSON.stringify(dst);
  pasteCueClipboard(dst, copyCueClipboard(src, ids(src, [0, 1, 2])), 0);
  check(JSON.stringify(src), srcBefore);
  check(JSON.stringify(dst), dstBefore);

  // The clipboard object handed to paste is not modified either.
  const clipBefore = JSON.stringify(clip);
  pasteCueClipboard(dst, clip, 0);
  check(JSON.stringify(clip), clipBefore);
});

// --- compatibility --------------------------------------------------------------------
checked('compatibility', ({ check }) => {
  // The old pasteCues API still works exactly as before.
  const target = parseAss(TARGET);
  const clip = [target.cues[0]];
  const old = pasteCues(target.cues, clip, 0);
  check(old.cues.length, 2);
  check(old.newIds.length, 1);
  check(old.cues[1].startMs, 1000);
  check(old.cues[1].endMs, 2000);

  // Non-ASS -> ASS: no style migration, plain cue paste.
  const srtish = {
    format: 'srt',
    cues: [{ id: 'x1', startMs: 500, endMs: 1500, text: 'srt line' }],
    eol: '\n',
    bom: false,
    finalNewline: true,
  };
  const srtClip = copyCueClipboard(srtish, ['x1']);
  check(srtClip.styles, []);
  const assTarget = parseAss(TARGET);
  const assOut = pasteCueClipboard(assTarget, srtClip, 0);
  check(assOut.styleMap, {});
  check(assOut.doc.styles.length, assTarget.styles.length);
  check(assOut.doc.cues[1].text, 'srt line');
  check(assOut.doc.cues[1].startMs, 1000);

  // ASS -> non-ASS: cue text/style names travel but no styles are added.
  const assSrc = parseAss(SOURCE);
  const vttish = {
    format: 'vtt',
    cues: [{ id: 'v1', startMs: 0, endMs: 1000, text: 'vtt line' }],
    eol: '\n',
    bom: false,
    finalNewline: true,
  };
  const assClip = copyCueClipboard(assSrc, ids(assSrc, [0]));
  const vttOut = pasteCueClipboard(vttish, assClip, 0);
  check(vttOut.styleMap, {});
  check(vttOut.doc.styles, undefined);
  check(vttOut.doc.cues.length, 2);
  check(vttOut.doc.cues[1].text, '{\\rTitle\\b1}Hello'); // untouched: no ASS style migration
  check(vttOut.doc.cues[1].startMs, 1000);

  // Empty clipboards are a no-op that returns the document as given.
  const empty = copyCueClipboard(assSrc, []);
  const emptyOut = pasteCueClipboard(assTarget, empty, 0);
  check(emptyOut.newIds, []);
  check(emptyOut.doc.cues.length, assTarget.cues.length);
  check(emptyOut.styleMap, {});

  // Named reset boundaries: a bare \r stays bare, a reset at block end, and `\b1` after
  // the name is untouched. Font names with an "r" and \t(...) arguments are not renamed.
  const edgeSrc = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        'Style: Default,Arial,72,&H00FFFFFF,0',
        'Style: Title,Georgia,60,&H0000FFFF,-1',
        'Style: Right,Consolas,40,&H00FF0000,0',
      ],
      eventFormat: EV,
      events: [
        String.raw`Dialogue: 0,0:00:01.00,0:00:02.00,Default,{\r}A{\rTitle}B`,
        String.raw`Dialogue: 0,0:00:02.00,0:00:03.00,Default,{\rRight}`,
        String.raw`Dialogue: 0,0:00:03.00,0:00:04.00,Default,{\fnArial\t(0,200,\rTitle)}{\rTitle\i1}z`,
      ],
    }),
  );
  const edgeDst = parseAss(
    assDoc({
      styleFormat: ST,
      styles: [
        'Style: Default,Arial,10,&H00FFFFFF,0',
        'Style: Title,Arial,11,&H00FFFFFF,0',
        'Style: Right,Arial,12,&H00FFFFFF,0',
      ],
      eventFormat: EV,
      events: ['Dialogue: 0,0:00:00.00,0:00:01.00,Default,base'],
    }),
  );
  const edgeClip = copyCueClipboard(edgeSrc, ids(edgeSrc, [0, 1, 2]));
  const edgeOut = pasteCueClipboard(edgeDst, edgeClip, 0);
  check(edgeOut.doc.cues[1].text, String.raw`{\r}A{\rTitle (paste 2)}B`);
  check(edgeOut.doc.cues[2].text, String.raw`{\rRight (paste 2)}`);
  check(
    edgeOut.doc.cues[3].text,
    String.raw`{\fnArial\t(0,200,\rTitle)}{\rTitle (paste 2)\i1}z`,
  );
  check(edgeOut.styleMap, {
    Default: 'Default (paste 2)',
    Title: 'Title (paste 2)',
    Right: 'Right (paste 2)',
  });
});
