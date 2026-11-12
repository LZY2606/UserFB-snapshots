<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Types and schemas](#types-and-schemas)
  - [`readSchema(spec)`](#readschemaspec)
  - [Class `Type`](#class-type)
    - [`Type.forSchema(schema, [opts])`](#typeforschemaschema-opts)
    - [`Type.forTypes(types, [opts])`](#typefortypestypes-opts)
    - [`Type.forValue(val, [opts])`](#typeforvalueval-opts)
    - [`Type.isType(any, [prefix...])`](#typeistypeany-prefix)
    - [`Type.__reset(size)`](#type__resetsize)
    - [`type.aliases`](#typealiases)
    - [`type.branchName`](#typebranchname)
    - [`type.clone(val, [opts])`](#typecloneval-opts)
    - [`type.compare(val1, val2)`](#typecompareval1-val2)
    - [`type.compareBuffers(buf1, buf2)`](#typecomparebuffersbuf1-buf2)
    - [`type.createResolver(writerType)`](#typecreateresolverwritertype)
    - [`type.decode(buf, [pos,] [resolver])`](#typedecodebuf-pos-resolver)
    - [`type.doc`](#typedoc)
    - [`type.encode(val, buf, [pos])`](#typeencodeval-buf-pos)
    - [`type.equals(any)`](#typeequalsany)
    - [`type.fingerprint([algorithm])`](#typefingerprintalgorithm)
    - [`type.fromBuffer(buf, [resolver,] [noCheck])`](#typefrombufferbuf-resolver-nocheck)
    - [`type.fromString(str)`](#typefromstringstr)
    - [`type.isValid(val, [opts])`](#typeisvalidval-opts)
    - [`type.name`](#typename)
    - [`type.random()`](#typerandom)
    - [`type.schema([opts])`](#typeschemaopts)
    - [`type.toBuffer(val)`](#typetobufferval)
    - [`type.toString([val])`](#typetostringval)
    - [`type.typeName`](#typetypename)
    - [`type.wrap(val)`](#typewrapval)
  - [Class `ArrayType(schema, [opts])`](#class-arraytypeschema-opts)
    - [`type.itemsType`](#typeitemstype)
  - [Class `EnumType(schema, [opts])`](#class-enumtypeschema-opts)
    - [`type.symbols`](#typesymbols)
  - [Class `FixedType(schema, [opts])`](#class-fixedtypeschema-opts)
    - [`type.size`](#typesize)
  - [Class `LogicalType(schema, [opts])`](#class-logicaltypeschema-opts)
    - [`type.underlyingType`](#typeunderlyingtype)
    - [`type._export(schema)`](#type_exportschema)
    - [`type._fromValue(val)`](#type_fromvalueval)
    - [`type._resolve(type)`](#type_resolvetype)
    - [`type._toValue(any)`](#type_tovalueany)
  - [Class `LongType(schema, [opts])`](#class-longtypeschema-opts)
    - [`LongType.__with(methods, [noUnpack])`](#longtype__withmethods-nounpack)
  - [Class `MapType(schema, [opts])`](#class-maptypeschema-opts)
    - [`type.valuesType`](#typevaluestype)
  - [Class `RecordType(schema, [opts])`](#class-recordtypeschema-opts)
    - [`type.field(name)`](#typefieldname)
      - [`class Field`](#class-field)
        - [`field.aliases`](#fieldaliases)
        - [`field.defaultValue()`](#fielddefaultvalue)
        - [`field.name`](#fieldname)
        - [`field.order`](#fieldorder)
        - [`field.type`](#fieldtype)
    - [`type.fields`](#typefields)
    - [`type.recordConstructor`](#typerecordconstructor)
      - [Class `Record(...)`](#class-record)
        - [`Record.type`](#recordtype)
        - [`record.clone([opts])`](#recordcloneopts)
        - [`record.compare(val)`](#recordcompareval)
        - [`record.isValid([opts])`](#recordisvalidopts)
        - [`record.toBuffer()`](#recordtobuffer)
        - [`record.toString()`](#recordtostring)
        - [`record.wrapped()`](#recordwrapped)
  - [Class `UnwrappedUnionType(schema, [opts])`](#class-unwrappeduniontypeschema-opts)
    - [`type.types`](#typetypes)
  - [Class `WrappedUnionType(schema, [opts])`](#class-wrappeduniontypeschema-opts)
    - [`type.types`](#typetypes-1)
- [Files and streams](#files-and-streams)
  - [`createBlobDecoder(blob, [opts])`](#createblobdecoderblob-opts)
  - [`createBlobEncoder(schema, [opts])`](#createblobencoderschema-opts)
  - [`createFileDecoder(path, [opts])`](#createfiledecoderpath-opts)
  - [`createFileEncoder(path, schema, [opts])`](#createfileencoderpath-schema-opts)
  - [`extractFileHeader(path, [opts])`](#extractfileheaderpath-opts)
  - [Class `BlockDecoder([opts])`](#class-blockdecoderopts)
    - [Event `'metadata'`](#event-metadata)
    - [Event `'data'`](#event-data)
    - [`BlockDecoder.getDefaultCodecs()`](#blockdecodergetdefaultcodecs)
  - [Class `BlockEncoder(schema, [opts])`](#class-blockencoderschema-opts)
    - [Event `'data'`](#event-data-1)
    - [`BlockEncoder.getDefaultCodecs()`](#blockencodergetdefaultcodecs)
  - [Class `RawDecoder(schema, [opts])`](#class-rawdecoderschema-opts)
    - [Event `'data'`](#event-data-2)
  - [Class `RawEncoder(schema, [opts])`](#class-rawencoderschema-opts)
    - [Event `'data'`](#event-data-3)
- [IPC & RPC](#ipc--rpc)
  - [`assembleProtocol(path, [opts,] cb)`](#assembleprotocolpath-opts-cb)
  - [`discoverProtocol(transport, [opts,] cb)`](#discoverprotocoltransport-opts-cb)
  - [`readProtocol(spec, [opts])`](#readprotocolspec-opts)
  - [Class `Service`](#class-service)
    - [`Service.forProtocol(protocol, [opts])`](#serviceforprotocolprotocol-opts)
    - [`service.createClient([opts])`](#servicecreateclientopts)
    - [`service.createServer([opts])`](#servicecreateserveropts)
    - [`service.doc`](#servicedoc)
    - [`service.hash`](#servicehash)
    - [`service.message(name)`](#servicemessagename)
      - [`class Message`](#class-message)
        - [`message.doc`](#messagedoc)
        - [`message.errorType`](#messageerrortype)
        - [`message.name`](#messagename)
        - [`message.oneWay`](#messageoneway)
        - [`message.requestType`](#messagerequesttype)
        - [`message.responseType`](#messageresponsetype)
        - [`message.schema([opts])`](#messageschemaopts)
    - [`service.messages`](#servicemessages)
    - [`service.name`](#servicename)
    - [`service.protocol`](#serviceprotocol)
    - [`service.type(name)`](#servicetypename)
    - [`service.types`](#servicetypes)
  - [Class `Client`](#class-client)
    - [Event `'channel'`](#event-channel)
    - [`client.activeChannels()`](#clientactivechannels)
    - [`client.createChannel(transport, [opts])`](#clientcreatechanneltransport-opts)
    - [`client.destroyChannels([opts])`](#clientdestroychannelsopts)
    - [`client.emitMessage(name, req, [opts,] [cb])`](#clientemitmessagename-req-opts-cb)
    - [`client.remoteProtocols()`](#clientremoteprotocols)
    - [`client.service`](#clientservice)
    - [`client.use(middleware...)`](#clientusemiddleware)
      - [Class `WrappedRequest`](#class-wrappedrequest)
        - [`headers`](#headers)
        - [`request`](#request)
      - [Class `WrappedResponse`](#class-wrappedresponse)
        - [`error`](#error)
        - [`headers`](#headers-1)
        - [`response`](#response)
      - [Class `CallContext`](#class-callcontext)
        - [`channel`](#channel)
        - [`locals`](#locals)
        - [`message`](#message)
  - [Class `Server`](#class-server)
    - [Event `'channel'`](#event-channel-1)
    - [`server.activeChannels()`](#serveractivechannels)
    - [`server.createChannel(transport, [opts])`](#servercreatechanneltransport-opts)
    - [`server.onMessage(name, handler)`](#serveronmessagename-handler)
    - [`server.remoteProtocols()`](#serverremoteprotocols)
    - [`server.service`](#serverservice)
    - [`server.use(middleware...)`](#serverusemiddleware)
  - [Class `ClientChannel`](#class-clientchannel)
    - [Event `'eot'`](#event-eot)
    - [Event `'handshake'`](#event-handshake)
    - [Event `'outgoingCall'`](#event-outgoingcall)
    - [`channel.destroy([noWait])`](#channeldestroynowait)
    - [`channel.client`](#channelclient)
    - [`channel.destroyed`](#channeldestroyed)
    - [`channel.draining`](#channeldraining)
    - [`channel.pending`](#channelpending)
    - [`channel.ping([timeout,] [cb])`](#channelpingtimeout-cb)
    - [`channel.timeout`](#channeltimeout)
  - [Class `ServerChannel`](#class-serverchannel)
    - [Event `'eot'`](#event-eot-1)
    - [Event `'handshake'`](#event-handshake-1)
    - [Event `'incomingCall'`](#event-incomingcall)
    - [`channel.destroy([noWait])`](#channeldestroynowait-1)
    - [`channel.destroyed`](#channeldestroyed-1)
    - [`channel.draining`](#channeldraining-1)
    - [`channel.pending`](#channelpending-1)
    - [`channel.server`](#channelserver)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Types and schemas

## `readSchema(spec)`

+ `spec` {String} _Type_ IDL specification.

Convenience method to generate a schema from a standalone type's IDL
specification. The spec must contain a single type definition, for example:

```javascript
const schema = parseTypeSchema(`record Header { long id; string name; }`);
const type = Type.forSchema(schema);
type.isValid({id: 123, name: 'abc'}); // true.
```

## Class `Type`

"Abstract" base Avro type class; all implementations inherit from it. It
shouldn't be instantiate directly, but rather through one of the following
factory methods described below.

### `Type.forSchema(schema, [opts])`

+ `schema` {Object|String} Decoded schema. This schema can be a string if it is
  a reference to a primitive type (e.g. `'int'`, ), or a reference to a type in
  the registry (see `opts` below).
+ `opts` {Object} Parsing options. The following keys are currently supported:
  + `assertLogicalTypes` {Boolean} The Avro specification mandates that we fall
    through to the underlying type if a logical type is invalid. When set, this
    option will override this behavior and throw an error when a logical type
    can't be applied.
  + `logicalTypes` {Object} Optional dictionary of
    [`LogicalType`](#class-logicaltypeattrs-opts-types). This can be used to
    support serialization and deserialization of arbitrary native objects.
  + `namespace` {String} Optional parent namespace.
  + `noAnonymousTypes` {Boolean} Throw an error if a named type (`enum`,
    `fixed`, `record`, or `error`) is missing its `name` field. By default
    anonymous types are supported; they behave exactly like their named
    equivalent except that they cannot be referenced, can be resolved by any
    compatible type, and use the type's `typeName` as union branch.
  + `registry` {Object} Registry of predefined type names. This can for example
    be used to override the types used for primitives or to split a schema
    declaration over multiple files.
  + `typeHook(schema, opts)` {Function} Function called before each type
    declaration or reference is parsed. The relevant decoded schema is
    available as first argument and the parsing options as second. This
    function can optionally return a type which will then be used in place of
    the result of parsing `schema`. Using this option, it is possible to
    customize the parsing process by intercepting the creation of any type.
    Here are a few examples of what is possible using a custom hook:
    + [Representing `enum`s as integers rather than strings.](https://gist.github.com/mtth/c0088c745de048c4e466#file-long-enum-js)
    + [Obfuscating all names inside a schema.](https://gist.github.com/mtth/c0088c745de048c4e466#file-obfuscate-js)
    + [Inlining fields to implement basic inheritance between records.](https://gist.github.com/mtth/c0088c745de048c4e466#file-inline-js)
  + `wrapUnions` {String|Boolean} Control whether unions should be represented
    using a [`WrappedUnionType`](#class-wrappeduniontypeattrs-opts) or an
    [`UnwrappedUnionType`](#class-unwrappeduniontypeattrs-opts). By default,
    the "natural" unwrapped alternative will be used if possible, falling back
    to wrapping if the former would lead to ambiguities. Possible values for
    this option are: `'auto'` (the default); `'always'` or `true` (always wrap
    unions); `'never'` or `false` (never wrap unions, an error will be thrown
    if an ambiguous union is parsed in this case).

Instantiate a type for its schema.

### `Type.forTypes(types, [opts])`

+ `types` {Array} Array of types to combine.
+ `opts` {Object} All the options of `Type.forSchema` are available, as well
  as:
  + `strictDefaults` {Boolean} When combining records with missing fields, the
    default behavior is to make such fields optional (wrapping their type
    inside a nullable union and setting their default to `null`). Activating
    this flag will instead combine the records into a map.

Merge multiple types into one. The resulting type will support all the input
types' values.

### `Type.forValue(val, [opts])`

+ `val` {Any} Value to generate the type for.
+ `opts` {Object} All of `Type.forTypes`' options are supported, along with:
  + `emptyArrayType` {Type} Temporary type used when an empty array is
    encountered. It will be discarded as soon as the array's type can be
    inferred. Defaults to `null`'s type.
  + `valueHook(val, opts)` Function called each time a type needs to be
    inferred from a value. This function should either return an alternate type
    to use, or `undefined` to proceed with the default inference logic.

Infer a type from a value.

### `Type.isType(any, [prefix...])`

+ `any` {...} Any object.
+ `prefix` {String} If specified, this function will only return `true` if
  the type's type name starts with at least one of these prefixes. For example,
  `Type.isType(type, 'union', 'int')` will return `true` if and only if `type`
  is either a union type or integer type.

Check whether `any` is an instance of `Type`. This is similar to `any
instanceof Type` but will work across contexts (e.g. `iframe`s).

### `Type.__reset(size)`

+ `size` {Number} New buffer size in bytes.

This method resizes the internal buffer used to encode all types. You can call
this method if you are encoding very large values and need to reclaim memory.
In some cases, it can also be beneficial to call this method at startup with a
sufficiently large buffer size to allow the JavaScript engine to better
optimize encoding.

### `type.aliases`

Returns a list of aliases for named types and `undefined` for others. Note that
it is possible to modify this list to add and remove aliases after the type is
created (altering which types can be resolved via `type.createResolver`).

### `type.branchName`

If `type` doesn't have a name, return its "type name" instead of `undefined`.
(This method then returns the type's branch name when included in a union.)

### `type.clone(val, [opts])`

+ `val` {...} The object to copy.
+ `opts` {Object} Options:
  + `coerceBuffers` {Boolean} Allow coercion of JSON buffer representations
    into actual `Buffer` objects. When used with unwrapped unions, ambiguities
    caused by this coercion are always resolved in favor of the buffer type.
  + `fieldHook(field, any, type)` {Function} Function called when each record
    field is populated. The value returned by this function will be used
    instead of `any`. `field` is the current `Field` instance and `type` the
    parent type.
  + `qualifyNames` {Boolean} The branch's key in the union object should be the
    qualified name of its type, however some serializers incorrectly omit the
    namespace (which can cause collisions). Passing in this option will attempt
    to lookup unqualified names as well and return correctly qualified names.
    This option has no effect when used with unwrapped unions.
  + `skipMissingFields` {Boolean} Ignore any missing fields (or equal to
    `undefined`). This can be useful in combination with clone's other options
    to perform validation after wrapping unions or coercing buffers. Fields
    missing in the input will be set to undefined in the output.
  + `wrapUnions` {Boolean} Allow wrapping of union values into their first
    matching branch. This option has no effect when used with unwrapped unions.

Deep copy a value of `type`.

### `type.compare(val1, val2)`

+ `val1` {...} Value of `type`.
+ `val2` {...} Value of `type`.

Returns `0` if both values are equal according to their [sort
order][sort-order], `-1` if the first is smaller than the second , and `1`
otherwise. Comparing invalid values is undefined behavior.

### `type.compareBuffers(buf1, buf2)`

+ `buf1` {Buffer} `type` value bytes.
+ `buf2` {Buffer} `type` value bytes.

Similar to [`compare`](#typecompareval1-val2), but doesn't require decoding
values.

### `type.createResolver(writerType)`

+ `writerType` {Type} Writer type.

Create a resolver that can be be passed to the `type`'s
[`decode`](#typedecodebuf-pos-resolver) and
[`fromBuffer`](#typefrombufferbuf-resolver-nocheck) methods. This will enable
decoding values which had been serialized using `writerType`, according to the
Avro [resolution rules][schema-resolution]. If the schemas are incompatible,
this method will throw an error.

For example, assume we have the following two versions of a type:

```javascript
// A schema's first version.
const v1 = avro.Type.forSchema({
  name: 'Person',
  type: 'record',
  fields: [
    {name: 'name', type: 'string'},
    {name: 'age', type: 'int'}
  ]
});

// The updated version.
const v2 = avro.Type.forSchema({
  type: 'record',
  name: 'Person',
  fields: [
    {
      name: 'name', type: [
        'string',
        {
          name: 'Name',
          type: 'record',
          fields: [
            {name: 'first', type: 'string'},
            {name: 'last', type: 'string'}
          ]
        }
      ]
    },
    {name: 'phone', type: ['null', 'string'], default: null}
  ]
});
```

The two types are compatible since the `name` field is present in both (the
`string` can be promoted to the new `union`) and the new `phone` field has a
default value.

```javascript
//  We can therefore create a resolver.
const resolver = v2.createResolver(v1);

// And pass it whenever we want to decode from the old type to the new.
const buf = v1.toBuffer({name: 'Ann', age: 25});
const obj = v2.fromBuffer(buf, resolver); // === {name: {string: 'Ann'}, phone: null}
```

See the [advanced usage page](Advanced-usage) for more details on how schema
evolution can be used to significantly speed up decoding.

### `type.decode(buf, [pos,] [resolver])`

+ `buf` {Buffer} Buffer to read from.
+ `pos` {Number} Offset to start reading from.
+ `resolver` {Resolver} Optional resolver to decode values serialized from
  another schema. See [`createResolver`](#typecreateresolverwritertype) for how
  to create one.

Returns `{value: value, offset: offset}` if `buf` contains a valid encoding of
`type` (`value` being the decoded value, and `offset` the new offset in the
buffer). Returns `{value: undefined, offset: -1}` when the buffer is too short.

### `type.doc`

Return the type's documentation (`doc` attribute in schema and docstring in IDL
spec).

### `type.encode(val, buf, [pos])`

+ `val` {...} The value to encode. An error will be raised if this isn't a
  valid `type` value.
+ `buf` {Buffer} Buffer to write to.
+ `pos` {Number} Offset to start writing at.

Encode a value into an existing buffer. If enough space was available in `buf`,
returns the new (non-negative) offset, otherwise returns `-N` where `N` is the
(positive) number of bytes by which the buffer was short.

### `type.equals(any)`

+ `any` {...} Any object.

Check whether two types are equal (i.e. have the same canonical schema).

### `type.fingerprint([algorithm])`

+ `algorithm` {String} Algorithm used to compute the hash. Defaults to `'md5'`.
  *Only `'md5'` is supported in the browser.*

Return a buffer identifying `type`.

### `type.fromBuffer(buf, [resolver,] [noCheck])`

+ `buf` {Buffer} Bytes containing a serialized value of `type`.
+ `resolver` {Resolver} To decode values serialized from another schema. See
  [`createResolver`](#typecreateresolverwritertype) for how to create an
  resolver.
+ `noCheck` {Boolean} Do not check that the entire buffer has been read. This
  can be useful when using an resolver which only decodes fields at the start of
  the buffer, allowing decoding to bail early and yield significant performance
  speedups.

Deserialize a buffer into its corresponding value.

### `type.fromString(str)`

+ `str` {String} String representing a JSON-serialized object.

Deserialize a JSON-encoded object of `type`.

### `type.isValid(val, [opts])`

+ `val` {...} The value to validate.
+ `opts` {Object} Options:
  + `errorHook(path, any, type)` {Function} Function called when an invalid
    value is encountered. When an invalid value causes its parent values to
    also be invalid, the latter do not trigger a callback. `path` will be an
    array of strings identifying where the mismatch occurred. This option is
    especially useful when dealing with complex records, for example to:
      + [Collect all paths to invalid nested values.](https://gist.github.com/mtth/fe006b5b001beeaed95f#file-collect-js)
      + [Throw an error with the full path to an invalid nested value.](https://gist.github.com/mtth/fe006b5b001beeaed95f#file-assert-js)
  + `noUndeclaredFields` {Boolean} When set, records with attributes that don't
    correspond to a declared field will be considered invalid. The default is
    to ignore any extra attributes.

Check whether `val` is a valid `type` value.

### `type.name`

Returns `type`'s fully qualified name if it exists, `undefined` otherwise.

### `type.random()`

Returns a random value of `type`.

### `type.schema([opts])`

+ `opts` {Object} Options:
  + `exportAttrs` {Boolean} Include aliases, field defaults, order, and logical
    type attributes in the returned schema.
  + `noDeref` {Boolean} Do not dereference any type names.

Returns `type`'s [canonical schema][canonical-schema]. This can be used to
compare schemas for equality.

### `type.toBuffer(val)`

+ `val` {...} The value to encode. It must be a valid `type` value.

Returns a `Buffer` containing the Avro serialization of `val`.

### `type.toString([val])`

+ `val` {...} The value to serialize. If not specified, this method will return
  a human-friendly description of `type`.

Serialize an object into a JSON-encoded string.

### `type.typeName`

Returns `type`'s "type name" (e.g. `'int'`, `'record'`, `'fixed'`).

### `type.wrap(val)`

+ `val` {...} The value to wrap, this value should should be valid for `type`.
  Behavior is otherwise undefined.

Convenience method to wrap a value into a valid branch for use in a wrapped
union:

```javascript
const intType = avro.Type.forSchema('int');
intType.wrap(123); // {int: 123}
```

## Class `ArrayType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `type.itemsType`

The type of the array's items.


## Class `EnumType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `type.symbols`

Returns the type's symbols (an array of strings representing the `enum`'s valid
values).


## Class `FixedType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `type.size`

The size in bytes of instances of this type.

## Class `LogicalType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

"Abstract class" used to implement custom types. To implement a new logical
type, the steps are:

+ Call `LogicalType`'s constructor inside your own subclass' to make sure the
  underlying type is property set up. Throwing an error anywhere inside your
  constructor will prevent the logical type from being used (the underlying
  type will be used instead).
+ Extend `LogicalType` in your own subclass (typically using `util.inherits`).
+ Override the following methods (prefixed with an underscore because they are
  internal to the class that defines them and should only be called by the
  internal `LogicalType` methods):
  + `_export` (optional)
  + `_fromValue`
  + `_resolve` (optional)
  + `_toValue`

See [here][logical-types] for more information. A couple sample implementations
are available as well:

+ [`DateType`](https://gist.github.com/mtth/1aec40375fbcb077aee7#file-date-js)
+ [`DecimalType`](https://gist.github.com/mtth/1aec40375fbcb077aee7#file-decimal-js)

### `type.underlyingType`

Use this method to get the underlying Avro type. This can be useful when a
logical type can support different underlying types.

### `type._export(schema)`

+ `schema` {Object} The type's raw exported attributes, containing `type` and
  `logicalType` keys.

This method should add attributes to be exported to the `schema` object. These
will then be included into any [`type.getSchema`](#typegetschema-opts) calls
with `exportAttrs` set. *A default implementation exporting nothing is
provided.*

### `type._fromValue(val)`

+ `val` {...} A value deserialized by the underlying type.

This method should return the converted value. *This method is abstract and
should be implemented but not called directly.*

### `type._resolve(type)`

+ `type` {Type} The writer's type.

This method should return:

+ `undefined` if the writer's values cannot be converted.
+ Otherwise, a function which converts a value deserialized by the writer's
  type into a wrapped value for the current type.

*This method is abstract and should be implemented but not called directly.*

### `type._toValue(any)`

+ `any` {...} A derived value.

This method should return a value which can be serialized by the underlying
type. If `any` isn't a valid value for this logical type, you can either return
`undefined` or throw an exception (slower). *This method is abstract and should
be implemented but not called directly.*

## Class `LongType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `LongType.__with(methods, [noUnpack])`

+ `methods` {Object} Method implementations dictionary keyed by method name,
  see below for details on each of the functions to implement.
+ `noUnpack` {Boolean} Do not automatically unpack bytes before passing them to
  the above `methods`' `fromBuffer` function and pack bytes returned by its
  `toBuffer` function.

This function provides a way to support arbitrary long representations. Doing
so requires implementing the following methods (a few examples are available
[here][custom-long]):

+ `compare(val1, val2)`

  See [`Type.compare`](#typecompareval1-val2).

+ `isValid(val, [opts])`

  See [`Type.isValid`](#typeisvalidval-opts).

+ `fromBuffer(buf)`

  + `buf` {Buffer} Encoded long. If `noUnpack` is off (the default), `buf` will
    be an 8-byte buffer containing the long's unpacked representation.
    Otherwise, `buf` will contain a variable length buffer with the long's
    packed representation.

  This method should return the corresponding decoded long.

+ `fromJSON(any)`

  + `any` {Number|...} Parsed value. To ensure that the `fromString` method
    works correctly on data JSON-serialized according to the Avro spec, this
    method should at least support numbers as input.

  This method should return the corresponding decoded long.

  It might also be useful to support other kinds of input (typically the output
  of the long implementation's `toJSON` method) to enable serializing large
  numbers without loss of precision (at the cost of violating the Avro spec).

+ `toBuffer(val)`

  + `val` {...} Decoded long.

  If `noUnpack` is off (the default), this method should return an 8-byte
  buffer with the `long`'s unpacked representation. Otherwise, `toBuffer`
  should return an already packed buffer (of variable length).

+ `toJSON(val)`

  + `val` {...} Decoded long.

  This method should return the `long`'s JSON representation.

## Class `MapType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `type.valuesType`

The type of the map's values (keys are always strings).

## Class `RecordType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

### `type.field(name)`

+ `name` {String} Field name.

Convenience method to retrieve a field by name. A field is an object with the
following methods:

#### `class Field`

##### `field.aliases`

The list of aliases for this field.

##### `field.defaultValue()`

The field's default value if specified, `undefined` otherwise.

##### `field.name`

The field's name.

##### `field.order`

One of `'ascending'`, `'descending'`, or `'ignored'`.

##### `field.type`

The field's type.

### `type.fields`

Returns the array of fields contained in this record.

### `type.recordConstructor`

The [`Record`](#class-record) constructor for instances of this type. Indeed,
each [`RecordType`](#class-recordtypeattrs-opts) generates a corresponding
`Record` constructor when its schema is parsed. This helps make decoding and
encoding records more efficient. This also lets us provide helpful methods on
decoded values (see below).

#### Class `Record(...)`

Calling the constructor directly can sometimes be a convenient shortcut to
instantiate new records of a given type. In particular, it will correctly
initialize all the missing record's fields with their default values.

The `Record` prototype also exposes a few convenience methods described below
(available on each decoded `record` value).

##### `Record.type`

Convenience class method to get the record's type.

##### `record.clone([opts])`

Convenience function to clone the current record.

##### `record.compare(val)`

Convenience function to compare the current record to another.

##### `record.isValid([opts])`

Convenience function to validate the current record.

##### `record.toBuffer()`

Convenience function to serialize the current record.

##### `record.toString()`

Convenience function to serialize the current record using JSON encoding.

##### `record.wrapped()`

Convenience function to wrap the record into a valid wrapped union branch.

## Class `UnwrappedUnionType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

This class is the default used to represent unions. Its values are decoded
without a wrapping object: `null` and `48` would be valid values for the schema
`["null", "int"]` (as opposed to `null` and `{'int': 48}` for wrapped unions).

This representation is usually more convenient and natural, however it isn't
able to guarantee correctness for all unions. For example, we wouldn't be able
to tell which branch the value `23` comes from in a schema `["int", "float"]`.
More concretely, a union can be represented using this class if it has at most
a single branch inside each of the categories below:

+ `'null'`
+ `'boolean'`
+ `'int'`, `'long'`, `'float'`, `'double'`
+ `'string'`, `'enum'`
+ `'bytes'`, `'fixed'`
+ `'array'`
+ `'map'`, `'record'`

So `['null', 'int']` and `['null', 'string', {type: 'array', items: 'string'}]`
are supported, but `['int', 'float']` and `['bytes', {name: 'Id', type:
'fixed', size: 2}]` are not.

Finally, note that by using logical types, it is possible to work around the
above requirements (by delegating the branch inference to the logical types
themselves).

### `type.types`

The possible types that this union can take.

## Class `WrappedUnionType(schema, [opts])`

+ `schema` {Object} Decoded type attributes.
+ `opts` {Object} Parsing options.

This class is the representation using for unions for types generated with
`forSchema`'s `wrapUnions` option set. It uses Avro's JSON encoding and is able
to correctly represent all unions: branch type information is never lost since
it is included in the decoded value.

### `type.types`

The possible types that this union can take.

Additionally, each value decoded from a wrapped union exposes its corresponding
type via its constructor. This is also typically faster than calling
`Object.keys()` on the value when the active branch is unknown.

```javascript
const type = new avro.types.WrappedUnionType(['int', 'long']);
const val = type.fromBuffer(new Buffer([2, 8])); // == {long: 4}
const branchType = val.constructor.getBranchType() // == <LongType>
```


# Files and streams

The following convenience functions are available for common operations on
container files:

## `createBlobDecoder(blob, [opts])`

+ `blob` {Blob} Binary blob.
+ `opts` {Object} Decoding options, passed to
  [`BlockDecoder`](#class-blockdecoderopts).

Returns a readable stream of decoded objects from an Avro container blob. *Only
available in the browser when using the full distribution.*

## `createBlobEncoder(schema, [opts])`

+ `schema` {Object|String|Type} Type used to serialize.
+ `opts` {Object} Encoding options, passed to
  [`BlockEncoder`](#class-blockencoderschema-opts).

Returns a duplex stream of objects. Written values will end up serialized into
an Avro container blob which will be output as the stream's only readable
value. *Only available in the browser when using the full distribution.*

## `createFileDecoder(path, [opts])`

+ `path` {String} Path to Avro container file.
+ `opts` {Object} Decoding options, passed to
  [`BlockDecoder`](#class-blockdecoderopts).

Returns a readable stream of decoded objects from an Avro container file. *Not
available in the browser.*

## `createFileEncoder(path, schema, [opts])`

+ `path` {String} Destination path.
+ `schema` {Object|String|Type} Type used to serialize.
+ `opts` {Object} Encoding options, passed to
  [`BlockEncoder`](#class-blockencoderschema-opts).

Returns a writable stream of objects. These will end up serialized into an Avro
container file. *Not available in the browser.*

## `extractFileHeader(path, [opts])`

+ `path` {String} Path to Avro container file.
+ `opts` {Object} Options:
  + `decode` {Boolean} Decode schema and codec metadata (otherwise they will be
    returned as bytes). Defaults to `true`.

Extract header from an Avro container file synchronously. If no header is
present (i.e. the path doesn't point to a valid Avro container file), `null` is
returned. *Not available in the browser.*

## Class `BlockDecoder([opts])`

+ `opts` {Object} Decoding options. Available keys:
  + `codecs` {Object} Dictionary of decompression functions, keyed by codec
    name. A decompression function has the signature `fn(compressedData, cb)` where
    `compressedData` is a buffer of compressed data, and must call `cb(err,
    uncompressedData)` on completion. The default contains handlers for the
    `'null'` and `'deflate'` codecs.
  + `noDecode` {Boolean} Do not decode records before returning them.
  + `parseHook(schema)` {Function} Function called to generate the type from the
    schema contained in the file. This can be used to pass in addtional options
    when parsing the schema (e.g. logical type information). See below for an
    example.

A duplex stream which decodes bytes coming from on Avro object container file.

Sample use of the `codecs` option to decode a Snappy encoded file using
[snappy](https://www.npmjs.com/package/snappy) (note [checksum
handling](https://avro.apache.org/docs/1.8.0/spec.html#snappy)):

```javascript
const crc32 = require('buffer-crc32');
const snappy = require('snappy');

const blockDecoder = new avro.streams.BlockDecoder({
  codecs: {
    snappy: function (buf, cb) {
      // Avro appends checksums to compressed blocks.
      const len = buf.length;
      const checksum = buf.slice(len - 4, len);
      const inflated = snappy.uncompress(buf.slice(0, len - 4), cb);
      if (!checksum.equals(crc32(inflated))) {
        // We make sure that the checksum matches.
        cb(new Error('invalid checksum'));
        return;
      }
      cb(null, inflated);
    }
  }
});
```

Note that the `BlockDecoder`'s `opts` aren't used when parsing the writer's
type. A `parseHook` should be used instead. The example below shows how to
instantiate a type with the `wrapUnions` option set:

```javascript
const decoder = new avro.streams.BlockDecoder({
  parseHook: (schema) => {
    return avro.Type.forSchema(schema, {wrapUnions: true});
  }
});
```

### Event `'metadata'`

+ `type` {Type} The type used to write the file.
+ `codec` {String} The codec's name.
+ `header` {Object} The file's header, containing in particular the raw schema
  and codec.

This event is guaranteed to be emitted before the first `'data'` event.

### Event `'data'`

+ `data` {...} Decoded element or raw bytes.

### `BlockDecoder.getDefaultCodecs()`

Get built-in decompression functions (currently `null` and `deflate`).

## Class `BlockEncoder(schema, [opts])`

+ `schema` {Object|String|Type} Schema used for encoding. Argument parsing
  logic is the same as for [`parse`](#parseschema-opts).
+ `opts` {Object} Encoding options. Available keys:
  + `blockSize` {Number} Maximum uncompressed size of each block data. A new
    block will be started when this number is exceeded. If it is too small to
    fit a single element, it will be increased appropriately. Defaults to 64kB.
  + `codec` {String} Name of codec to use for encoding. See `codecs` option
    below to support arbitrary compression functions.
  + `codecs` {Object} Dictionary of compression functions, keyed by codec
    name. A compression function has the signature `fn(uncompressedData, cb)` where
    `uncompressedData` is a buffer of uncompressed data, and must call `cb(err,
    compressedData)` on completion. The default contains handlers for the
    `'null'` and `'deflate'` codecs.
  + `omitHeader` {Boolean} Don't emit the header. This can be useful when
    appending to an existing container file. Defaults to `false`.
  + `syncMarker` {Buffer} 16 byte buffer to use as synchronization marker
    inside the file. If unspecified, a random value will be generated.

A duplex stream to create Avro container object files.

### Event `'data'`

+ `data` {Buffer} Serialized bytes.

### `BlockEncoder.getDefaultCodecs()`

Get built-in compression functions (currently `null` and `deflate`).

## Class `RawDecoder(schema, [opts])`

+ `schema` {Object|String|Type} Writer schema. Required since the input doesn't
  contain a header. Argument parsing logic is the same as for
  [`parse`](#parseschema-opts).
+ `opts` {Object} Decoding options. Available keys:
  + `decode` {Boolean} Whether to decode records before returning them.
    Defaults to `true`.

A duplex stream which can be used to decode a stream of serialized Avro objects
with no headers or blocks.

### Event `'data'`

+ `data` {...} Decoded element or raw bytes.

## Class `RawEncoder(schema, [opts])`

+ `schema` {Object|String|Type} Schema used for encoding. Argument parsing
  logic is the same as for [`parse`](#parseschema-opts).
+ `opts` {Object} Encoding options. Available keys:
  + `batchSize` {Number} To increase performance, records are serialized in
    batches. Use this option to control how often batches are emitted. If it is
    too small to fit a single record, it will be increased automatically.
    Defaults to 64kB.

The encoding equivalent of `RawDecoder`.

### Event `'data'`

+ `data` {Buffer} Serialized bytes.


# IPC & RPC

Avro also defines a way of executing remote procedure calls.

## `assembleProtocol(path, [opts,] cb)`

+ `path` {String} Path to Avro IDL file.
+ `opts` {Object} Options:
  + `ackVoidMessages` {Boolean} By default, using `void` as response type will
    mark the corresponding message as one-way. When this option is set, `void`
    becomes equivalent to `null`.
  + `delimitedCollections` {Boolean} The parser will be default support
    collections (`array` items and `map` values) even when they aren't
    surrounded by `</>` markers; this tends to lead to cleaner inline
    declarations. You can disable this extensions by setting this option.
  + `importHook(path, kind, cb)` {Function} Function called to load each file.
    The default will look up the files in the local file-system and load them
    via `fs.readFile`. `kind` is one of `'idl'`, `'protocol'`, or `'schema'`
    depending on the kind of import requested. *In the browser, no default
    is provided.*
  + `typeRefs` {Object} Type references, used to expand custom type names. This
    option defaults to values compatible with the Java implementation.
+ `cb(err, schema)` {Function} Callback. If an error occurred, its `path`
  property will contain the path to the file which caused it.

Assemble IDL files into a protocol. This protocol can then be passed to
`Service.forProtocol` to instantiate the corresponding service.

## `discoverProtocol(transport, [opts,] cb)`

+ `transport` {Transport} See below.
+ `opts` {Object} Options:
  + `scope` {String} Remove server scope.
  + `timeout` {Number} Maximum delay to wait for a response, `0` for no limit.
    Defaults to `10000`.
+ `cb(err, protocol)` {Function} Callback.

Discover a remote server's protocol. This can be useful to emit requests to
another server without having a local copy of the protocol.

## `readProtocol(spec, [opts])`

+ `spec` {String} Protocol IDL specification.
+ `opts` {Object} Options (see `assembleProtocol` for details).
  + `ackVoidMessages` {Boolean} See `assembleProtocol`.
  + `delimitedCollections` {Boolean} See `assembleProtocol`.
  + `typeRefs` {Object} See `assembleProtocol`.

Synchronous version of `assembleProtocol`. Note that it doesn't support
imports.

## Class `Service`

`Service` instances are generated from a [protocol
declaration][protocol-declaration] and define an API that can be used to send
remote messages (for example to another machine or another process on the same
machine).

### `Service.forProtocol(protocol, [opts])`

+ `protocol` {Object} A valid Avro protocol.
+ `opts` {Object} All of `Type.forSchema`'s options are accepted.

Construct a service from a protocol.

### `service.createClient([opts])`

+ `opts` {Object} Options:
  + `buffering` {Boolean} By default emitting messages before any channels
    become active will fail. Setting this option will instead cause messages to
    be buffered until a channel becomes available.
  + `channelPolicy(channels)` {Function} Function to load balance between
    active channels. Should return one of the passed in channels. The default
    selects a channel at random.
  + `remoteProtocols` {Object} Map of remote protocols, keyed by hash, to cache
    locally. This will save a handshake when connecting using one of these
    protocols.
  + `server` {Server} Convenience function to connect the client to an existing
    server using an efficient in-memory channel.
  + `strictTypes` {Boolean} Disable conversion of string errors to `Error`
    objects and of `null` to `undefined`.
  + `timeout` {Number} Default timeout in milliseconds used when emitting
    requests, specify `0` for no timeout (note that this may cause memory leaks
    in the presence of communication errors). Defaults to `10000`. This timeout
    can be overridden on a per-request basis. Finally, this timeout only
    applies to RPC handling (i.e. neither middleware or buffering count towards
    this timeout).
  + `transport` {Transport} Convenience option to add a transport to the newly
    created client.

Generate a client corresponding to this service. This client can be used to
send messages to a server for a compatible service.

### `service.createServer([opts])`

+ `opts` {Object} Options:
  + `noCapitalize` {Boolean} By default, handler setters will be generated on
    the server using the convention `on<CapitalizedMessageName>` (e.g. message
    `resolveUrl` would correspond to `onResolveUrl`). Use this option to use
    the raw name instead.
  + `remoteProtocols` {Object} Map of remote protocols, keyed by hash, to cache
    locally. This will save a handshake with clients connecting using one of
    these protocols.
  + `silent` {Boolean} Suppress default behavior of outputting handler errors
    to standard error.
  + `strictTypes` {Boolean} Disable automatic conversion of `Error` objects to
    strings, and `null` to `undefined`. When set, handlers' returned error
    parameters must either be a valid union branch or `undefined`.
  + `systemErrorFormatter(err)` {Function} Function called to format system
    errors before sending them to the calling client. It should return a
    string.

Generate a server corresponding to this service. This server can be used to
respond to messages from compatible protocols' clients.

### `service.doc`

Get the service's docstring.

### `service.hash`

Returns a buffer containing the service's protocol's hash.

### `service.message(name)`

+ `name` {String} Message name.

Get a single message from this service.

#### `class Message`

##### `message.doc`

The message's documentation (`doc` field).

##### `message.errorType`

The message's error type (always a union, with a string as first branch).

##### `message.name`

The message's name.

##### `message.oneWay`

Whether the message expects a response.

##### `message.requestType`

The type of this message's requests (always a record).

##### `message.responseType`

The type of this message's responses.

##### `message.schema([opts])`

Return this message's schema.

### `service.messages`

Retrieve a list of all the messages defined in the service.

### `service.name`

Returns the service's fully qualified name.

### `service.protocol`

Returns the service's protocol.

### `service.type(name)`

+ `name` {String} A type's fully qualified name.

Convenience function to retrieve a type defined inside this service. Returns
`undefined` if no type exists for the given name.

### `service.types`

Returns a list of the types declared in this service.

## Class `Client`

### Event `'channel'`

+ `channel` {ClientChannel} The newly created channel.

Event emitted each time a channel is created.

### `client.activeChannels()`

Returns a list of this client's currently active channels (i.e. neither
draining nor destroyed).

### `client.createChannel(transport, [opts])`

+ `transport` {Duplex|Object|Function} The transport used to communicate with
  the remote listener. Multiple argument types are supported, see below.
+ `opts` {Object} Options.
  + `endWritable` {Boolean} Set this to `false` to prevent the transport's
    writable stream from being `end`ed when the emitter is destroyed (for
    stateful transports) or when a request is sent (for stateless transports).
    Defaults to `true`.
  + `noPing` {Boolean} Do not emit a ping request when the emitter is created.
    For stateful transports this will assume that a connection has already been
    established, for stateless transports this will delay handshakes until the
    first message is sent.
  + `objectMode` {Boolean} Expect a transport in object mode. Instead of
    exchanging buffers, objects `{id, payload}` will be written and expected.
    This can be used to implement custom transport encodings.
  + `scope` {String} Scope used to multiplex messages across a shared
    connection. There should be at most one emitter or listener per scope on a
    single stateful transport. Matching emitter/listener pairs should have
    matching scopes. Scoping isn't supported on stateless transports.
  + `serverHash` {Buffer} Hash of remote protocol to use for the initial
    handshake. If unspecified or the corresponding hash isn't found in the
    client's cache, the client's protocol will be used instead.

Generate a channel for this client. This channel can then be used to
communicate with a remote server of compatible protocol.

There are two major types of transports:

+ Stateful: a pair of binary streams `{readable, writable}`. As a convenience
  passing a single duplex stream is also supported and equivalent to passing
  `{readable: duplex, writable: duplex}`.

+ Stateless: stream factory `fn(cb)` which should return a writable stream and
  call its callback argument with an eventual error and readable stream (if
  available).

### `client.destroyChannels([opts])`

+ `opts` {Object} Options:
  + `noWait` {Boolean} Don't wait for pending requests to drain before
    destroying the channels.

Destroy all the client's currently active channels.

### `client.emitMessage(name, req, [opts,] [cb])`

+ `name` {String} Name of the message to emit.
+ `req` {Object} Request value, must correspond to the message's declared
  request type.
+ `opts` {Object} Options. These options will be available as second argument
  to the chosen channel's `'outgoingCall'` event.
  + `timeout` {Number} Request specific timeout.
+ `cb(err, res)` {Function} Function called with the remote call's response
  (and eventual error) when available. If not specified and an error occurs,
  the error will be emitted on the client instead.

Send a message. This is always done asynchronously.

### `client.remoteProtocols()`

Returns the client's cached protocols.

### `client.service`

The client's service.

### `client.use(middleware...)`

+ `middleware(wreq, wres, next)` {Function} Middleware handler.

Install a middleware function.

#### Class `WrappedRequest`

##### `headers`

Map of bytes.

##### `request`

The decoded request.

#### Class `WrappedResponse`

##### `error`

Decoded error. If `error` is anything but `undefined`, the `response` field
will be ignored and the error will be sent instead.

##### `headers`

Map of bytes.

##### `response`

Decoded response.

#### Class `CallContext`

##### `channel`

The channel used to emit or receive this message (either a `ClientChannel` or
`ServerChannel`).

##### `locals`

An object useful to store call-local information to pass between middlewares
and handlers.

##### `message`

The message being processed.

## Class `Server`

### Event `'channel'`

+ `channel` {ServerChannel} The newly created channel.

Event emitted each time a channel is created.

### `server.activeChannels()`

Returns a copy of the server's active channels.

### `server.createChannel(transport, [opts])`

+ `transport` {Duplex|Object|Function} Similar to `client.createChannel`'s
  corresponding argument, except that readable and writable roles are reversed
  for stateless transports.
+ `opts` {Object} Options.
  + `defaultHandler(wreq, wres, prev)` {Function} Function called when no
    handler has been installed for a given message. The default sends back a
    "not implemented" error response.
  + `endWritable` {Boolean} Set this to `false` to prevent the transport's
    writable stream from being `end`ed when the emitter is destroyed (for
    stateful transports) or when a response is sent (for stateless transports).
    Defaults to `true`.
  + `objectMode` {Boolean} Expect a transport in object mode. Instead of
    exchanging buffers, objects `{id, payload}` will be written and expected.
    This can be used to implement custom transport encodings.
  + `scope` {String} Scope used to multiplex messages accross a shared
    connection. There should be at most one channel per scope on a single
    stateful transport. Matching channel pairs (client and server) should have
    matching scopes. Scoping isn't supported on stateless transports.

Generate a channel for this server. This channel can be used to respond to
messages emitted from compatible clients.

### `server.onMessage(name, handler)`

+ `name` {String} Message name to add the handler for. An error will be thrown
  if this name isn't defined in the protocol. At most one handler can exist for
  a given name (any previously defined handler will be overwritten).
+ `handler(req, cb)` {Function} Handler, called each time a message with
  matching name is received. The callback argument `cb(err, res)` should be
  called to send the response back to the emitter.

Add a handler for a given message.

### `server.remoteProtocols()`

Returns the server's cached protocols.

### `server.service`

Returns the server's service.

### `server.use(middleware...)`

+ `middleware(wreq, wres, next)` {Function} Middleware handler.

Install a middleware function.

## Class `ClientChannel`

Instance of this class are [`EventEmitter`s][event-emitter], with the following
events:

### Event `'eot'`

End of transmission event, emitted after the client is destroyed and there are
no more pending requests.

### Event `'handshake'`

+ `hreq` {Object} Handshake request.
+ `hres` {Object} Handshake response.

Emitted when the server's handshake response is received. Additionally, the
following guarantees are made w.r.t. the timing of this event:

+ Destroying the channel inside a (synchronous) handler for this event will
  interrupt any ongoing handshake. If the handshake response's match was
  `NONE`, it will prevent a connection from taking place in the case of
  stateful channels and cancel the retry of the request in the case of
  stateless channels.
+ For stateful channels which do not reuse connections (i.e. created without
  setting `noPing` to `true`), this event will be emitted before any
  `'outgoingCall'` events.

### Event `'outgoingCall'`

+ `ctx` {CallContext} The call's context.
+ `opts` {Object} The options used when emitting the message.

Emitted when a message was just emitted using this channel.

### `channel.destroy([noWait])`

+ `noWait` {Boolean} Cancel any pending requests. By default pending requests
  will still be honored.

Disable the channel.

### `channel.client`

The channel's client.

### `channel.destroyed`

Whether the channel was destroyed.

### `channel.draining`

Whether the channel is still accepting new requests.

### `channel.pending`

The number of pending calls on this channel (i.e. the number of messages
emitted on this channel which haven't yet had a response).

### `channel.ping([timeout,] [cb])`

+ `timeout` {Number} The ping request's timeout.
+ `cb(err)` {Function} Function called when the request's response is received.
  If not specified and an error occurs, the channel will be destroyed.

### `channel.timeout`

The channel's default timeout.

## Class `ServerChannel`

### Event `'eot'`

End of transmission event, emitted after the channel is destroyed and there are
no more responses to send.

### Event `'handshake'`

+ `hreq` {Object} Handshake request.
+ `hres` {Object} Handshake response.

Emitted right before the server sends a handshake response. This event is
guaranteed to be emitted before any `'incomingCall'` event. Additionally,
destroying the channel synchronously in one of this event's handlers will
prevent any responses from being sent back.

### Event `'incomingCall'`

+ `context` {CallContext} The call's context.
+ `opts` {Object} The options used when emitting the message.

Emitted when a message was just received on this channel.

### `channel.destroy([noWait])`

+ `noWait` {Boolean} Don't wait for all pending responses to have been sent.

Disable this channel and release underlying streams. In general you shouldn't
need to call this: channels are best destroyed from the client side.

### `channel.destroyed`

Check whether the channel was destroyed.

### `channel.draining`

Whether the channel is still accepting requests.

### `channel.pending`

The number of pending calls (i.e. the number of messages received which haven't
yet had their response sent).

### `channel.server`

Get the channel's server.


[canonical-schema]: https://avro.apache.org/docs/current/spec.html#Parsing+Canonical+Form+for+Schemas
[schema-resolution]: https://avro.apache.org/docs/current/spec.html#Schema+Resolution
[sort-order]: https://avro.apache.org/docs/current/spec.html#order
[fingerprint]: https://avro.apache.org/docs/current/spec.html#Schema+Fingerprints
[custom-long]: Advanced-usage#custom-long-types
[logical-types]: Advanced-usage#logical-types
[framing-messages]: https://avro.apache.org/docs/current/spec.html#Message+Framing
[event-emitter]: https://nodejs.org/api/events.html#events_class_events_eventemitter
[protocol-declaration]: https://avro.apache.org/docs/current/spec.html#Protocol+Declaration
