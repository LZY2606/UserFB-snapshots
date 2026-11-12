/* jshint node: true */

// Tests for the binary transcoder (`Type#createTranscoder`). Unlike the
// object-level resolver tests, these exercise raw byte conversion: records,
// arrays, and maps are never materialized, and lossless leaf paths are
// protected by read traps that fail if a value reader is ever called.

'use strict';

var assert = require('node:assert/strict');
var test = require('node:test');
var avro = require('../lib');
var Tap = require('../lib/utils').Tap;

var Type = avro.Type;
var builtins = avro.types;

// Run `fn` with the given leaf types' value readers rigged to throw. The
// transcoder's lossless paths must never invoke them; the trap is always
// restored, even on failure.
function withReadTraps(typeNames, fn) {
  var saved = typeNames.map(function (name) {
    var proto = builtins[name].prototype;
    var original = proto._read;
    proto._read = function () {
      throw new Error('unexpected value read on trapped ' + name);
    };
    return original;
  });
  try {
    return fn();
  } finally {
    typeNames.forEach(function (name, i) {
      builtins[name].prototype._read = saved[i];
    });
  }
}

// Encode a zigzag long directly into a fresh buffer (used to build
// standalone, hand-crafted block samples).
function rawLong(n) {
  var tap = new Tap(Buffer.alloc(16));
  tap.writeLong(n);
  return tap.buf.slice(0, tap.pos);
}

// Custom long type whose in-memory representation is the raw 8-byte
// little-endian buffer, so full 64-bit patterns can be compared without any
// JavaScript number precision loss.
function rawLongType() {
  return builtins.LongType.__with({
    toBuffer: function (buf) { return buf; },
    fromBuffer: function (buf) { return Buffer.from(buf); },
    fromJSON: function (str) { return Buffer.from(str); },
    toJSON: function (buf) { return buf.toString('hex'); },
    isValid: function (buf) { return Buffer.isBuffer(buf) && buf.length === 8; },
    compare: Buffer.compare
  });
}

