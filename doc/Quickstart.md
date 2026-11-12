<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Types](#types)
  - [What is a `Type`?](#what-is-a-type)
  - [Container files](#container-files)
- [Services](#services)
  - [Defining a `Service`](#defining-a-service)
  - [Server implementation](#server-implementation)
  - [Calling our service](#calling-our-service)
- [Next steps](#next-steps)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Types

One of the main features provided by [Avro][] is a way to encode (_serialize_)
data. Once data is encoded, it can be stored in a file or a database, sent
across the internet to be decoded on another computer, etc.

Many other encodings exist. [JSON][] for example is very commonly used from
JavaScript: it's built-in (via `JSON.parse` and `JSON.stringify`), reasonably
fast, and produces human-readable encodings.

```javascript
> pet = {kind: 'DOG', name: 'Beethoven', age: 4};
> str = JSON.stringify(pet);
'{"kind":"DOG","name":"Beethoven","age":4}' // Encoded data (still readable).
> str.length
41 // Number of bytes in the encoding.
```

JSON isn't always the most adequate encoding though. It produces relatively
large encodings since keys are repeated in the output (`kind`, `name`, and
`age` above). It also doesn't enforce any properties on the data, so any
validation has to be done separately (other encodings with the same
_schema-less_ property include [xml][], [MessagePack][message-pack]).

Avro `type`s provide an alternate serialization mechanism, with a different set
of properties:

+ _Schema-aware_: each `type` is tied to a particular data structure and will
  validate that any encoded data matches this structure.
+ _Compact_: Avro's binary encoding isn't meant to be human-readable, it is
  instead optimized for size and speed. Depending on the data, `avsc` can be an
  order of magnitude faster and smaller than JSON.


## What is a `Type`?

A `type` is an JavaScript object which knows how to
[`decode`](Api#typedecodebuf-pos-resolver) and
[`encode`](Api#typeencodeval-buf-pos) a "family" of values. Examples of
supported families include:

+ All strings.
+ All arrays of numbers.
+ All `Buffer`s of length 4.
+ All objects with an integer `id` property and string `name` property.

By default, Avro uses [schemas][avro-schemas] to define which family a type
supports. These schemas are written using JSON (the human-readable encoding
described earlier). The syntax can be confusing at first so `avsc` provides an
alternate way of defining a type: given a decoded value, we will _infer_ a
matching type. We can use this to view what the corresponding Avro schema would
look like:

```javascript
> avro = require('avsc');
> inferredType = avro.Type.forValue(pet); // Infer the type of a `pet`.
> inferredType.schema();
{ type: 'record', // "Record" is Avro parlance for "structured object".
  fields:
   [ { name: 'kind', type: 'string' }, // Each field corresponds to a property.
     { name: 'name', type: 'string' },
     { name: 'age', type: 'int' } ] }
```

Now that we have a type matching our `pet` object, we can encode it:

```javascript
> buf = inferredType.toBuffer(pet);
> buf.length
15 // 60% smaller than JSON!
> inferredType.fromBuffer(buf);
{ kind: 'DOG', // Loss-less serialization roundtrip.
  name: 'Beethoven',
  age: 4 }
```

We can also validate other data against our inferred schema:

```javascript
> inferredType.isValid({kind: 'CAT', name: 'Garfield', age: 5.2});
false // The age isn't an integer.
> inferredType.isValid({name: 'Mozart', age: 3});
false // The kind field is missing.
> inferredType.isValid({kind: 'PIG', name: 'Babe', age: 2});
true // All fields match.
```

It is sometimes useful to define a type by hand, for example to declare
assumptions that the inference logic can't make. Let's assume that the `kind`
field should only ever contain values `'CAT'` or `'DOG'`. We can use this
extra information to improve on the inferred schema:

```javascript
> exactType = avro.Type.forSchema({
...   type: 'record',
...   fields: [
...     {name: 'kind', type: {type: 'enum', symbols: ['CAT', 'DOG']}},
...     {name: 'name', type: 'string'},
...     {name: 'age', type: 'int'}
...   ]
... });
> buf = exactType.toBuffer(pet);
> buf.length
12 // 70% smaller than JSON!
```

Validation is also tightened accordingly:

```javascript
> exactType.isValid({kind: 'PIG', name: 'Babe', age: 2});
false // The pig kind wasn't defined in our enum.
> exactType.isValid({kind: 'DOG', name: 'Lassie', age: 5});
true // But dog was.
```

## Container files

Avro defines a compact way to store encoded values. These [object container
files][object-container] hold serialized Avro records along with their schema.
Reading them is as simple as calling `createFileDecoder`, which will return a
[readable stream][rstream] of decoded records:

```javascript
avro.createFileDecoder('./persons.avro')
  .on('data', function (person) {
    if (person.address.city === 'San Francisco') {
      doSomethingWith(person);
    }
  });
```

In case we need the records' `type` or the file's codec, these are available by
listening to the `'metadata'` event. To access a file's header synchronously,
there also exists an `extractFileHeader` method. Finally, writing to an Avro
container file is likewise possible using `createFileEncoder`, which will
return a writable stream.


# Services

As well as defining an encoding, Avro defines a standard way of executing
remote procedure calls (_RPCs_), exposed via _services_. By leveraging these
services, we can implement portable and "type-safe" APIs:

+ Clients and servers can be implemented once and reused over many different
  transports (in-memory, TCP, HTTP, etc.).
+ All data flowing through the API is automatically validated: call arguments
  and return values are guaranteed to match the types specified in the API.

In this section, we'll walk through an example of building a simple link
management service similar to [bitly][].

## Defining a `Service`

The first step to creating a service is to define its _protocol_, describing
the available API calls and their signature. There are a couple ways of doing
so; we can write the [JSON declaration][json-protocol] directly, or we can use
Avro's [IDL syntax][idl] (which can then be compiled to JSON). The latter is
typically more convenient so we will use this here.

```java
/** A simple service to shorten URLs. */
protocol LinkService {

  /** Map a URL to an alias, throwing an error if it already exists. */
  null createAlias(string alias, string url);

  /** Expand an alias, returning null if the alias doesn't exist. */
  union { null, string } expandAlias(string alias);
}
```

With the above spec saved to a file, say `LinkService.avdl`, we can instantiate
the corresponding service as follows:

```javascript
// We first compile the IDL specification into a JSON protocol.
avro.assembleProtocol('./LinkService.avdl', function (err, protocol) {
  // From which we can create our service.
  const service = avro.Service.forProtocol(protocol);
});
```

The `service` object can then be used generate clients and servers, as
described in the following sections.

## Server implementation

So far, we haven't said anything about how our API's responses will be
computed. This is where servers come in, servers provide the logic powering our
API.

For each call declared in the protocol (`createAlias` and `expandAlias` above),
servers expose a similarly named handler (`onCreateAlias` and `onExpandAlias`)
with the same signature:

```javascript
const urlCache = new Map(); // We'll use an in-memory map to store links.

// We instantiate a server corresponding to our API and implement both calls.
const server = service.createServer()
  .onCreateAlias(function (alias, url, cb) {
    if (urlCache.has(alias)) {
      cb(new Error('alias already exists'));
    } else {
      urlCache.set(alias, url); // Add the mapping to the cache.
      cb();
    }
  })
  .onExpandAlias(function (alias, cb) {
    cb(null, urlCache.get(alias));
  });
```

Notice that no part of the above implementation is coupled to a particular
communication scheme (e.g. HTTP, TCP, AMQP): the code we wrote is
_transport-agnostic_.

## Calling our service

The simplest way to call our service is use an in-memory client, passing in our
`server` above as option to `service.createClient`:

```javascript
const client = service.createClient({server});

// We first send a request to create an alias.
client.createAlias('hn', 'https://news.ycombinator.com/', function (err) {
  // Which we can now expand.
  client.expandAlias('hn', function (err, url) {
    console.log(`hn is currently aliased to ${url}`);
  });
});
```

The above is handy for local testing or quick debugging. More interesting
perhaps is the ability to communicate with our server over any binary streams,
for example TCP sockets:

```javascript
const net = require('net');

// Set up the server to listen to incoming connections on port 24950.
net.createServer()
  .on('connection', function (con) { server.createChannel(con); })
  .listen(24950);

// And create a matching client:
const client = service.createClient({transport: net.connect(24950)});
```

Note that RPC calls messages are always sent asynchronously and in parallel:
requests do not block each other. Furthermore, responses are available as soon
as they are received from the server; the client keeps track of which calls are
pending and triggers the right callbacks as responses come back.

Both above transports (in-memory and TCP) have the additional property of being
[_stateful_][transports]: each connection can be used to exchange multiple
messages, making them particularly efficient (avoiding the overhead of
handshakes). These aren't the only kind though, it is possible to exchange
messages over stateless connections, for example HTTP:

```javascript
const http = require('http');

// Each HTTP request/response will correspond to a single API call.
http.createServer()
  .on('request', function (req, res) {
    server.createChannel(function (cb) { cb(null, res); return req; });
  })
  .listen(8080);

// Similarly, an HTTP client:
const client = service.createClient({transport: function (cb) {
  return http.request({method: 'POST', port: 8080})
    .on('response', function (res) { cb(null, err); })
    .on('error', cb);
}});
```


# Next steps

The [API documentation](Api) provides a comprehensive list of available
functions and their options. The [Advanced usage section](Advanced-usage) goes
through a few more examples of advanced functionality.


[avro]: https://avro.apache.org/docs/1.8.0/index.html
[avro-schemas]: https://avro.apache.org/docs/1.8.0/spec.html#schemas
[json]: http://www.json.org/
[xml]: https://www.w3.org/XML/
[message-pack]: http://msgpack.org/index.html
[bitly]: https://bitly.com/
[json-protocol]: https://avro.apache.org/docs/1.8.0/spec.html#Protocol+Declaration
[transports]: https://avro.apache.org/docs/1.8.0/spec.html#Message+Transport
[idl]: https://avro.apache.org/docs/1.8.0/idl.html
[object-container]: https://avro.apache.org/docs/current/spec.html#Object+Container+Files
[rstream]: https://nodejs.org/api/stream.html#stream_class_stream_readable
