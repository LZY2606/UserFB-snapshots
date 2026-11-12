import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compile, decompile, sampleDialogue } from '../src/index.js';

const HEADER = String.raw`[Script Info]
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1
Style: alt,Georgia,24,&H00FF0000,&H000000FF,&H00000000,&H00000000,1,1,0,0,100,100,0,0,1,2,2,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Text
`;

function timelineASS(body, style = 'Default') {
  return `${HEADER}Dialogue: 0,0:00:01.00,0:00:05.00,${style},${body}`;
}

function compileTimeline(body, style = 'Default') {
  return compile(timelineASS(body, style), { animationTimeline: true });
}

// All dialogues above start at 1s, so `offset` is milliseconds into the line.
function at(offsetMs) {
  return 1 + offsetMs / 1000;
}

function sampleTag(body, offsetMs, style = 'Default') {
  const compiled = compileTimeline(body, style);
  const fragments = sampleDialogue(compiled, 0, at(offsetMs));
  return fragments[0].tag;
}

function stripTimeline(value) {
  if (Array.isArray(value)) {
    return value.map(stripTimeline);
  }
  if (value && typeof value === 'object') {
    const result = {};
    Object.keys(value).forEach((key) => {
      if (key !== 'animationTimeline' && key !== 'timelineIndex') {
        result[key] = stripTimeline(value[key]);
      }
    });
    return result;
  }
  return value;
}

test('ordered-overrides', () => {
  // Hand-checked stacked example: 20 -> 40 then -> 80, two linear transforms
  // over the same window. At the midpoint the first transform alone is 30.
  const firstOnly = compileTimeline(String.raw`{\t(0,2000,\fs40)}A`);
  assert.equal(sampleDialogue(firstOnly, 0, at(1000))[0].tag.fs, 30);
  // Chained, the second transform starts from that accumulated 30 instead
  // of the style default, so at the midpoint it reaches (30 + 80) / 2 = 55.
  const chained = compileTimeline(String.raw`{\t(0,2000,\fs40)\t(0,2000,\fs80)}A`);
  assert.equal(sampleDialogue(chained, 0, at(1000))[0].tag.fs, 55);
  // Swapping the order changes the answer: 20 -> 80 gives 50, then 50 -> 40
  // at the midpoint is 45. Independent-from-default + last-wins would give
  // 30 here, which must not happen.
  const swapped = compileTimeline(String.raw`{\t(0,2000,\fs80)\t(0,2000,\fs40)}A`);
  assert.equal(sampleDialogue(swapped, 0, at(1000))[0].tag.fs, 45);
  assert.notEqual(sampleDialogue(swapped, 0, at(1000))[0].tag.fs, 30);

  // A static assignment before the transform is the transform's start value.
  assert.equal(sampleTag(String.raw`{\fs30\t(0,1000,\fs60)}A`, 500).fs, 45);
  // A static assignment after the transform wins for that property.
  assert.equal(sampleTag(String.raw`{\t(0,1000,\fs60)\fs30}A`, 500).fs, 30);
  // The static set only overrides the property it touches.
  const partial = sampleTag(String.raw`{\t(0,1000,\fs60\fsp10)\fs30}A`, 500);
  assert.equal(partial.fs, 30);
  assert.equal(partial.fsp, 5);

  // Operations are inherited by later fragments without re-applying them.
  const inherited = compileTimeline(String.raw`{\fs30}A{\fsp4}B`);
  const fragments = sampleDialogue(inherited, 0, at(0));
  assert.equal(fragments[0].text, 'A');
  assert.equal(fragments[0].tag.fs, 30);
  assert.equal(fragments[1].text, 'B');
  assert.equal(fragments[1].tag.fs, 30);
  assert.equal(fragments[1].tag.fsp, 4);
  assert.equal(fragments[0].tag.fsp, 0);
});

