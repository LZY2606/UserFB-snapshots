<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Overview](#overview)
  - [What is Avro?](#what-is-avro)
  - [Why JavaScript?](#why-javascript)
- [Browser support](#browser-support)
  - [Compatibility table](#compatibility-table)
  - [Distributions](#distributions)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Overview

`avsc` is a pure JavaScript implementation of the [Avro
specification][avro-specification] (compliant with the current version,
`1.8.1`).

## What is Avro?

Avro is a data serialization system. It is part of the [Apache Software
Foundation][asf] and is widely used both in offline systems (e.g. in
combination with Hadoop) and online (e.g. as message encoding for Kafka).

Quoting the [official documentation][avro-documentation]:

> Avro provides:
>
> + Rich data structures.
> + A compact, fast, binary data format.
> + A container file, to store persistent data.
> + Remote procedure call (RPC).
> + Simple integration with dynamic languages. Code generation is not required to
>   read or write data files nor to use or implement RPC protocols. Code
>   generation as an optional optimization, only worth implementing for
>   statically typed languages.

Avro is roughly similar to [Protocol Buffers][protocol-buffers], in that they
both deal with structured data (as opposed to JSON and [MessagePack][] for
example). This allows for various benefits: for example built-in data
validation, and faster, more compact encodings.


## Why JavaScript?

It turns out JavaScript is a great fit for Avro. JavaScript's dynamic nature
lets us generate optimized code for each data type. This lets us be often
(much) faster than JSON (see the [Benchmarks page](Benchmarks)).


# Browser support

## Compatibility table

`avsc` is tested against a variety of browsers using [Sauce Labs][saucelabs].

[![Browser compatibility table](https://saucelabs.com/browser-matrix/mtth.svg)](https://saucelabs.com/u/buffer)


## Distributions

`avsc` comes in multiple flavors to help minimize code size. Depending on which
module you `require`, you will get a different distribution:

+ `'avsc/etc/browser/avsc'`: the full distribution (~51kB minified and
  compressed), including serialization, protocols, and Avro file support. This
  is also the default distribution you get when you `require('avsc')` directly.
+ `'avsc/etc/browser/avsc-protocols'`: a slightly lighter distribution (~34kB)
  which doesn't include file support.
+ `'avsc/etc/browser/avsc-types'`: the lightest distribution (~20kB) which
  only includes serialization support.

Note that in all the above cases, the core `avsc` libraries only represent a
fraction of the total size (~15kB with everything included). If you were
already using some of `avsc`'s dependencies (e.g. `'events'`), your bundle will
increase by less than the sizes indicated.


[avro-specification]: https://avro.apache.org/docs/current/spec.html
[asf]: http://www.apache.org/
[avro-documentation]: http://avro.apache.org/docs/current/
[saucelabs]: https://saucelabs.com/
[protocol-buffers]: https://developers.google.com/protocol-buffers/
[messagepack]: http://msgpack.org/index.html
