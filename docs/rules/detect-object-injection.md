# Detects "variable[key]" as a left- or right-hand assignment operand (`security/detect-object-injection`)

⚠️ This rule _warns_ in the ✅ `recommended` config.

<!-- end auto-generated rule header -->

JavaScript allows you to use expressions to access object properties in addition to using dot notation. So instead of writing this:

```js
object.name = 'foo';
```

You can write this:

```js
object['name'] = 'foo';
```

Square bracket notation allows any expression to be used in place of an identifier, so you can also do this:

```js
const key = 'name';
object[key] = 'foo';
```

By doing so, you've now obfuscated the property name from the reader, which makes it easy for a malicious actor to replace the value of `key` and change the behavior of the code.

This rule flags any expression in the form of `object[expression]` no matter where it occurs. Examples of patterns this will be flagged are:

```js
object[key] = value;

value = object[key];

doSomething(object[key]);
```

More information: [The Dangers of Square Bracket Notation](../the-dangers-of-square-bracket-notation.md)

## Options

### `allowNullPrototype`

Type: `boolean`
Default: `false`

The rule is enabled without any options by default, so this option is never
turned on implicitly. When it is explicitly set to `true`, a dynamic property
access such as `object[key]` is not reported if the analysis can prove that
`object` is an unmodified dictionary created with the built-in
`Object.create(null)`.

```js
// eslint.config.js
{
  rules: {
    'security/detect-object-injection': ['warn', { allowNullPrototype: true }],
  },
}
```

```js
const permissions = Object.create(null);
let alias = permissions;

alias[userKey] = true; // allowed: provenance follows the object, not the name
```

The proof is intentionally narrow:

- It follows object identity through declarations, ordinary assignments,
  parentheses, aliases, and conditional expressions. A conditional expression
  is only allowed when every branch yields a pristine dictionary.
- Shadowed `Object` bindings (parameters, locals, imports, and so on) are not
  treated as the built-in factory. Parameters, unknown call results, property
  reads, ordinary objects, and values captured from an outer scope are never
  considered proven dictionaries.
- Every alias of an object loses its proof when the prototype is changed with
  `Object.setPrototypeOf` or a `__proto__` write, when the object is passed to
  an unknown call, used as a method receiver, written into another object,
  returned, or captured by a closure. Reassigning a variable to a new
  `Object.create(null)` proves the new value only; old aliases stay invalid.
- `if`/`else` successors are allowed only when every path that reaches them
  proves the receiver safe. Branches that always return or throw do not
  contribute to the successor.
- Loops and `try` bodies are analysed conservatively. Assignments inside them
  invalidate proofs for the affected variables, so a proof that was already
  invalid before entering the construct is not reused. A receiver may still be
  accepted inside these constructs when the assignment invalidation leaves its
  proof intact. Writes that cannot be modelled precisely (for example, writes
  performed by unknown helper functions) are treated as escapes or
  invalidations rather than as proofs.
- Invalidation is per object: an unknown call in one branch does not invalidate
  unrelated dictionaries elsewhere in the file. Analysis state is created for
  each file scan and is not shared between lint runs.

