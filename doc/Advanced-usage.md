<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Type inference](#type-inference)
- [Schema evolution](#schema-evolution)
- [Logical types](#logical-types)
- [Custom long types](#custom-long-types)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Type inference

Avro requires a schema in order to be able to encode and decode values. Writing
such a schema isn't always straightforward however, especially when unfamiliar
with the syntax. `Type.forValue` aims to help by auto-generating a valid type
for any value:

```javascript
const type = avro.Type.forValue([1, 4.5, 8]);
// We can now encode or any array of floats using this type:
const buf = type.toBuffer([4, 6.1]);
const val = type.fromBuffer(buf); // [4, 6.1]
// We can also access the auto-generated schema:
const schema = type.schema();
```

For most use-cases, the resulting type should be sufficient; and in cases where
it isn't, it should hopefully provide a helpful starting point.


# Schema evolution

Schema evolution allows a type to deserialize binary data written by another
[compatible][schema-resolution] type. This is done via
[`createResolver`][create-resolver-api], and is particularly useful when we are
only interested in a subset of the fields inside a record. By selectively
decoding fields, we can significantly increase throughput.

As a motivating example, consider the following event:

```javascript
const heavyType = avro.Type.forSchema({
  name: 'Event',
  type: 'record',
  fields: [
    {name: 'time', type: 'long'},
    {name: 'userId', type: 'int'},
    {name: 'actions', type: {type: 'array', items: 'string'}},
  ]
});
```

Let's assume that we would like to compute statistics on users' actions but
only for a few user IDs. One approach would be to decode the full record each
time, but this is wasteful if very few users match our filter. We can do better
by using the following reader's schema, and creating the corresponding
resolver:

```javascript
const lightType = avro.Type.forSchema({
  name: 'LightEvent',
  aliases: ['Event'],
  type: 'record',
  fields: [
    {name: 'userId', type: 'int'},
  ]
});

const resolver = lightType.createResolver(heavyType);
```

We decode only the `userId` field, and then, if the ID matches, process the
full record. The function below implements this logic, returning a fully
decoded record if the ID matches, and `undefined` otherwise.

```javascript
function fastDecode(buf) {
  const lightRecord = lightType.fromBuffer(buf, resolver, true);
  if (lightRecord.userId % 100 === 48) { // Arbitrary check.
    return heavyType.fromBuffer(buf);
  }
}
```

In the above example, using randomly generated records, if the filter matches
roughly 1% of the time, we are able to get a **400%** throughput increase
compared to decoding the full record each time! The heavier the schema (and the
closer to the beginning of the record the used fields are), the higher this
increase will be.


# Logical types

The built-in types provided by Avro are sufficient for many use-cases, but it
can often be much more convenient to work with native JavaScript objects. As a
quick motivating example, let's imagine we have the following schema:

```javascript
const schema = {
  name: 'Transaction',
  type: 'record',
  fields: [
    {name: 'amount', type: 'int'},
    {name: 'time', type: {type: 'long', logicalType: 'timestamp-millis'}}
  ]
};
```

The `time` field encodes a timestamp as a `long`, but it would be better if we
could deserialize it directly into a native `Date` object. This is possible
using Avro's *logical types*, with the following two steps:

+ Adding a `logicalType` attribute to the type's definition (e.g.
  `'timestamp-millis'` above).
+ Implementing a corresponding [`LogicalType`][logical-type-api] and adding it
  to [`Type.forSchema`][parse-api]'s `logicalTypes`.

For example, we can use this [`DateType`
](https://gist.github.com/mtth/1aec40375fbcb077aee7#file-date-js) to
transparently deserialize/serialize native `Date` objects:

```javascript
const type = avro.Type.forSchema(
  schema,
  {logicalTypes: {'timestamp-millis': DateType}}
);

// We create a new transaction.
const transaction = {
  amount: 32,
  time: new Date('Thu Nov 05 2015 11:38:05 GMT-0800 (PST)')
};

// Our type is able to directly serialize it, including the date.
const buf = type.toBuffer(transaction);

// And we can get the date back just as easily.
const date = type.fromBuffer(buf).time; // `Date` object.
```

Logical types can also be used with schema evolution. This is done by
implementing an additional `_resolve` method. It should return a function which
converts values of the writer's type into the logical type's values. For
example, the above `DateType` can read dates which were serialized as strings:

```javascript
const str = 'Thu Nov 05 2015 11:38:05 GMT-0800 (PST)';
const stringType = avro.Type.forSchema('string');
const buf = stringType.toBuffer(str); // `str` encoded as an Avro string.

const dateType = type.field('time').type;
const resolver = dateType.createResolver(stringType);
const date = dateType.fromBuffer(buf, resolver); // Date corresponding to `str`.
```

As a more fully featured example, you can also take a look at this
[`DecimalType`](https://gist.github.com/mtth/1aec40375fbcb077aee7#file-decimal-js)
which implements the [decimal logical type][decimal-type] described in the
spec. Or, see how to use a logical type to implement a
[`MetaType`](https://gist.github.com/mtth/fb87030c8cc142730d15), the type of
all types.


# Custom long types

JavaScript represents all numbers as doubles internally, which means that it is
possible to lose precision when using very large numbers (absolute value
greater than `9e+15` or so). For example:

```javascript
Number.parseInt('9007199254740995') === 9007199254740996 // true
```

In most cases, these bounds are so large that this is not a problem (timestamps
fit nicely inside the supported precision). However it might happen that the
full range must be supported. (To avoid silently corrupting data, the default
[`LongType`](Api#class-longtypeschema-opts) will throw an error when
encountering a number outside the supported precision range.)

There are multiple JavaScript libraries to represent 64-bit integers, with
different characteristics (e.g. some are faster but do not run in the browser).
Rather than tie us to any particular one, `avsc` lets us choose the most
adequate with [`LongType.__with`](Api#longtype__withmethods-nounpack). Below
are a few sample implementations for popular libraries (refer to the API
documentation for details on each option):

+ [`node-int64`](https://www.npmjs.com/package/node-int64):

  ```javascript
  const Long = require('node-int64');

  const longType = avro.types.LongType.__with({
    fromBuffer: (buf) => { return new Long(buf); },
    toBuffer: (n) => { return n.toBuffer(); },
    fromJSON: (obj) => { return new Long(obj); },
    toJSON: (n) => { return +n; },
    isValid: (n) => { return n instanceof Long; },
    compare: (n1, n2) => { return n1.compare(n2); }
  });
  ```

+ [`int64-native`](https://www.npmjs.com/package/int64-native):

  ```javascript
  const Long = require('int64-native');

  const longType = avro.types.LongType.__with({
    fromBuffer: (buf) => { return new Long('0x' + buf.toString('hex')); },
    toBuffer: (n) => { return Buffer.from(n.toString().slice(2), 'hex'); },
    fromJSON: (obj) => { return new Long(obj); },
    toJSON: (n) => { return +n; },
    isValid: (n) => { return n instanceof Long; },
    compare: (n1, n2) => { return n1.compare(n2); }
  });
  ```

+ [`long`](https://www.npmjs.com/package/long):

  ```javascript
  const Long = require('long');

  const longType = avro.types.LongType.__with({
    fromBuffer: (buf) => {
      return new Long(buf.readInt32LE(), buf.readInt32LE(4));
    },
    toBuffer: (n) => {
      const buf = Buffer.alloc(8);
      buf.writeInt32LE(n.getLowBits());
      buf.writeInt32LE(n.getHighBits(), 4);
      return buf;
    },
    fromJSON: Long.fromValue,
    toJSON: (n) => { return +n; },
    isValid: Long.isLong,
    compare: (n1, n2) => { return n1.compare(n2); }
  });
  ```

Any such implementation can then be used in place of the default `LongType` to
provide full 64-bit support when decoding and encoding binary data. To do so,
we override the default type used for `long`s by adding our implementation to
the `registry` when parsing a schema:

```javascript
// Our schema here is very simple, but this would work for arbitrarily complex
// ones (applying to all longs inside of it).
const type = avro.Type.forSchema('long', {registry: {'long': longType}});

// Avro serialization of Number.MAX_SAFE_INTEGER + 4 (which is incorrectly
// rounded when represented as a double):
const buf = Buffer.from([0x86, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x20]);

// Assuming we are using the `node-int64` implementation.
const obj = new Long(buf);
const encoded = type.toBuffer(obj); // == buf
const decoded = type.fromBuffer(buf); // == obj (No precision loss.)
```

Because the built-in JSON parser is itself limited by JavaScript's internal
number representation, using the `toString` and `fromString` methods is
generally still unsafe (see `LongType.__with`'s documentation for a possible
workaround).

Finally, to make integration easier, `toBuffer` and `fromBuffer` deal with
already unpacked buffers by default. To leverage an external optimized packing
and unpacking routine (for example when using a native C++ addon), we can
disable this behavior by setting `LongType.__with`'s `noUnpack` argument to
`true`.


[infer-api]: API#inferval-opts
[parse-api]: API#parseschema-opts
[create-resolver-api]: API#typecreateresolverwritertype
[logical-type-api]: API#class-logicaltypeattrs-opts-types
[decimal-type]: https://avro.apache.org/docs/current/spec.html#Decimal
[schema-resolution]: https://avro.apache.org/docs/current/spec.html#Schema+Resolution