test('binary-record', function () {
  var writer = Type.forSchema({
    type: 'record',
    name: 'ArchEvent',
    fields: [
      {name: 'id', type: 'long'},
      {name: 'code', type: {type: 'enum', name: 'Status', symbols: ['A', 'B', 'C']}},
      {name: 'payload', type: {type: 'array', items: 'string'}},
      {name: 'blob', type: 'bytes'},
      {name: 'nm', type: 'string'},
      {name: 'magic', type: {type: 'fixed', name: 'Magic', size: 4}},
      {name: 'scratch', type: {type: 'array', items: 'int'}}
    ]
  });
  var reader = Type.forSchema({
    type: 'record',
    name: 'ArchEvent',
    fields: [
      // Reordered, aliased, defaulted, and dropped writer fields.
      {name: 'payload', type: {type: 'array', items: 'string'}},
      {name: 'id', type: 'long'},
      {name: 'name', type: 'string', aliases: ['nm']},
      {name: 'source', type: 'string', default: 'offline'},
      {name: 'tags', type: {type: 'array', items: 'string'}, default: []},
      {
        name: 'code',
        type: {
          type: 'enum',
          name: 'Status',
          symbols: ['C', 'B', 'A', 'D'],
          default: 'D'
        }
      },
      {name: 'blob', type: 'bytes'},
      {name: 'magic', type: {type: 'fixed', name: 'Magic', size: 4}}
    ]
  });

  var value = {
    id: 17,
    code: 'B',
    payload: ['a', '雪'],
    blob: Buffer.from([1, 2, 3, 250]),
    nm: 'n-1',
    magic: Buffer.from([9, 8, 7, 6]),
    scratch: [1, 2, 3] // Dropped on the reader but still fully consumed.
  };
  var input = writer.toBuffer(value);
  var plan = reader.createTranscoder(writer);

  var trapped = [
    'LongType', 'StringType', 'BytesType', 'FixedType'
  ];
  var result = withReadTraps(trapped, function () {
    return plan.transcode(input);
  });

  // The whole writer datum (including the dropped field) is consumed.
  assert.equal(result.offset, input.length);
  // The returned buffer is independent storage, never borrowed input.
  assert.notEqual(result.buffer, input);
  assert.ok(!result.buffer.equals(input)); // Layout changed (reorder/default).

  // Cross-check against the classic object-level resolver path.
  var expected = reader.fromBuffer(input, reader.createResolver(writer));
  var decoded = reader.fromBuffer(result.buffer);
  assert.deepEqual(decoded, expected);
  assert.equal(decoded.source, 'offline'); // Field default.
  assert.deepEqual(decoded.tags, []); // Nested collection default.
  assert.equal(decoded.name, 'n-1'); // Alias.
  assert.deepEqual(decoded.magic, Buffer.from([9, 8, 7, 6]));

  // A trailing datum must not be swallowed.
  var trailed = Buffer.concat([input, input]);
  var first = plan.transcode(trailed, 0);
  assert.equal(first.offset, input.length);
  var second = plan.transcode(trailed, first.offset);
  assert.equal(second.offset, trailed.length);
  assert.deepEqual(reader.fromBuffer(second.buffer), expected);

  // Full 64-bit long bit patterns (beyond the JS safe integer range) survive
  // byte-for-byte. The standard long reader cannot decode these, so both
  // sides use a raw-buffer long implementation.
  var RawLong = rawLongType();
  var w64 = Type.forSchema({
    type: 'record',
    name: 'Wide',
    fields: [
      {name: 'lo', type: RawLong},
      {name: 'note', type: 'string'}
    ]
  });
  var r64 = Type.forSchema({
    type: 'record',
    name: 'Wide',
    fields: [
      {name: 'note', type: 'string'},
      {name: 'lo', type: RawLong}
    ]
  });
  // 2^63 - 1 and -(2^63): every bit matters, neither is a safe JS integer.
  var patterns = [
    Buffer.from([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f]),
    Buffer.from([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80])
  ];
  var plan64 = r64.createTranscoder(w64);
  // Trap the custom long type's own value reader (it overrides the default
  // `LongType` reader), so a byte-copy regression would fail loudly rather
  // than be masked by an identical re-encode. The trap is scoped strictly to
  // the transcode call so the verification decode below still works.
  var longProto = Object.getPrototypeOf(RawLong);
  var savedLongRead = longProto._read;
  function trapLongRead() {
    throw new Error('unexpected value read on trapped abstract long');
  }
  patterns.forEach(function (pattern) {
    var buf64 = w64.toBuffer({lo: pattern, note: 'x'});
    var out64;
    longProto._read = trapLongRead;
    try {
      out64 = withReadTraps(['StringType'], function () {
        return plan64.transcode(buf64);
      });
    } finally {
      longProto._read = savedLongRead;
    }
    assert.equal(out64.offset, buf64.length);
    var back = r64.fromBuffer(out64.buffer);
    assert.ok(back.lo.equals(pattern), 'long bit pattern preserved');
  });

  // Recursive records still produce a finite, working plan.
  var wRec = Type.forSchema({
    type: 'record',
    name: 'Node',
    fields: [
      {name: 'v', type: 'int'},
      {name: 'next', type: ['null', 'Node']}
    ]
  });
  var rRec = Type.forSchema({
    type: 'record',
    name: 'Node',
    fields: [
      {name: 'next', type: ['Node', 'null']},
      {name: 'v', type: 'long'},
      {name: 'tag', type: 'string', default: 'd'}
    ]
  });
  var recBuf = wRec.toBuffer({v: 1, next: {v: 2, next: null}});
  var recOut = rRec.createTranscoder(wRec).transcode(recBuf);
  assert.equal(recOut.offset, recBuf.length);
  assert.equal(JSON.stringify(rRec.fromBuffer(recOut.buffer)), JSON.stringify({
    next: {next: null, v: 2, tag: 'd'},
    v: 1,
    tag: 'd'
  }));

  // Compatibility failures are rejected while building the plan.
  assert.throws(function () {
    reader.createTranscoder(Type.forSchema({
      type: 'record',
      name: 'Other',
      fields: [{name: 'id', type: 'long'}]
    }));
  });
  assert.throws(function () {
    // A default-less reader field with no writer counterpart.
    Type.forSchema({
      type: 'record',
      name: 'ArchEvent',
      fields: [
        {name: 'id', type: 'long'},
        {name: 'missing', type: 'int'}
      ]
    }).createTranscoder(writer);
  });

  // Logical types are explicitly unsupported.
  var logical = new builtins.LogicalType({type: 'long', logicalType: 'stub'});
  assert.throws(function () { reader.createTranscoder(logical); });
});

