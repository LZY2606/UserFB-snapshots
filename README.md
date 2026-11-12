# jmespath.js

[![Build Status](https://travis-ci.org/jmespath/jmespath.js.png?branch=master)](https://travis-ci.org/jmespath/jmespath.js)

jmespath.js is a javascript implementation of JMESPath,
which is a query language for JSON.  It will take a JSON
document and transform it into another JSON document
through a JMESPath expression.

Using jmespath.js is really easy.  There's a single function
you use, `jmespath.search`:


```
> var jmespath = require('jmespath');
> jmespath.search({foo: {bar: {baz: [0, 1, 2, 3, 4]}}}, "foo.bar.baz[2]")
2
```

In the example we gave the ``search`` function input data of
`{foo: {bar: {baz: [0, 1, 2, 3, 4]}}}` as well as the JMESPath
expression `foo.bar.baz[2]`, and the `search` function evaluated
the expression against the input data to produce the result ``2``.

The JMESPath language can do a lot more than select an element
from a list.  Here are a few more examples:

```
> jmespath.search({foo: {bar: {baz: [0, 1, 2, 3, 4]}}}, "foo.bar")
{ baz: [ 0, 1, 2, 3, 4 ] }

> jmespath.search({"foo": [{"first": "a", "last": "b"},
                           {"first": "c", "last": "d"}]},
                  "foo[*].first")
[ 'a', 'c' ]

> jmespath.search({"foo": [{"age": 20}, {"age": 25},
                           {"age": 30}, {"age": 35},
                           {"age": 40}]},
                  "foo[?age > `30`]")
[ { age: 35 }, { age: 40 } ]
```

## Lexical bindings with let

API responses commonly pair a shared configuration block with a list of
records.  Once a projection like `rows[?...]` is entered, `@` refers to a
single record and the outer configuration is no longer reachable.  A
`let` expression binds a name to a value once, and that name stays in
scope for the whole body, including nested projections.  The syntax is:

```
let $name = <expression>, $other = <expression> in <expression>
```

Bindings evaluate left to right against the current node on entry, later
right-hand sides may reference earlier names, and an inner `let` may
shadow an outer one.  A `null` binding is still a declared value;
referencing a name that was never bound throws an `UndefinedVariable`
error.

Keeping a top-level threshold while filtering the records:

```
> var response = {
...   threshold: 2,
...   rows: [{id: 'a', n: 1}, {id: 'b', n: 3}]
... };
> jmespath.search(response,
...   "let $limit = threshold in rows[?n > $limit].id")
[ 'b' ]
```

A more typical paginated payload, with a shared config block used inside
both a filter and a multiselect:

```
> var payload = {
...   config: {region: 'us-east', minScore: 80},
...   records: [
...     {name: 'alpha', score: 75, tags: ['x']},
...     {name: 'beta',  score: 91, tags: ['x', 'y']}
...   ]
... };
> jmespath.search(payload,
...   "let $cfg = config in " +
...   "records[?score >= $cfg.minScore].{name: name, region: $cfg.region}")
[ { name: 'beta', region: 'us-east' } ]
```

## Expression references capture bindings

An expression reference created with `&` captures the bindings visible at
its creation point.  When `map`, `sort_by`, `max_by` and friends execute
it later, `@` is still the element being processed, while variables come
from the captured scope.  References can themselves be stored in
variables:

```
> var payload = {
...   config: 'outer',
...   rows: [{n: 1}, {n: 2}]
... };
> jmespath.search(payload,
...   "let $cfg = config in map(&[n, $cfg], rows)")
[ [ 1, 'outer' ], [ 2, 'outer' ] ]
```

Shadowing the same name after the reference is created does not change
what the captured reference sees, which distinguishes lexical capture
from dynamic lookup:

```
> jmespath.search(payload,
...   "let $cfg = config in " +
...   "let $ref = &$cfg in " +
...   "let $cfg = `\"inner\"` in map($ref, rows)")
[ 'outer', 'outer' ]
```

Outside a binding structure `let` and `in` remain ordinary field names,
and `$` characters inside quoted identifiers or string literals are
untouched, so existing queries keep working unchanged.

## More Resources

The example above only show a small amount of what
a JMESPath expression can do.  If you want to take a
tour of the language, the *best* place to go is the
[JMESPath Tutorial](http://jmespath.org/tutorial.html).

One of the best things about JMESPath is that it is
implemented in many different programming languages including
python, ruby, php, lua, etc.  To see a complete list of libraries,
check out the [JMESPath libraries page](http://jmespath.org/libraries.html).

And finally, the full JMESPath specification can be found
on the [JMESPath site](http://jmespath.org/specification.html).