test('overlap-accel', () => {
  // Two overlapping transforms on the same property stack:
  // at 750ms: first gives 20 + 20 * 0.75 = 35,
  // second is at p = 0.25: 35 + (80 - 35) * 0.25 = 46.25.
  const overlap = compileTimeline(String.raw`{\t(0,1000,\fs40)\t(500,1500,\fs80)}A`);
  assert.equal(sampleDialogue(overlap, 0, at(750))[0].tag.fs, 46.25);

  // Acceleration: p = 0.5^2 = 0.25 at the midpoint.
  assert.equal(sampleTag(String.raw`{\t(0,1000,2,\fs40)}A`, 500).fs, 25);
  // p = 0.5^4 = 0.0625.
  assert.equal(sampleTag(String.raw`{\t(0,1000,4,\fs40)}A`, 500).fs, 21.25);

  // Zero-length transform is a step at t1, not a division by zero.
  const step = compileTimeline(String.raw`{\t(500,500,\fs80)}A`);
  assert.equal(sampleDialogue(step, 0, at(400))[0].tag.fs, 20);
  assert.equal(sampleDialogue(step, 0, at(500))[0].tag.fs, 80);
  assert.equal(sampleDialogue(step, 0, at(600))[0].tag.fs, 80);

  // Invalid transform parameters are rejected while the option is on.
  assert.throws(() => compileTimeline(String.raw`{\t(1000,500,\fs40)}A`), RangeError);
  assert.throws(() => compileTimeline(String.raw`{\t(0,1000,0,\fs40)}A`), RangeError);
  assert.throws(() => compileTimeline(String.raw`{\t(0,1000,-2,\fs40)}A`), RangeError);
  assert.throws(() => compileTimeline(String.raw`{\t(0,1000,NaN,\fs40)}A`));
});

test('reset-slices', () => {
  // `\r` wipes both static and animated state and restores style defaults.
  const reset = compileTimeline(String.raw`{\fs30\i1}A{\r}B`);
  const fragments = sampleDialogue(reset, 0, at(0));
  assert.equal(fragments[0].tag.fs, 30);
  assert.equal(fragments[0].tag.i, 1);
  assert.equal(fragments[1].tag.fs, 20);
  assert.equal(fragments[1].tag.i, 0);

  // Named reset switches to the other style's defaults.
  const named = compileTimeline(String.raw`{\fs30}A{\ralt}B`);
  const namedFragments = sampleDialogue(named, 0, at(0));
  assert.equal(namedFragments[1].tag.fs, 24);
  assert.equal(namedFragments[1].tag.fn, 'Georgia');
  assert.equal(namedFragments[1].tag.b, 1);
  assert.equal(namedFragments[0].tag.fn, 'Arial');

  // The animation clock is not restarted by `\r`: the second transform is
  // still measured from the dialogue start.
  const clock = compileTimeline(String.raw`{\t(0,1000,\fs40)}A{\r\t(0,1000,\fs40)}B`);
  assert.equal(sampleDialogue(clock, 0, at(250))[1].tag.fs, 25);
  assert.equal(sampleDialogue(clock, 0, at(1000))[1].tag.fs, 40);

  // Relative font sizes are expanded once at compile time against the
  // current static state, never re-scaled while sampling.
  // \fs+2 on 20 adds 20% -> 24; \fs+5 on 20 adds 50% -> 30.
  const relative = compileTimeline(String.raw`{\fs+2}A`);
  assert.equal(sampleDialogue(relative, 0, at(0))[0].tag.fs, 24);
  assert.equal(sampleDialogue(relative, 0, at(3000))[0].tag.fs, 24);
  assert.equal(sampleTag(String.raw`{\fs+5}A`, 0).fs, 30);
  // Inside a transform the relative value resolves against the static state
  // of the preceding fragment: \fs+10 doubles the static 30 to 60 once.
  const relativeT = compileTimeline(String.raw`{\fs30}A{\t(0,1000,\fs+10)}B`);
  const relFragments = sampleDialogue(relativeT, 0, at(500));
  assert.equal(relFragments[1].tag.fs, 45);
  assert.equal(sampleDialogue(relativeT, 0, at(1000))[1].tag.fs, 60);

  // Static style properties survive alongside the animated ones.
  const styled = sampleTag(String.raw`{\b1\i1\u1\fnCourier\fscx120}A`, 0);
  assert.equal(styled.b, 1);
  assert.equal(styled.i, 1);
  assert.equal(styled.u, 1);
  assert.equal(styled.fn, 'Courier');
  assert.equal(styled.fscx, 120);
});