test('binary-union', function () {
  var writer = Type.forSchema(['null', 'int', 'string']);
  // Different branch order, with the int branch promoted into a long.
  var reader = Type.forSchema(['string', 'long', 'null']);
  var plan = reader.createTranscoder(writer);

  var cases = [
    {value: null, wIndex: 0, rIndex: 2},
    {value: 42, wIndex: 1, rIndex: 1},
    {value: 'hi', wIndex: 2, rIndex: 0}
  ];
  cases.forEach(function (c) {
    var input = writer.toBuffer(c.value);
    assert.equal(input[0], rawLong(c.wIndex)[0], 'writer branch index');
    var out = plan.transcode(input);
    assert.equal(out.offset, input.length);
    assert.equal(out.buffer[0], rawLong(c.rIndex)[0], 'reader branch index');
    assert.equal(reader.fromBuffer(out.buffer), writer.fromBuffer(input));
  });

  // An illegal on-wire union index throws instead of producing output.
  assert.throws(function () { plan.transcode(Buffer.from([0x08])); });
  assert.throws(function () { plan.transcode(Buffer.from([0x06])); });
  // A truncated index/datum throws as well.
  assert.throws(function () { plan.transcode(Buffer.from([0x02, 0x80])); });

  // A writer branch with no compatible reader branch fails planning.
  assert.throws(function () {
    reader.createTranscoder(Type.forSchema(['null', 'boolean']));
  });

  // Non-union reader over a union writer: the index is consumed, every
  // branch must be readable as the reader type.
  var numWriter = Type.forSchema(['int', 'long']);
  var longPlan = Type.forSchema('long').createTranscoder(numWriter);
  var branchBuf = Buffer.concat([rawLong(1), rawLong(9007199254740990)]);
  var longOut = longPlan.transcode(branchBuf);
  assert.equal(
    Type.forSchema('long').fromBuffer(longOut.buffer),
    9007199254740990
  );
  assert.equal(longOut.offset, branchBuf.length);
  assert.throws(function () {
    Type.forSchema('long').createTranscoder(Type.forSchema(['null', 'int']));
  });

  // Union reader over a non-union writer: the matching branch index is
  // prepended.
  var unionReader = Type.forSchema(['null', 'string']);
  var wrapPlan = unionReader.createTranscoder(Type.forSchema('string'));
  var wrapOut = wrapPlan.transcode(Type.forSchema('string').toBuffer('x'));
  assert.equal(wrapOut.buffer[0], rawLong(1)[0]);
  assert.equal(unionReader.fromBuffer(wrapOut.buffer), 'x');

  // Unions nested inside a reordered record.
  var wRec = Type.forSchema({
    type: 'record',
    name: 'Envelope',
    fields: [
      {name: 'choice', type: ['null', 'int', 'string']},
      {name: 'seq', type: 'long'}
    ]
  });
  var rRec = Type.forSchema({
    type: 'record',
    name: 'Envelope',
    fields: [
      {name: 'seq', type: 'long'},
      {name: 'choice', type: ['string', 'long', 'null']}
    ]
  });
  var recPlan = rRec.createTranscoder(wRec);
  ['abc', 123, null].forEach(function (choice) {
    var buf = wRec.toBuffer({choice: choice, seq: 9});
    var out = recPlan.transcode(buf);
    assert.equal(out.offset, buf.length);
    assert.equal(
      JSON.stringify(rRec.fromBuffer(out.buffer)),
      JSON.stringify({seq: 9, choice: choice})
    );
  });
});

