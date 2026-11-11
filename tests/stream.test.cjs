// Streaming parser tests: chunked delivery, line terminators, error/region
// recovery, decoder end-of-stream handling and per-instance state isolation.
// Runs with the built-in node:test runner against the public lib exports.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {WebVTT, VTTCue, VTTRegion} = require('../lib');

// Feed `parts` through a fresh parser and record every event as it happens.
// mode 'bytes' decodes Buffers with a native TextDecoder, mode 'string' feeds
// strings through WebVTT.StringDecoder. `snapshots[i]` is the ordered list of
// event types observed up to the return of the i-th parse() call, so tests
// can check what was delivered before flush().
function run(parts, mode) {
  const parser = new WebVTT.Parser(
    {VTTCue, VTTRegion},
    mode === 'string' ? WebVTT.StringDecoder() : new TextDecoder());
  const cues = [];
  const regions = [];
  const errors = [];
  const log = [];
  let flushes = 0;
  parser.oncue = cue => {
    cues.push(cue);
    log.push('cue');
  };
  parser.onregion = region => {
    regions.push(region);
    log.push('region');
  };
  parser.onparsingerror = error => {
    errors.push(error.code);
    log.push('error:' + error.code);
  };
  parser.onflush = () => {
    flushes++;
    log.push('flush');
  };
  const snapshots = [];
  for (const part of parts) {
    parser.parse(part);
    snapshots.push(log.join(' '));
  }
  parser.flush();
  return {cues, regions, errors, flushes, snapshots};
}

// Serializable projection of a cue for deepEqual comparisons.
function cueView(cue) {
  return [cue.id, cue.startTime, cue.endTime, cue.text];
}
function cueViews(cues) {
  return cues.map(cueView);
}

// All ways to chop a source up that a test input has to survive:
// whole, every single two-chunk split point, and one part per unit
// (byte for binary mode, Unicode code point for string mode).
function* replaysOf(source, mode) {
  if (mode === 'string') {
    yield {label: 'string/whole', parts: [source]};
    const chars = [...source];
    let offset = 0;
    for (let i = 0; i <= chars.length; i++) {
      yield {label: 'string/split@char' + i, parts: [source.slice(0, offset), source.slice(offset)]};
      offset += i < chars.length ? chars[i].length : 0;
    }
    yield {label: 'string/chars', parts: chars};
    return;
  }
  const buf = Buffer.from(source, 'utf8');
  yield {label: 'bytes/whole', parts: [buf]};
  for (let i = 0; i <= buf.length; i++) {
    yield {label: 'bytes/split@' + i, parts: [buf.subarray(0, i), buf.subarray(i)]};
  }
  yield {label: 'bytes/dribble', parts: [...buf].map(b => Buffer.from([b]))};
}

