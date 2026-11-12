/* jshint node: true */

// Binary transcoder: stream-compatible conversion between Avro encodings
// without ever materializing records, arrays, or maps as JavaScript objects.
//
// A transcoder plan is created from a reader type via `createTranscoder` and
// can be reused across buffers. Each call to `transcode` reads a single writer
// datum and emits its reader-encoding; no input storage is borrowed by the
// returned buffer and all execution state lives on the call stack, so the same
// plan can be interleaved across buffers safely.

'use strict';

var util = require('util');
var utils = require('./utils');

var Tap = utils.Tap;
var f = util.format;

// Initial size of the output buffer (doubled on overflow).
var INITIAL_SIZE = 1024;

// Sentinel thrown when an input or output tap runs past its buffer. Catching
// it lets `transcode` report a single truncation error instead of returning a
// half-written datum.
function TruncationError() {}

/**
 * Reusable binary transcoding plan.
 *
 * @param rootOp {Function} Entrypoint operation, `fn(rtap, wtap)`.
 */
function Transcoder(rootOp) {
  this._op = rootOp;
  Object.freeze(this);
}

Transcoder.prototype.transcode = function (buffer, offset) {
  if (!Buffer.isBuffer(buffer)) {
    throw new Error(f('not a buffer: %j', buffer));
  }
  var pos = offset === undefined ? 0 : offset;
  if (pos < 0 || pos > buffer.length) {
    throw new Error(f('invalid offset: %s', pos));
  }

  var out = utils.newBuffer(INITIAL_SIZE);
  var wtap = new Tap(out);
  var rtap = new Tap(buffer, pos);
  try {
    this._op(rtap, wtap);
    if (!rtap.isValid() || !wtap.isValid()) {
      throw new TruncationError();
    }
  } catch (err) {
    if (err instanceof TruncationError) {
      throw new Error('truncated datum');
    }
    throw err;
  }
  return {
    // Slice the tap's live buffer: `grow` may have replaced it with a
    // larger one, so the original `out` reference would truncate.
    buffer: wtap.buf.slice(0, wtap.pos), // Independent copy of the output.
    offset: rtap.pos
  };
};

/**
 * Build a transcoder plan converting `writerType` data into the reader type.
 *
 * Compatibility follows the same rules as `createResolver` (name and alias
 * checks are reused verbatim); any incompatibility fails here, at planning
 * time. Logical types are not supported.
 *
 * @param readerType {Type}
 * @param writerType {Type}
 */
function createPlan(readerType, writerType) {
  if (!Type.isType(readerType) || !Type.isType(writerType)) {
    throw new Error(f('not a type: %j', writerType));
  }
  if (Type.isType(readerType, 'logical') || Type.isType(writerType, 'logical')) {
    throw new Error('transcoding logical types is not supported');
  }
  // Two-level memoization table (reader type -> writer type -> op), so
  // recursive record pairs produce a finite cyclic operation graph even for
  // anonymous record types.
  var registry = new Map();
  var op = build(readerType, writerType, registry);
  return new Transcoder(op);
}

// Recursive plan builder.
function build(reader, writer, registry) {
  if (Type.isType(reader, 'union') && !Type.isType(writer, 'union')) {
    // A non-union writer datum read into a union reader: the union's
    // resolver picks the first compatible branch, and the output datum must
    // carry that branch's index.
    var rIndex = findUnionBranch(reader, writer);
    if (rIndex < 0) {
      throw incompatible(reader, writer);
    }
    var branchOp = build(reader.types[rIndex], writer, registry);
    return function (rtap, wtap) {
      writeVarint(wtap, rIndex);
      branchOp(rtap, wtap);
    };
  }

  var typeName = writer.typeName;
  switch (typeName) {
    case 'null':
      return checkSame(reader, writer, 'null', noop);
    case 'boolean':
      return checkSame(reader, writer, 'boolean', copyBoolean);
    case 'int':
      return intOp(reader, writer);
    case 'long':
    case 'abstract:long':
      // Custom long classes share the standard zigzag wire format, so they
      // are handled at the byte level without touching their JS wrappers.
      return longOp(reader, writer);
    case 'float':
      return floatOp(reader, writer);
    case 'double':
      return doubleOp(reader, writer);
    case 'string':
      return stringOp(reader, writer);
    case 'bytes':
      return bytesOp(reader, writer);
    case 'enum':
      return enumOp(reader, writer);
    case 'fixed':
      return fixedOp(reader, writer);
    case 'record':
    case 'error':
      return recordOp(reader, writer, registry);
    case 'array':
      return arrayOp(reader, writer, registry);
    case 'map':
      return mapOp(reader, writer, registry);
    case 'union:unwrapped':
    case 'union:wrapped':
      return unionOp(reader, writer, registry);
    default:
      throw new Error(f('cannot transcode %s as %s', writer, reader));
  }
}