test('binary-blocks', function () {
  // Arrays whose elements grow during transcoding: each int is a short
  // varint on input but becomes an 8-byte double on output, so input block
  // byte lengths cannot be reused on the output.
  var wArray = Type.forSchema({type: 'array', items: 'int'});
  var rArray = Type.forSchema({type: 'array', items: 'double'});
  var arrayPlan = rArray.createTranscoder(wArray);

  function encInt(n) {
    var t = new Tap(Buffer.alloc(8));
    t.writeInt(n);
    return t.buf.slice(0, t.pos);
  }

  // Hand-built negative-count block (count -2, declared byte length), with
  // two int elements, followed by a positive-count block, then the zero
  // terminator.
  var firstElems = Buffer.concat([encInt(100), encInt(200)]);
  var builder = new Tap(Buffer.alloc(64));
  builder.writeLong(-2);
  builder.writeLong(firstElems.length);
  firstElems.copy(builder.buf, builder.pos);
  builder.pos += firstElems.length;
  builder.writeLong(1); // Second, positive block.
  var third = encInt(30000);
  third.copy(builder.buf, builder.pos);
  builder.pos += third.length;
  builder.writeLong(0); // Block terminator.
  var negBlockInput = builder.buf.slice(0, builder.pos);

  var negOut = arrayPlan.transcode(negBlockInput);
  assert.equal(negOut.offset, negBlockInput.length);
  assert.deepEqual(rArray.fromBuffer(negOut.buffer), [100, 200, 30000]);
  // The output uses freshly computed positive counts, never the input's
  // negative-block marker or its stale byte length. Each output element is
  // now a full 8-byte double.
  var outTap = new Tap(negOut.buffer);
  var firstCount = outTap.readLong();
  assert.ok(firstCount > 0, 'output block count is positive');
  assert.equal(firstCount, 2);
  assert.equal(outTap.pos, 1); // No byte-length field follows a count.

  // Empty arrays encode as a lone zero block and round-trip.
  var emptyInput = wArray.toBuffer([]);
  var emptyOut = arrayPlan.transcode(emptyInput);
  assert.equal(emptyOut.offset, emptyInput.length);
  assert.deepEqual(rArray.fromBuffer(emptyOut.buffer), []);
  assert.deepEqual(Array.from(emptyOut.buffer), [0]);

  // Maps: negative block too, with int -> long value promotion.
  var wMap = Type.forSchema({type: 'map', values: 'int'});
  var rMap = Type.forSchema({type: 'map', values: 'long'});
  var mapPlan = rMap.createTranscoder(wMap);
  var mapInner = new Tap(Buffer.alloc(32));
  mapInner.writeString('k1');
  mapInner.writeInt(11);
  var mapPayload = mapInner.buf.slice(0, mapInner.pos);
  var mapBuilder = new Tap(Buffer.alloc(64));
  mapBuilder.writeLong(-1);
  mapBuilder.writeLong(mapPayload.length);
  mapPayload.copy(mapBuilder.buf, mapBuilder.pos);
  mapBuilder.pos += mapPayload.length;
  mapBuilder.writeLong(0);
  var mapInput = mapBuilder.buf.slice(0, mapBuilder.pos);
  var mapOut = mapPlan.transcode(mapInput);
  assert.equal(mapOut.offset, mapInput.length);
  assert.deepEqual(rMap.fromBuffer(mapOut.buffer), {k1: 11});

  // Null inside a collection is handled without any object materialization.
  var wNulls = Type.forSchema({type: 'array', items: ['null', 'string']});
  var rNulls = Type.forSchema({type: 'array', items: ['string', 'null']});
  var nullsBuf = wNulls.toBuffer([null, 'z', null]);
  var nullsOut = rNulls.createTranscoder(wNulls).transcode(nullsBuf);
  assert.equal(nullsOut.offset, nullsBuf.length);
  assert.deepEqual(rNulls.fromBuffer(nullsOut.buffer), [null, 'z', null]);

  // Truncated blocks and truncated element data must throw, never return a
  // half-built collection.
  assert.throws(function () { arrayPlan.transcode(negBlockInput.slice(0, 4)); });
  assert.throws(function () { mapPlan.transcode(mapInput.slice(0, 3)); });
  assert.throws(function () {
    // Positive count of 1 but no element following it.
    arrayPlan.transcode(Buffer.from([0x02]));
  });
});

