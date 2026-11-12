<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->

- [Overview](#overview)
- [Creating a transcoder](#creating-a-transcoder)
- [Converting a stream of records](#converting-a-stream-of-records)
- [What is preserved](#what-is-preserved)
- [Compatibility and limitations](#compatibility-and-limitations)
- [Errors](#errors)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Overview

The usual way to read data written under one Avro schema with a different
(reader) schema is `Type#createResolver`, which decodes each datum into a
JavaScript object. That is convenient, but it has two costs for bulk archive
migration:

+ Fields the reader does not care about are still decoded into objects, and
  large `string`, `bytes`, or nested collection fields are fully expanded.
+ A `long` whose value falls outside JavaScript's safe integer range cannot be
  represented exactly as a number, so its bit pattern cannot be carried across
  a decode/re-encode round trip.

`Type#createTranscoder` provides a lower-level alternative. It returns a
reusable *plan* that converts a datum's binary encoding directly into the
reader schema's binary encoding, without ever materializing a record, array,
or map as an object. The object-level resolver and encoder are unchanged and
remain available.

# Creating a transcoder

Call `createTranscoder` on the **reader** type, passing the **writer** type.
Compatibility is checked once, when the plan is created; an incompatible
writer (or a logical type) throws immediately rather than failing later on
data:

```javascript
const avro = require('avsc');

const writerType = avro.Type.forSchema({
  type: 'record',
  name: 'Event',
  fields: [
    {name: 'id', type: 'long'},
    {name: 'payload', type: {type: 'array', items: 'string'}},
    {name: 'blob', type: 'bytes'} // Present on the wire, dropped by the reader.
  ]
});

const readerType = avro.Type.forSchema({
  type: 'record',
  name: 'Event',
  fields: [
    {name: 'payload', type: {type: 'array', items: 'string'}}, // Reordered.
    {name: 'id', type: 'long'},
    {name: 'source', type: 'string', default: 'offline'}      // New field.
  ]
});

const transcoder = readerType.createTranscoder(writerType);
```

A plan is stateless across calls and can be reused for every datum. Each call
to `transcode(buffer, offset = 0)` reads exactly one writer datum and returns:

+ `buffer`: a freshly allocated `Buffer` holding one record encoded with the
  reader schema. It never borrows storage from the input, so mutating the input
  afterwards cannot affect a previously returned buffer.
+ `offset`: the absolute position in the input just past the consumed datum,
  ready to feed back in for the next record.

```javascript
const input = writerType.toBuffer({
  id: 17,
  payload: ['a', '雪'],
  blob: Buffer.from([1, 2, 3])
});

const {buffer, offset} = transcoder.transcode(input);
// `offset === input.length`: the whole writer datum (including the dropped
// `blob` field) was consumed.
const record = readerType.fromBuffer(buffer);
// record.payload === ['a', '雪']
// record.id === 17
// record.source === 'offline' (the field default)
```

# Converting a stream of records

Because `offset` is absolute and a call never reads past the current datum, a
buffer holding several concatenated records can be walked one record at a
time. The second record is never swallowed:

```javascript
// `stream` holds back-to-back writer-encoded records.
let pos = 0;
const converted = [];
while (pos < stream.length) {
  const {buffer, offset} = transcoder.transcode(stream, pos);
  converted.push(buffer);
  pos = offset; // Advance to the start of the next datum.
}
// `pos === stream.length` once every record has been processed.
```

The same plan can also be interleaved across different input buffers (for
example records from multiple archive shards); no state is shared between
calls.

# What is preserved

When the writer and reader types do not require a conversion, the
transcoder moves raw bytes instead of calling the type's value reader:

+ `string`, `bytes`, and `fixed` are copied length prefix and payload together,
  without UTF-8 decoding or allocating an intermediate `Buffer` per value.
+ `long` (including custom long types from `LongType.__with`) copies the
  zigzag varint verbatim, so every 64-bit pattern survives exactly, even values
  beyond `Number.MAX_SAFE_INTEGER` that the default `long` reader rejects with
  a precision-loss error.
+ Same-type `float`, `double`, and `boolean` leaves are copied byte for byte.

Conversions that the resolver accepts are still applied where required:
numeric promotions (`int`→`long`→`float`→`double`), `enum` symbol remapping
(with the reader's default for unknown symbols), the `string`/`bytes`
interconversion, record field reordering and aliasing, and union branch
re-indexing. Equal-size, name-compatible `fixed` types are copied as raw
bytes. The accepted conversions are exactly those of `createResolver`; for
example this resolver does not promote `fixed`→`bytes` or `enum`→`string`,
and neither does the transcoder.

Array and map blocks accept both encodings on input: a positive element count,
or a negative count followed by a byte length. The byte length is only used to
locate the block; output block counts are always recomputed, so elements whose
encoding grows or shrinks during conversion still produce valid blocks.

# Compatibility and limitations

+ Compatibility rules are the same as `createResolver`, including record and
  field aliases, ambiguous-alias rejection, and name matching. Nothing is
  loosened for the transcoder.
+ Records support field reordering, reader field aliases, removed fields
  (their bytes are still fully consumed), and field defaults (including
  defaults containing nested collections).
+ Recursive records produce a finite, reusable plan.
+ Only the built-in Avro types are supported. Encountering a logical type on
  either the reader or writer side fails when the plan is created.
+ Union handling follows the writer's on-wire branch index, which is rewritten
  to the matching reader branch index; the two unions may list their branches
  in different orders. If any writer branch has no compatible reader branch,
  creating the plan throws.

# Errors

A plan never returns a half-written datum. During `transcode`:

+ a truncated length prefix or an incomplete datum,
+ an illegal union branch index,
+ or an out-of-range enum index

all throw an error, and no partial result is returned. Planning-time
incompatibilities (unknown names, missing default-less fields, unsupported
logical types, unresolvable union branches) throw when `createTranscoder` is
called, before any data is processed.