function checkSame(reader, writer, name, op) {
  if (reader.typeName !== name) {
    throw new Error(f('cannot read %s as %s', writer, reader));
  }
  return op;
}

// Primitive leaves. The "copy" operations never call the types' value reading
// functions, so (for example) full 64-bit long bit patterns pass through even
// when they cannot be represented as JavaScript numbers.

function noop() {}

function copyBoolean(rtap, wtap) {
  ensure(rtap, 1);
  grow(wtap, 1);
  wtap.buf[wtap.pos++] = rtap.buf[rtap.pos++];
}

// Copy a variable-length zigzag integer's raw bytes, returning nothing.
function copyVarint(rtap, wtap) {
  var buf = rtap.buf;
  var pos = rtap.pos;
  var b;
  do {
    if (pos >= buf.length) {
      throw new TruncationError();
    }
    b = buf[pos++];
  } while (b & 0x80);
  writeRaw(wtap, buf, rtap.pos, pos);
  rtap.pos = pos;
}

// Read a zigzag integer as a JS number (only used for promotions, where the
// resolver rules already require the value to be representable).
function readVarint(rtap) {
  var n = 0;
  var k = 0;
  var buf = rtap.buf;
  var b, h, fv, fk;

  do {
    if (rtap.pos >= buf.length) {
      throw new TruncationError();
    }
    b = buf[rtap.pos++];
    h = b & 0x80;
    n |= (b & 0x7f) << k;
    k += 7;
  } while (h && k < 28);

  if (h) {
    fv = n;
    fk = 268435456; // 2 ** 28.
    do {
      if (rtap.pos >= buf.length) {
        throw new TruncationError();
      }
      b = buf[rtap.pos++];
      fv += (b & 0x7f) * fk;
      fk *= 128;
    } while (b & 0x80);
    return (fv % 2 ? -(fv + 1) : fv) / 2;
  }
  return (n >> 1) ^ -(n & 1);
}

function intOp(reader, writer) {
  switch (reader.typeName) {
    case 'int':
      return copyVarint;
    case 'long':
    case 'abstract:long':
      // int and long share the zigzag encoding for all representable values.
      return copyVarint;
    case 'float':
      return numberPromotion(readVarint, writeFloat);
    case 'double':
      return numberPromotion(readVarint, writeDouble);
    default:
      throw incompatible(reader, writer);
  }
}

function longOp(reader, writer) {
  switch (reader.typeName) {
    case 'long':
    case 'abstract:long':
      // Byte-for-byte copy: all 64-bit patterns survive, even beyond the
      // JS safe integer range and inside custom long wrappers. `int`
      // writers share the same zigzag wire encoding.
      return copyVarint;
    case 'float':
      // The resolver accepts this and rounds anyway; float cannot represent
      // most out-of-range longs exactly, matching `createResolver` semantics.
      return numberPromotion(readVarint, writeFloat);
    case 'double':
      return numberPromotion(readVarint, writeDouble);
    default:
      throw incompatible(reader, writer);
  }
}

function floatOp(reader, writer) {
  switch (reader.typeName) {
    case 'float':
      return copyFloatRaw;
    case 'double':
      return floatToDouble;
    default:
      throw incompatible(reader, writer);
  }
}

function doubleOp(reader, writer) {
  return checkSame(reader, writer, 'double', copyDoubleRaw);
}