test('color-alpha', () => {
  // BBGGRR bytes interpolate independently: (01,02,03) -> (04,08,0C) at the
  // midpoint gives (03,05,08), and halves round up.
  const color = sampleTag(String.raw`{\1c&H010203&\t(0,1000,\1c&H04080C&)}A`, 500);
  assert.equal(color.c1, '030508');
  // The endpoints are exact, the color is never treated as one integer.
  assert.equal(sampleTag(String.raw`{\1c&H010203&\t(0,1000,\1c&H04080C&)}A`, 0).c1, '010203');
  assert.equal(sampleTag(String.raw`{\1c&H010203&\t(0,1000,\1c&H04080C&)}A`, 1000).c1, '04080C');

  // White -> black midpoint is 0x80 (127.5 rounds up to 128) on every byte.
  assert.equal(sampleTag(String.raw`{\1c&HFFFFFF&\t(0,1000,\1c&H000000&)}A`, 500).c1, '808080');

  // `\alpha` expands to all four alpha bytes; transparency 0x00 -> 0xFF at
  // the midpoint is 0x80.
  const alpha = sampleTag(String.raw`{\alpha&H00&\t(0,1000,\alpha&HFF&)}A`, 500);
  assert.equal(alpha.a1, '80');
  assert.equal(alpha.a2, '80');
  assert.equal(alpha.a3, '80');
  assert.equal(alpha.a4, '80');

  // A single targeted alpha only moves that byte.
  const single = sampleTag(String.raw`{\3a&H00&\t(0,1000,\3a&HFF&)}A`, 500);
  assert.equal(single.a3, '80');
  assert.equal(single.a1, '00');

  // Static color assignments are plain replacements.
  assert.equal(sampleTag(String.raw`{\1c&HFF0000&}A`, 0).c1, 'FF0000');
});

test('time-domain', () => {
  const compiled = compileTimeline(String.raw`{\t(0,4000,\fs40)}A`);
  // [start, end): the start is included, the end is excluded.
  assert.ok(sampleDialogue(compiled, 0, 1) !== null);
  assert.equal(sampleDialogue(compiled, 0, 5), null);
  assert.equal(sampleDialogue(compiled, 0, 0.999), null);
  assert.equal(sampleDialogue(compiled, 0, 5.001), null);

  // Invalid indices and non-finite times throw instead of returning null.
  assert.throws(() => sampleDialogue(compiled, -1, 1), RangeError);
  assert.throws(() => sampleDialogue(compiled, 1, 1), RangeError);
  assert.throws(() => sampleDialogue(compiled, 1.5, 1), RangeError);
  assert.throws(() => sampleDialogue(compiled, 0, NaN), RangeError);
  assert.throws(() => sampleDialogue(compiled, 0, Infinity), RangeError);
  assert.throws(() => sampleDialogue(compiled, 0, -Infinity), RangeError);

  // A compiled object without the timeline cannot be sampled.
  const plain = compile(timelineASS(String.raw`{\fs40}A`));
  assert.throws(() => sampleDialogue(plain, 0, 1), Error);

  // Time may jump backwards and queries for different dialogues may be
  // interleaved on the same compiled object.
  const two = compile(`${HEADER}Dialogue: 0,0:00:01.00,0:00:05.00,Default,{\\t(0,4000,\\fs40)}A
Dialogue: 0,0:00:02.00,0:00:06.00,alt,{\\t(0,4000,\\fs44)}B`, { animationTimeline: true });
  assert.equal(sampleDialogue(two, 0, at(3000))[0].tag.fs, 35);
  assert.equal(sampleDialogue(two, 1, 2 + 3)[0].tag.fs, 39); // alt starts at 24 -> 44
  assert.equal(sampleDialogue(two, 0, at(0))[0].tag.fs, 20); // backwards jump
  assert.equal(sampleDialogue(two, 1, 2)[0].tag.fs, 24);
  assert.equal(sampleDialogue(two, 0, at(3000))[0].tag.fs, 35); // repeatable
});