test('replay-splits', () => {
  // Short inputs, all LF terminated. Every one is checked as a whole, at
  // every single split point, byte by byte, and through the string decoder.
  const inputs = [
    {
      name: 'blank-separated',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        'first\n' +
        '\n' +
        '00:03.000 --> 00:04.000\n' +
        'second\n' +
        '\n',
      cues: [['', 1, 2, 'first'], ['', 3, 4, 'second']],
      errors: []
    },
    {
      // No blank line between the cues: the next timing line terminates the
      // previous cue and starts the next one.
      name: 'no-blank-transition',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        'first\n' +
        '00:03.000 --> 00:04.000\n' +
        'second\n' +
        '\n',
      cues: [['', 1, 2, 'first'], ['', 3, 4, 'second']],
      errors: []
    },
    {
      // Three cues without blank lines; the adjacent timing lines give the
      // middle cue an empty text.
      name: 'triple-adjacent-empty-text',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        'one\n' +
        '00:02.000 --> 00:03.000\n' +
        '00:03.000 --> 00:04.000\n' +
        'three\n' +
        '\n',
      cues: [['', 1, 2, 'one'], ['', 2, 3, ''], ['', 3, 4, 'three']],
      errors: []
    },
    {
      // No blank line after the last cue text; flush() has to deliver it.
      name: 'no-trailing-blank',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        'first\n' +
        '00:03.000 --> 00:04.000\n' +
        'second\n',
      cues: [['', 1, 2, 'first'], ['', 3, 4, 'second']],
      errors: []
    },
    {
      // No line terminator after the last cue text at all.
      name: 'no-trailing-newline',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        'first',
      cues: [['', 1, 2, 'first']],
      errors: []
    },
    {
      // Multi-byte content: byte splits cut through the characters.
      name: 'multibyte',
      source: 'WEBVTT\n' +
        '\n' +
        '00:01.000 --> 00:02.000\n' +
        '中文🙂\n' +
        '00:03.000 --> 00:04.000\n' +
        'tail 中\n' +
        '\n',
      cues: [['', 1, 2, '中文🙂'], ['', 3, 4, 'tail 中']],
      errors: []
    },
    {
      // A cue identifier line followed by the timing line.
      name: 'cue-id',
      source: 'WEBVTT\n' +
        '\n' +
        'my-id\n' +
        '00:01.000 --> 00:02.000\n' +
        'text\n' +
        '\n',
      cues: [['my-id', 1, 2, 'text']],
      errors: []
    }
  ];

  for (const input of inputs) {
    for (const mode of ['bytes', 'string']) {
      for (const replay of replaysOf(input.source, mode)) {
        const label = input.name + ' ' + replay.label;
        const r = run(replay.parts, mode);
        assert.deepEqual(cueViews(r.cues), input.cues, label + ' cues');
        assert.deepEqual(r.errors, input.errors, label + ' errors');
        assert.equal(r.flushes, 1, label + ' flushes');
      }
    }
  }

  // Events must be delivered as soon as their boundary is known, not
  // deferred to flush(). Splitting right after the second timing line used
  // to lose the second cue and report a spurious timestamp error.
  const noBlank = inputs[1].source;
  const cut = noBlank.indexOf('second');
  const split = run([Buffer.from(noBlank.slice(0, cut)), Buffer.from(noBlank.slice(cut))], 'bytes');
  assert.deepEqual(split.snapshots[0], 'cue', 'no-blank events after first chunk');
  assert.deepEqual(split.snapshots[1], 'cue cue', 'no-blank events after second chunk');
  assert.deepEqual(cueViews(split.cues), inputs[1].cues, 'no-blank split cues');

  // A whole chunk whose cues are fully terminated delivers them all before
  // flush() is called.
  const whole = run([Buffer.from(inputs[0].source)], 'bytes');
  assert.deepEqual(whole.snapshots[0], 'cue cue', 'blank-separated events at parse return');

  // Without a trailing terminator the last cue boundary is only known at
  // flush time; nothing may be delivered early, and flush must not lose it.
  const open = run([Buffer.from(inputs[4].source)], 'bytes');
  assert.deepEqual(open.snapshots[0], '', 'open-ended cue not delivered during parse');
  assert.deepEqual(cueViews(open.cues), inputs[4].cues, 'open-ended cue delivered by flush');

  // An empty parse() call must not disturb a parser waiting for more input.
  const paused = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nfi'),
    '',
    Buffer.from('rst\n\n')
  ], 'bytes');
  assert.deepEqual(cueViews(paused.cues), [['', 1, 2, 'first']], 'empty parse keeps waiting state');
  assert.deepEqual(paused.errors, [], 'empty parse adds no errors');
});