function copyFloatRaw(rtap, wtap) {
  ensure(rtap, 4);
  var pos = rtap.pos;
  rtap.pos += 4;
  writeRaw(wtap, rtap.buf, pos, pos + 4);
}

function floatToDouble(rtap, wtap) {
  ensure(rtap, 4);
  var pos = rtap.pos;
  rtap.pos += 4;
  writeDouble(wtap, rtap.buf.readFloatLE(pos));
}

function copyDoubleRaw(rtap, wtap) {
  ensure(rtap, 8);
  var pos = rtap.pos;
  rtap.pos += 8;
  writeRaw(wtap, rtap.buf, pos, pos + 8);
}

function numberPromotion(read, write) {
  return function (rtap, wtap) {
    write(wtap, read(rtap));
  };
}

function writeFloat(wtap, val) {
  grow(wtap, 4);
  wtap.buf.writeFloatLE(val, wtap.pos);
  wtap.pos += 4;
}

function writeDouble(wtap, val) {
  grow(wtap, 8);
  wtap.buf.writeDoubleLE(val, wtap.pos);
  wtap.pos += 8;
}

function stringOp(reader, writer) {
  // The resolver's `StringType._update` (also reused by `BytesType`) accepts
  // a string writer into either a string or bytes reader. The encodings are
  // identical length-prefixed payloads, so bytes are moved untouched.
  if (reader.typeName === 'string' || reader.typeName === 'bytes') {
    return copyBytesLeaf;
  }
  throw incompatible(reader, writer);
}

function bytesOp(reader, writer) {
  // A bytes writer is resolvable to a string or bytes reader (both share the
  // same length-prefixed wire layout).
  if (reader.typeName === 'bytes' || reader.typeName === 'string') {
    return copyBytesLeaf;
  }
  throw incompatible(reader, writer);
}

// Shared by strings and bytes: copy the length prefix and payload without
// touching either type's read function.
function copyBytesLeaf(rtap, wtap) {
  var len = readVarint(rtap);
  if (len < 0) {
    // Mirrors `Tap#readBytes`: a negative length is an invalid datum rather
    // than silent truncation.
    throw new Error('negative length');
  }
  ensure(rtap, len);
  writeVarint(wtap, len);
  writeRaw(wtap, rtap.buf, rtap.pos, rtap.pos + len);
  rtap.pos += len;
}

function fixedOp(reader, writer) {
  // The resolver only accepts an equal-size, name-compatible fixed type;
  // unlike the Avro spec it does not promote fixed to bytes.
  if (
    reader.typeName === 'fixed' &&
    reader.size === writer.size &&
    hasCompatibleName(reader, writer, true)
  ) {
    var size = writer.size;
    return function (rtap, wtap) {
      ensure(rtap, size);
      writeRaw(wtap, rtap.buf, rtap.pos, rtap.pos + size);
      rtap.pos += size;
    };
  }
  throw incompatible(reader, writer);
}

function enumOp(reader, writer) {
  if (
    reader.typeName === 'enum' &&
    hasCompatibleName(reader, writer, true)
  ) {
    var allKnown = writer.symbols.every(function (s) {
      return ~reader.symbols.indexOf(s);
    });
    if (!allKnown && reader.default === undefined) {
      throw incompatible(reader, writer);
    }
    // Map each writer symbol position to the corresponding reader position,
    // falling back to the reader's default symbol (the same rule as
    // `EnumType#_update`).
    var defaultIndex = reader._indices[reader.default];
    var mapping = writer.symbols.map(function (s) {
      var i = reader._indices[s];
      return i === undefined ? defaultIndex : i;
    });
    return function (rtap, wtap) {
      var index = readVarint(rtap);
      if (index < 0 || index >= mapping.length) {
        throw new Error(f('invalid %s enum index: %s', writer.name, index));
      }
      writeVarint(wtap, mapping[index]);
    };
  }
  // The resolver does not promote enums to strings; any other reader type is
  // incompatible.
  throw incompatible(reader, writer);
}