test('binary-framing', function () {
  var writer = Type.forSchema({
    type: 'record',
    name: 'Frame',
    fields: [
      {name: 'id', type: 'long'},
      {name: 'payload', type: {type: 'array', items: 'string'}},
      {name: 'blob', type: 'bytes'}
    ]
  });
  var reader = Type.forSchema({
    type: 'record',
    name: 'Frame',
    fields: [
      {name: 'payload', type: {type: 'array', items: 'string'}},
      {name: 'id', type: 'long'},
      {name: 'source', type: 'string', default: 'offline'},
      {name: 'blob', type: 'bytes'}
    ]
  });
  var plan = reader.createTranscoder(writer);

  var values = [
    {id: 1, payload: [], blob: Buffer.alloc(0)},
    {id: 2, payload: ['aa', 'bb'], blob: Buffer.from([255, 0])},
    {id: 3, payload: ['ccc'], blob: Buffer.from([1, 2, 3, 4, 5])}
  ];
  var frames = values.map(function (v) { return writer.toBuffer(v); });
  var stream = Buffer.concat(frames);

  // Walk the stream using each returned absolute offset.
  var offset = 0;
  var decoded = [];
  frames.forEach(function (frame, i) {
    var out = plan.transcode(stream, offset);
    assert.equal(out.offset, offset + frame.length, 'offset after frame ' + i);
    offset = out.offset;
    decoded.push(reader.fromBuffer(out.buffer));
  });
  assert.equal(offset, stream.length);
  assert.deepEqual(decoded.map(function (v) { return v.id; }), [1, 2, 3]);
  assert.equal(decoded[1].source, 'offline');

  // Reading past the end throws rather than returning an empty datum.
  assert.throws(function () { plan.transcode(stream, stream.length); });

  // The same plan can be interleaved across independent buffers without any
  // shared state.
  var other = Type.forSchema({
    type: 'record',
    name: 'Other',
    fields: [{name: 'n', type: 'long'}]
  });
  var otherPlan = other.createTranscoder(other);
  var a = plan.transcode(frames[0], 0);
  var b = otherPlan.transcode(other.toBuffer({n: 99}), 0);
  var c = plan.transcode(frames[1], 0);
  var d = otherPlan.transcode(other.toBuffer({n: 100}), 0);
  assert.equal(reader.fromBuffer(a.buffer).id, 1);
  assert.equal(other.fromBuffer(b.buffer).n, 99);
  assert.equal(reader.fromBuffer(c.buffer).id, 2);
  assert.equal(other.fromBuffer(d.buffer).n, 100);

  // Output never borrows the input's mutable storage.
  var source = writer.toBuffer(values[1]);
  var produced = plan.transcode(source).buffer;
  var snapshot = Buffer.from(produced);
  source.fill(0); // Mutate the input after transcoding.
  assert.ok(produced.equals(snapshot), 'output unaffected by input mutation');

  // Reusing the plan repeatedly stays stable and position-independent.
  for (var i = 0; i < 50; i++) {
    var pick = i % frames.length;
    var repeated = plan.transcode(frames[pick], 0);
    assert.equal(repeated.offset, frames[pick].length);
    assert.equal(reader.fromBuffer(repeated.buffer).id, values[pick].id);
  }

  // A prefix offset is honored (the leading byte is skipped over).
  var prefixed = Buffer.concat([Buffer.from([0x77]), frames[2]]);
  var shifted = plan.transcode(prefixed, 1);
  assert.equal(shifted.offset, prefixed.length);
  assert.equal(reader.fromBuffer(shifted.buffer).id, 3);

  // Truncated length prefixes and incomplete datums are rejected.
  assert.throws(function () {
    plan.transcode(Buffer.from([0x02])); // Long cut mid-varint region.
  });
  assert.throws(function () {
    plan.transcode(frames[2].slice(0, frames[2].length - 1));
  });
  assert.throws(function () {
    plan.transcode(Buffer.alloc(0));
  });
});