test('compatibility', () => {
  const body = String.raw`{\fs30\t(0,1000,\fs40)\an8\pos(10,20)\fad(100,200)\k10}A{\r\fs50}B`;
  const plain = compile(timelineASS(body));
  const timed = compile(timelineASS(body), { animationTimeline: true });

  // Default output keeps its exact shape: no timeline fields are added.
  assert.equal(plain.dialogues[0].animationTimeline, undefined);
  plain.dialogues[0].slices.forEach((slice) => {
    slice.fragments.forEach((fragment) => {
      assert.equal('timelineIndex' in fragment, false);
    });
  });

  // Apart from the timeline fields the compiled structure is identical.
  assert.deepEqual(stripTimeline(timed), stripTimeline(plain));
  // Decompilation is unchanged with the option enabled.
  assert.equal(decompile(timed), decompile(plain));

  // Unsupported animation targets are rejected explicitly when sampling is
  // requested, but stay silently ignored with the option off.
  const unsupported = [
    String.raw`{\t(\pos(1,2))}A`,
    String.raw`{\t(\clip(1,2,3,4))}A`,
    String.raw`{\t(\be5)}A`,
    String.raw`{\t(\b1)}A`,
  ];
  unsupported.forEach((text) => {
    assert.throws(() => compile(timelineASS(text), { animationTimeline: true }));
    assert.doesNotThrow(() => compile(timelineASS(text)));
  });

  // Position, fade and karaoke fields are still compiled as before.
  const kept = compileTimeline(String.raw`{\pos(10,20)\fad(100,200)\k10}A`).dialogues[0];
  assert.deepEqual(kept.pos, { x: 10, y: 20 });
  assert.deepEqual(kept.fade, { type: 'fad', t1: 100, t2: 200 });
  assert.equal(kept.slices[0].fragments[0].tag.k, 10);

  // Drawing fragments return an independent copy without pixel computation.
  const drawn = compileTimeline(String.raw`{\p1}m 0 0 l 10 0 10 10{\p0}`);
  const sourceDrawing = drawn.dialogues[0].slices[0].fragments[0].drawing;
  const sampled = sampleDialogue(drawn, 0, at(0));
  assert.deepEqual(sampled[0].drawing, sourceDrawing);
  assert.notEqual(sampled[0].drawing, sourceDrawing);
  sampled[0].drawing.minX = 999;
  assert.equal(sourceDrawing.minX, 0);

  // Neither the compiled object, its styles nor previous results are mutated
  // by queries, including queries made in reverse time order.
  const snapshot = JSON.stringify(timed);
  const stylesSnapshot = JSON.stringify(timed.styles);
  const first = sampleDialogue(timed, 0, at(800));
  const firstSnapshot = JSON.stringify(first);
  first[0].tag.fs = 0;
  first[0].text = 'mutated';
  const second = sampleDialogue(timed, 0, at(200));
  const third = sampleDialogue(timed, 0, at(800));
  assert.equal(JSON.stringify(third), firstSnapshot);
  assert.ok(second[0].tag.fs !== 0);
  assert.equal(JSON.stringify(timed), snapshot);
  assert.equal(JSON.stringify(timed.styles), stylesSnapshot);
});