function recordOp(reader, writer, registry) {
  if (
    !Type.isType(reader, 'record', 'error') ||
    !hasCompatibleName(reader, writer, true)
  ) {
    throw incompatible(reader, writer);
  }

  var byWriter = registry.get(reader);
  if (!byWriter) {
    byWriter = new Map();
    registry.set(reader, byWriter);
  }
  if (byWriter.has(writer)) {
    // Recursive record pair: reuse the op registered before descending into
    // the fields (its closure is populated by the time it runs).
    return byWriter.get(writer);
  }

  var rFields = reader.fields;
  var wFields = writer.fields;
  var wFieldsMap = utils.toMap(wFields, function (field) {
    return field.name;
  });

  // Closures populated below; the op is registered in the memo before any
  // field type is built, so recursive references resolve back to it.
  var writerSteps, defaultBuffers, rCount;
  var op = function (rtap, wtap) {
    // Writer fields are consumed (and transcoded) in writer order into a
    // scratch buffer, then reassembled in reader field order so that
    // reorderings, aliases, and defaults land where the reader record
    // expects them. The scratch is local to this call: plans keep no
    // cross-call state.
    var stap = new Tap(utils.newBuffer(256));
    var ranges = new Array(rCount);
    for (var k = 0; k < writerSteps.length; k++) {
      var step = writerSteps[k];
      if (step.skip) {
        skipValue(step.skip, rtap);
      } else {
        var targets = step.targets;
        var start = rtap.pos;
        for (var m = 0; m < targets.length; m++) {
          var target = targets[m];
          var s0 = stap.pos;
          target.op(rtap, stap);
          ranges[target.index] = [s0, stap.pos];
          if (m < targets.length - 1) {
            // The same writer datum feeds several reader fields: rewind and
            // encode it again for the next target.
            rtap.pos = start;
          }
        }
      }
    }
    if (!stap.isValid()) {
      throw new TruncationError();
    }
    for (var index = 0; index < rCount; index++) {
      var range = ranges[index];
      if (range) {
        writeRaw(wtap, stap.buf, range[0], range[1]);
      } else {
        var buf = defaultBuffers[index];
        writeRaw(wtap, buf, 0, buf.length);
      }
    }
  };
  byWriter.set(writer, op);
  rCount = rFields.length;

  // For each reader field, record the writer field name it comes from, or
  // `null` when it is filled from its default.
  var readerSource = new Array(rFields.length);
  var i, j, field, names, matches;
  for (i = 0; i < rFields.length; i++) {
    field = rFields[i];
    names = getAliases(field);
    matches = [];
    for (j = 0; j < names.length; j++) {
      if (wFieldsMap[names[j]]) {
        matches.push(names[j]);
      }
    }
    if (matches.length > 1) {
      throw new Error(
        f('ambiguous aliasing for %s.%s (%s)', writer.name, field.name, matches)
      );
    }
    if (!matches.length) {
      if (field.defaultValue() === undefined) {
        throw new Error(
          f('no matching field for default-less %s.%s', writer.name, field.name)
        );
      }
      readerSource[i] = null;
    } else {
      readerSource[i] = matches[0];
    }
  }

  // Precomputed default encodings for reader fields absent from the writer;
  // `null` marks a field whose data comes from the writer.
  defaultBuffers = rFields.map(function (field, index) {
    return readerSource[index] === null ?
      field.type.toBuffer(field.defaultValue()) :
      null;
  });

  // Plan the writer fields in their own order: either skip (fully consume)
  // them, or render each one into the scratch buffer for every reader field
  // aliased to it. Recursive field types find `op` already in the memo.
  writerSteps = wFields.map(function (wField) {
    var targets = [];
    for (var index = 0; index < rFields.length; index++) {
      if (readerSource[index] === wField.name) {
        targets.push({
          index: index,
          op: build(rFields[index].type, wField.type, registry)
        });
      }
    }
    return targets.length ? {targets: targets} : {skip: wField.type};
  });

  return op;
}

function arrayOp(reader, writer, registry) {
  if (reader.typeName !== 'array') {
    throw incompatible(reader, writer);
  }
  var itemOp = build(reader.itemsType, writer.itemsType, registry);
  return function (rtap, wtap) {
    transcodeBlocks(rtap, wtap, itemOp, false);
  };
}