test('line-endings', () => {
  const body = '00:01.000 --> 00:02.000\nalpha\nbeta\n\n';
  const expected = [['', 1, 2, 'alpha\nbeta']];
  const variants = [
    {name: 'lf', source: 'WEBVTT\n\n' + body},
    {name: 'crlf', source: 'WEBVTT\r\n\r\n' + body.replaceAll('\n', '\r\n')},
    {name: 'cr', source: 'WEBVTT\r\r' + body.replaceAll('\n', '\r')},
    {
      name: 'mixed',
      source: 'WEBVTT\r\n' +
        '\n' +
        '00:01.000 --> 00:02.000\r' +
        'alpha\r\n' +
        'beta\n' +
        '\r' +
        '\n'
    }
  ];
  for (const variant of variants) {
    for (const replay of replaysOf(variant.source, 'bytes')) {
      const label = variant.name + ' ' + replay.label;
      const r = run(replay.parts, 'bytes');
      assert.deepEqual(cueViews(r.cues), expected, label + ' cues');
      assert.deepEqual(r.errors, [], label + ' errors');
    }
  }

  // A CR at the end of one chunk and a LF at the start of the next are one
  // single terminator, not two. An extra empty line here would cut the cue
  // text in half and derail the rest of the parse.
  const splitCRLF = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nalpha\r'),
    Buffer.from('\nbeta\r\n\r\n')
  ], 'bytes');
  assert.deepEqual(cueViews(splitCRLF.cues), expected, 'cross-chunk CRLF cues');
  assert.deepEqual(splitCRLF.errors, [], 'cross-chunk CRLF errors');

  // A CR followed by a non-LF character: the CR already ended the line and
  // the next character belongs to the next line.
  const crThenText = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nalpha\r'),
    Buffer.from('beta\n\n')
  ], 'bytes');
  assert.deepEqual(cueViews(crThenText.cues), expected, 'CR then text cues');

  // A CR followed by a real second terminator: the second terminator is a
  // genuine empty line and must not be swallowed.
  const crThenTerminator = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nalpha\r'),
    Buffer.from('\r\n\n')
  ], 'bytes');
  assert.deepEqual(cueViews(crThenTerminator.cues), [['', 1, 2, 'alpha']], 'CR then terminator cues');
  assert.deepEqual(crThenTerminator.errors, [], 'CR then terminator errors');

  // An empty parse() between the CR and the LF keeps the merge pending.
  const emptyBetween = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nalpha\r'),
    '',
    Buffer.from('\nbeta\n\n')
  ], 'bytes');
  assert.deepEqual(cueViews(emptyBetween.cues), expected, 'empty parse between CR and LF');

  // A CR-terminated header line is processed immediately; the parser does
  // not hold it back waiting for a LF that may never arrive.
  const regionCR = run([
    Buffer.from('WEBVTT\nRegion: id=r width=30%\r'),
    Buffer.from('\n\n00:01.000 --> 00:02.000 region:r\nx\n\n')
  ], 'bytes');
  assert.deepEqual(regionCR.snapshots[0], 'region', 'CR-terminated region delivered at parse return');
  assert.equal(regionCR.regions.length, 1, 'CR region count');
  assert.equal(regionCR.regions[0].width, 30, 'CR region width');
  assert.equal(regionCR.cues[0].region, regionCR.regions[0], 'CR cue binds region');
  assert.deepEqual(regionCR.errors, [], 'CR region errors');

  // NOTE blocks and the bad-cue discard state follow the same CR rules.
  const noteCR = run([Buffer.from(
    'WEBVTT\r' +
    '\r' +
    'NOTE\r' +
    'comment\r' +
    '\r' +
    '00:01.000 --> 00:02.000\r' +
    'x\r' +
    '\r'
  )], 'bytes');
  assert.deepEqual(cueViews(noteCR.cues), [['', 1, 2, 'x']], 'NOTE with CR cues');
  assert.deepEqual(noteCR.errors, [], 'NOTE with CR errors');

  const badCueCR = run([Buffer.from(
    'WEBVTT\r' +
    '\r' +
    'bad\r' +
    'invalid --> timing\r' +
    'skip\r' +
    '\r' +
    '00:01.000 --> 00:02.000\r' +
    'x\r' +
    '\r'
  )], 'bytes');
  assert.deepEqual(cueViews(badCueCR.cues), [['', 1, 2, 'x']], 'bad cue with CR cues');
  assert.deepEqual(badCueCR.errors, [1], 'bad cue with CR errors');

  // A WEBVTT signature without any line terminator keeps the existing
  // BadSignature behaviour at flush().
  const noTerminator = run([Buffer.from('WEBVTT')], 'bytes');
  assert.deepEqual(noTerminator.cues, [], 'unterminated signature cues');
  assert.deepEqual(noTerminator.errors, [0], 'unterminated signature BadSignature');

  // Repeating flush() must not re-deliver cues, regions or errors, but
  // onflush fires for every call.
  const parser = new WebVTT.Parser({VTTCue, VTTRegion}, new TextDecoder());
  const cues = [];
  const regions = [];
  const errors = [];
  let flushes = 0;
  parser.oncue = cue => cues.push(cueView(cue));
  parser.onregion = region => regions.push(region);
  parser.onparsingerror = error => errors.push(error.code);
  parser.onflush = () => flushes++;
  parser.parse(Buffer.from('WEBVTT\r\nRegion: id=r width=30%\r\n\r\n00:01.000 --> 00:02.000\r\nx\r\n\r\n'));
  parser.flush();
  assert.deepEqual(cues, [['', 1, 2, 'x']], 'flush repeat cues after first flush');
  parser.flush();
  parser.flush();
  assert.deepEqual(cues, [['', 1, 2, 'x']], 'flush repeat does not resend cues');
  assert.equal(regions.length, 1, 'flush repeat does not resend regions');
  assert.deepEqual(errors, [], 'flush repeat does not resend errors');
  assert.equal(flushes, 3, 'onflush fires per call');
});

