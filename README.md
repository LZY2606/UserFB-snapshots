# finder

![finder](https://medv.io/assets/finder.png)

**The CSS Selector Generator**

[![Test](https://github.com/antonmedv/finder/actions/workflows/test.yml/badge.svg)](https://github.com/antonmedv/finder/actions/workflows/test.yml)
[![JSR](https://jsr.io/badges/@medv/finder)](https://jsr.io/@medv/finder)

## Features

* Generates **shortest** CSS selectors
* **Unique** CSS selectors per page
* Stable and **robust** CSS selectors
* Size: **1.5kb** (minified & gzipped)

## Install

```bash
npm install @medv/finder
```

## Usage 

```ts
import { finder } from '@medv/finder';

document.addEventListener('click', (event) => {
  const selector = finder(event.target);
});
```

## Example

An example of a generated selector:

```css
.blog > article:nth-of-type(3) .add-comment
```

## Configuration

```js
const selector = finder(event.target, {
  root: document.body,
  timeoutMs: 1000,
});
```

### root

Defines the root of the search. Defaults to `document.body`.

### timeoutMs

Timeout to search for a selector. Defaults to `1000ms`. After the timeout, finder fallbacks to `nth-child` selectors.

### className

Function that determines if a class name may be used in a selector. Defaults to a word-like class names.

You can extend the default behaviour wrapping the `className` function:

```js
import { finder, className } from '@medv/finder';

finder(event.target, {
  className: name => className(name) || name.startsWith('my-class-'),
});
```

### tagName

Function that determines if a tag name may be used in a selector. Defaults to `() => true`.

### attr

Function that determines if an attribute may be used in a selector. Defaults to a word-like attribute names and values.

You can extend the default behaviour wrapping the `attr` function:

```js
import { finder, attr } from '@medv/finder';

finder(event.target, {
  attr: (name, value) => attr(name, value) || name.startsWith('data-my-attr-'),
});
```

### idName

Function that determines if an id name may be used in a selector. Defaults to a word-like id names.

### seedMinLength

Minimum length of levels in fining selector. Defaults to `3`.

### optimizedMinLength

Minimum length for optimising selector. Defaults to `2`.

## Shadow DOM

`finder` returns one CSS string, which only makes sense inside a single root: a
selector generated for an element in a shadow root cannot be replayed from
`document`. For components built from nested (open) shadow roots, use
`finderPath` and `resolvePath`.

### finderPath

```ts
import { finderPath, resolvePath } from '@medv/finder';

const path = finderPath(target, { root: document });
// e.g. ['x-card:nth-of-type(2)', '.detail', '.price']
const element = resolvePath(document, path); // === target
```

`finderPath(input, options?)` returns an array of CSS selectors, one per tree on
the way from the start to the target.

* `options.root` sets the start and may be a `Document`, `Element` or
  `ShadowRoot`; it defaults to the current `document`.
* Every segment except the last matches exactly one shadow host inside the
  current root, then replay descends into that host's **open** shadow root. The
  last segment matches exactly the target inside its own root.
* No shadow-piercing syntax is emitted; each segment is scoped to its own root,
  so identically named elements (e.g. `.price`) in different components produce
  different paths and never satisfy a uniqueness check by borrowing a selector
  from outside the root.
* The id/class/tag/attribute filtering and the positional
  (`nth-of-type`/`nth-child`) fallback are the same as `finder`, so a host with
  no usable name is still located by its position.
* It returns `[]` when the start is itself the target.
* It throws when the target is not reachable from the start: the target is
  detached or outside the start's tree, or the only route crosses a **closed**
  shadow root. A slotted (light DOM) element stays in its host's light tree and
  is reached without entering any shadow root.

### resolvePath

`resolvePath(root, path)` replays a path produced by `finderPath`.

* An empty path returns the start itself.
* It returns `null` if any segment matches zero or more than one element, is not
  valid CSS, or an intermediate host has no open shadow root — it never returns
  the first match of an ambiguous segment.

```ts
// Two identical components keep distinct, replayable paths.
const pathA = finderPath(priceA, { root: document });
const pathB = finderPath(priceB, { root: document });
resolvePath(document, pathA); // priceA
resolvePath(document, pathB); // priceB

// Restrict the search to one component's shadow root.
finderPath(priceB, { root: cardB.shadowRoot });
```

## License

[MIT](LICENSE)