function mapOp(reader, writer, registry) {
  if (reader.typeName !== 'map') {
    throw incompatible(reader, writer);
  }
  var valueOp = build(reader.valuesType, writer.valuesType, registry);
  return function (rtap, wtap) {
    transcodeBlocks(rtap, wtap, valueOp, true);
  };
}

// Shared array/map block handling. Both positive-count and negative-count
// (with byte length) input blocks are accepted; output always uses positive
// counts, recomputed from the transcoded elements so changed element lengths
// are reflected.
function transcodeBlocks(rtap, wtap, elementOp, isMap) {
  var count;
  while ((count = readBlockHeader(rtap)) !== 0) {
    writeVarint(wtap, count);
    for (var i = 0; i < count; i++) {
      if (isMap) {
        // Map keys are strings: move length + UTF-8 payload untouched.
        copyBytesLeaf(rtap, wtap);
      }
      elementOp(rtap, wtap);
    }
  }
  writeVarint(wtap, 0); // Terminal zero block.
}

// Read a block count, consuming the optional byte length on negative blocks.
// The declared byte length is only validated, never reused: element sizes may
// change during transcoding.
function readBlockHeader(rtap) {
  var n = readVarint(rtap);
  if (n < 0) {
    var byteLen = readVarint(rtap);
    if (byteLen < 0) {
      throw new Error('negative block size');
    }
    return -n;
  }
  return n;
}

function unionOp(reader, writer, registry) {
  if (Type.isType(reader, 'union')) {
    // Every writer branch must resolve to exactly one reader branch; we
    // reuse the reader union's own compatibility search rather than
    // relaxing its rules, and fail planning if no branch matches.
    var branchOps = writer.types.map(function (wType, wIndex) {
      var rIndex = findUnionBranch(reader, wType);
      if (rIndex < 0) {
        throw new Error(
          f('cannot read union branch %s (%s) as %s', wIndex, wType, reader)
        );
      }
      return {
        index: rIndex,
        op: build(reader.types[rIndex], wType, registry)
      };
    });
    var numBranches = branchOps.length;
    return function (rtap, wtap) {
      var wIndex = readVarint(rtap);
      if (wIndex < 0 || wIndex >= numBranches) {
        throw new Error(f('invalid union index: %s', wIndex));
      }
      var branch = branchOps[wIndex];
      writeVarint(wtap, branch.index);
      branch.op(rtap, wtap);
    };
  }

  // Non-union reader over a union writer: the resolver requires every
  // writer branch to be readable as the reader type; the union index is
  // consumed but not re-emitted.
  var ops = writer.types.map(function (wType) {
    reader.createResolver(wType); // Throws on an incompatible branch.
    return build(reader, wType, registry);
  });
  var branchCount = ops.length;
  return function (rtap, wtap) {
    var index = readVarint(rtap);
    if (index < 0 || index >= branchCount) {
      throw new Error(f('invalid union index: %s', index));
    }
    ops[index](rtap, wtap);
  };
}

// Mirror `UnionType#_update`'s branch search: the first reader branch whose
// resolver accepts the writer type wins. We never catch errors from nested
// `build` calls: an incompatibility inside an otherwise matching branch must
// fail planning, not silently select another branch.
function findUnionBranch(reader, writerType) {
  var types = reader.types;
  for (var i = 0, l = types.length; i < l; i++) {
    try {
      types[i].createResolver(writerType);
    } catch (err) {
      continue;
    }
    return i;
  }
  return -1;
}

