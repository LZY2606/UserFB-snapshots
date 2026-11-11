import * as fontkit from 'fontkit';
import assert from 'assert';
import { buildSourceTestFont } from '../checks/make-source-test-font.mjs';

describe('layoutWithSource', function () {
  let continuation = fontkit.openSync(
    new URL('data/lookup-continuation.ttf', import.meta.url),
  );
  let synth = fontkit.create(buildSourceTestFont());
  let amiri = fontkit.openSync(
    new URL('data/amiri/amiri-regular.ttf', import.meta.url),
  );

  it('should track sources through one-to-many substitutions', function () {
    let run = continuation.layoutWithSource('aa');
    assert.deepEqual(
      run.glyphs.map((g) => g.id),
      [2, 1, 2, 1],
    );
    assert.deepEqual(run.sourceRanges, [
      [[0, 1]],
      [[0, 1]],
      [[1, 2]],
      [[1, 2]],
    ]);
  });

  it('should keep skipped marks out of ligature sources', function () {
    let run = synth.layoutWithSource('áb');
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
  });

  it('should not lend deleted characters to other glyphs', function () {
    let run = continuation.layoutWithSource('ccb');
    assert.deepEqual(
      run.glyphs.map((g) => g.id),
      [2],
    );
    assert.deepEqual(run.sourceRanges, [[[2, 3]]]);
  });

  it('should reverse sources along with RTL glyphs', function () {
    let run = amiri.layoutWithSource('١٢٣', 'arab', 'ARA ', 'rtl');
    assert.deepEqual(
      run.glyphs.map((g) => g.id),
      [448, 447, 446],
    );
    assert.deepEqual(run.sourceRanges, [[[2, 3]], [[1, 2]], [[0, 1]]]);
  });

  it('should produce the same glyphs and positions as layout', function () {
    let tracked = continuation.layoutWithSource('aa');
    let plain = continuation.layout('aa');
    assert.deepEqual(
      tracked.glyphs.map((g) => g.id),
      plain.glyphs.map((g) => g.id),
    );
    assert.deepEqual(
      tracked.positions.map((p) => ({ ...p })),
      plain.positions.map((p) => ({ ...p })),
    );
    assert.equal(plain.sourceRanges, undefined);
  });

  it('should not share source ranges between runs', function () {
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
    assert.equal(second.glyphs[0].sourceRanges, undefined);
  });

  it('should return empty sources for an empty string', function () {
    let run = continuation.layoutWithSource('');
    assert.deepEqual(run.glyphs, []);
    assert.deepEqual(run.sourceRanges, []);
  });

  it('should reject unsupported inputs', function () {
    assert.throws(() =>
      continuation.layoutWithSource([continuation.glyphForCodePoint(97)]),
    );
    assert.throws(() => continuation.layoutWithSource('a\uD800'));
    assert.throws(() => continuation.layoutWithSource('\uDC00'));
    assert.throws(() => continuation.layoutWithSource('aa', [], 'thai'));
    let morx = fontkit.openSync(
      new URL('data/fonttest/TestMORXEleven.ttf', import.meta.url),
    );
    assert.throws(() => morx.layoutWithSource('abc'));
  });
});