test('recovery-regions', () => {
  // One mixed chain: duplicate-id Region: headers, a NOTE block, one broken
  // timing line discarded up to the blank line, a normal cue after it, and
  // a no-blank transition into the last cue.
  const source = 'WEBVTT\n' +
    'Region: id=r width=30%\n' +
    'Region: id=r width=40% lines=2\n' +
    '\n' +
    'NOTE ignored\n' +
    'not a cue\n' +
    '\n' +
    'bad\n' +
    'invalid --> timing\n' +
    'skip\n' +
    '\n' +
    'id1\n' +
    '00:01.000 --> 00:02.000 region:r line:25% position:30% size:70% align:start\n' +
    '中🙂\n' +
    '00:03.000 --> 00:04.000 line:3\n' +
    'next\n' +
    '\n';

  function checkChain(r, label) {
    // The broken timing line is reported exactly once with code 1.
    assert.deepEqual(r.errors, [1], label + ' errors');
    // Both Region: headers deliver their own region object; the first one
    // is not overwritten by the duplicate id.
    assert.equal(r.regions.length, 2, label + ' region count');
    assert.equal(r.regions[0].width, 30, label + ' first region kept');
    assert.equal(r.regions[1].width, 40, label + ' second region width');
    assert.equal(r.regions[1].lines, 2, label + ' second region lines');
    // Cues after the discarded one arrive in order.
    assert.deepEqual(cueViews(r.cues), [
      ['id1', 1, 2, '中🙂'],
      ['', 3, 4, 'next']
    ], label + ' cues');
    // region:r binds the last declared region object.
    assert.equal(r.cues[0].region, r.regions[1], label + ' cue binds last region');
    assert.equal(r.cues[1].region, null, label + ' second cue has no region');
    // line:25% turns snapToLines off, an integer line keeps it on.
    assert.equal(r.cues[0].line, 25, label + ' percent line value');
    assert.equal(r.cues[0].snapToLines, false, label + ' percent line snapToLines');
    assert.equal(r.cues[1].line, 3, label + ' integer line value');
    assert.equal(r.cues[1].snapToLines, true, label + ' integer line snapToLines');
    assert.equal(r.flushes, 1, label + ' flushes');
  }

  for (const mode of ['bytes', 'string']) {
    for (const replay of replaysOf(source, mode)) {
      checkChain(run(replay.parts, mode), 'chain ' + replay.label);
    }
  }

  // Everything up to the final blank line is already delivered when parse()
  // returns; flush() only adds the flush notification.
  const whole = run([Buffer.from(source)], 'bytes');
  assert.deepEqual(
    whole.snapshots[0],
    'region region error:1 cue cue',
    'chain events at parse return'
  );
});