// Skip an entire datum without decoding it into an object. Used for writer
// record fields absent from the reader record.
function skipValue(type, rtap) {
  var typeName = type.typeName;
  switch (typeName) {
    case 'null':
      return;
    case 'boolean':
      ensure(rtap, 1);
      rtap.pos++;
      return;
    case 'int':
    case 'long':
    case 'abstract:long':
      readVarint(rtap);
      return;
    case 'float':
      ensure(rtap, 4);
      rtap.pos += 4;
      return;
    case 'double':
      ensure(rtap, 8);
      rtap.pos += 8;
      return;
    case 'enum':
      readVarint(rtap);
      return;
    case 'fixed':
      ensure(rtap, type.size);
      rtap.pos += type.size;
      return;
    case 'string':
    case 'bytes':
      var len = readVarint(rtap);
      if (len < 0) {
        throw new Error('negative length');
      }
      ensure(rtap, len);
      rtap.pos += len;
      return;
    case 'record':
    case 'error':
      var fields = type.fields;
      for (var i = 0, l = fields.length; i < l; i++) {
        skipValue(fields[i].type, rtap);
      }
      return;
    case 'array':
      skipBlocks(rtap, type.itemsType, false);
      return;
    case 'map':
      skipBlocks(rtap, type.valuesType, true);
      return;
    case 'union:unwrapped':
    case 'union:wrapped':
      var index = readVarint(rtap);
      var branch = type.types[index];
      if (!branch) {
        throw new Error(f('invalid union index: %s', index));
      }
      skipValue(branch, rtap);
      return;
    default:
      throw new Error(f('cannot skip type: %s', type));
  }
}

function skipBlocks(rtap, valueType, isMap) {
  var n;
  while ((n = readBlockHeader(rtap)) !== 0) {
    for (var i = 0; i < n; i++) {
      if (isMap) {
        var keyLen = readVarint(rtap);
        if (keyLen < 0) {
          throw new Error('negative length');
        }
        ensure(rtap, keyLen);
        rtap.pos += keyLen;
      }
      skipValue(valueType, rtap);
    }
  }
}

// --- Low-level tap helpers ---

function ensure(rtap, len) {
  if (rtap.pos + len > rtap.buf.length) {
    throw new TruncationError();
  }
}

function grow(wtap, len) {
  var needed = wtap.pos + len;
  if (needed <= wtap.buf.length) {
    return;
  }
  var size = wtap.buf.length;
  while (size < needed) {
    size *= 2;
  }
  var buf = utils.newBuffer(size);
  wtap.buf.copy(buf, 0, 0, wtap.pos);
  wtap.buf = buf;
}

function writeRaw(wtap, src, start, end) {
  var len = end - start;
  grow(wtap, len);
  src.copy(wtap.buf, wtap.pos, start, end);
  wtap.pos += len;
}

function writeVarint(wtap, n) {
  grow(wtap, 10); // Longest possible zigzag varint.
  var buf = wtap.buf;
  var fv, m;
  if (n >= -1073741824 && n < 1073741824) {
    m = n >= 0 ? n << 1 : (~n << 1) | 1;
    do {
      buf[wtap.pos] = m & 0x7f;
      m >>= 7;
    } while (m && (buf[wtap.pos++] |= 0x80));
  } else {
    fv = n >= 0 ? n * 2 : (-n * 2) - 1;
    do {
      buf[wtap.pos] = fv & 0x7f;
      fv /= 128;
    } while (fv >= 1 && (buf[wtap.pos++] |= 0x80));
  }
  wtap.pos++;
}

function incompatible(reader, writer) {
  return new Error(f('cannot read %s as %s', writer, reader));
}

// Local copies of the name/alias helpers in `types.js`, kept in sync with
// `createResolver`'s rules (transcoder planning never loosens them).

function getAliases(obj) {
  var names = {};
  if (obj.name) {
    names[obj.name] = true;
  }
  var aliases = obj.aliases || [];
  for (var i = 0, l = aliases.length; i < l; i++) {
    names[aliases[i]] = true;
  }
  return Object.keys(names);
}

function hasCompatibleName(reader, writer, strict) {
  if (!writer.name) {
    return true;
  }
  var name = strict ? writer.name : utils.unqualify(writer.name);
  var aliases = getAliases(reader);
  for (var i = 0, l = aliases.length; i < l; i++) {
    var alias = strict ? aliases[i] : utils.unqualify(aliases[i]);
    if (alias === name) {
      return true;
    }
  }
  return false;
}

// Late import to avoid a circular require during module initialization.
var Type = require('./types').Type;

module.exports = {
  Transcoder: Transcoder,
  createPlan: createPlan
};