test('decoder-eof', () => {
  // A truncated four-byte UTF-8 character at the very end of the stream:
  // the native TextDecoder only emits the replacement character from its
  // final decode() call, and it still belongs to the last cue's text.
  const truncated = run([Buffer.concat([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nx'),
    Buffer.from([0xf0, 0x9f])
  ])], 'bytes');
  assert.deepEqual(truncated.snapshots[0], '', 'truncated tail not delivered during parse');
  assert.deepEqual(cueViews(truncated.cues), [['', 1, 2, 'x�']], 'decoder tail joins last cue text');
  assert.deepEqual(truncated.errors, [], 'truncated tail errors');
  assert.equal(truncated.flushes, 1, 'truncated tail flushes');

  // A complete multi-byte character split across chunks decodes intact.
  const emoji = Buffer.from('🙂', 'utf8');
  const splitEmoji = run([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\n中'),
    emoji.subarray(0, 2),
    emoji.subarray(2),
    Buffer.from('\n\n')
  ], 'bytes');
  assert.deepEqual(cueViews(splitEmoji.cues), [['', 1, 2, '中🙂']], 'split multibyte cues');
  assert.deepEqual(splitEmoji.errors, [], 'split multibyte errors');

  // The last cue without a trailing blank line is delivered by flush(),
  // complete and only once, even when flush() is repeated.
  const parser = new WebVTT.Parser({VTTCue, VTTRegion}, new TextDecoder());
  const cues = [];
  const errors = [];
  let flushes = 0;
  parser.oncue = cue => cues.push(cueView(cue));
  parser.onparsingerror = error => errors.push(error.code);
  parser.onflush = () => flushes++;
  parser.parse(Buffer.concat([
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nlast '),
    Buffer.from([0xe4, 0xb8]) // first two bytes of 中
  ]));
  assert.deepEqual(cues, [], 'open last cue waits for flush');
  parser.flush();
  assert.deepEqual(cues, [['', 1, 2, 'last �']], 'flush delivers decoder tail once');
  parser.flush();
  assert.deepEqual(cues, [['', 1, 2, 'last �']], 'second flush does not resend');
  assert.deepEqual(errors, [], 'decoder tail run errors');
  assert.equal(flushes, 2, 'onflush per call');

  // The string decoder path ends the same way.
  const fromString = run(['WEBVTT\n\n00:01.000 --> 00:02.000\nx�'], 'string');
  assert.deepEqual(cueViews(fromString.cues), [['', 1, 2, 'x�']], 'string decoder tail cues');
  assert.deepEqual(fromString.errors, [], 'string decoder tail errors');
});

test('instance-isolation', () => {
  // Two parsers fed interleaved chunks: pending lines, the CR-merge state
  // and the region table must stay per-instance. Parser A (binary) pauses
  // right after a CR; parser B (string) is inside a NOTE block waiting for
  // its terminating empty line. If A's CR state leaked into B, B's empty
  // line would be eaten and its cue lost. If A's region table leaked into
  // B, B's region:ra setting would bind A's region.
  const aParts = [
    Buffer.from('WEBVTT\nRegion: id=ra width=30%\n\n00:01.000 --> 00:02.000 region:ra\nA1\n\r'),
    Buffer.from('\n00:03.000 --> 00:04.000\nA2\n\n')
  ];
  const bParts = [
    'WEBVTT\n\nNOTE\n',
    '\n00:05.000 --> 00:06.000 region:ra\nB1\n\n'
  ];

  function makeParser(mode, sink) {
    const parser = new WebVTT.Parser(
      {VTTCue, VTTRegion},
      mode === 'string' ? WebVTT.StringDecoder() : new TextDecoder());
    parser.oncue = cue => sink.cues.push(cue);
    parser.onregion = region => sink.regions.push(region);
    parser.onparsingerror = error => sink.errors.push(error.code);
    return parser;
  }
  const sinkA = {cues: [], regions: [], errors: []};
  const sinkB = {cues: [], regions: [], errors: []};
  const a = makeParser('bytes', sinkA);
  const b = makeParser('string', sinkB);

  a.parse(aParts[0]); // ends in CR, A remembers the pending CR
  b.parse(bParts[0]); // B waits inside its NOTE block
  b.parse(bParts[1]); // B's empty line must survive A's pending CR
  a.parse(aParts[1]); // A's LF merges with its own CR
  a.flush();
  b.flush();

  assert.deepEqual(cueViews(sinkA.cues), [['', 1, 2, 'A1'], ['', 3, 4, 'A2']], 'A cues');
  assert.equal(sinkA.regions.length, 1, 'A region count');
  assert.equal(sinkA.regions[0].width, 30, 'A region width');
  assert.equal(sinkA.cues[0].region, sinkA.regions[0], 'A cue binds A region');
  assert.equal(sinkA.cues[1].region, null, 'A second cue has no region');
  assert.deepEqual(sinkA.errors, [], 'A errors');

  assert.deepEqual(cueViews(sinkB.cues), [['', 5, 6, 'B1']], 'B cues');
  assert.equal(sinkB.regions.length, 0, 'B region count');
  assert.equal(sinkB.cues[0].region, null, 'B cue does not see A region');
  assert.deepEqual(sinkB.errors, [], 'B errors');
});
