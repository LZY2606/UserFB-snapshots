'use strict';

// Regression tests for the `transformAttr` option.
//
// Every assertion inspects the parsed HTML (parse5), never the output string
// alone, so serialization bugs that still "look" right are caught. The same
// suite runs against the source entry point and both browser bundles.

const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const parse5 = require('parse5');

const ROOT = path.join(__dirname, '..');

function loadSource() {
  const xss = require('../lib');
  return {
    name: 'lib/index.js',
    xss: xss,
    FilterXSS: xss.FilterXSS,
    withResult: xss.filterXSSWithResult,
  };
}

function loadBundle(file) {
  const context = vm.createContext({ window: {}, console: console });
  const source = fs.readFileSync(path.join(ROOT, 'dist', file), 'utf8');
  vm.runInContext(source, context, { filename: file });
  const xss = context.window.filterXSS;
  assert.strictEqual(typeof xss, 'function', file + ' must expose window.filterXSS');
  return {
    name: 'dist/' + file,
    xss: xss,
    FilterXSS: xss.FilterXSS,
    withResult: xss.filterXSSWithResult,
  };
}

// Records produced inside a vm bundle come from that realm's Object
// constructor, so they must be flattened before a deep comparison.
function plain(value) {
  return JSON.parse(JSON.stringify(value));
}

function freshError() {
  return new Error('callback sentinel');
}

// Parse `html` and return the attrs of the single expected top-level element.
function parsedAttrs(html, tag) {
  const fragment = parse5.parseFragment(html);
  assert.strictEqual(
    fragment.childNodes.length,
    1,
    'output must be exactly one top-level node: ' + JSON.stringify(html)
  );
  const node = fragment.childNodes[0];
  assert.strictEqual(node.nodeName, tag, 'unexpected element in ' + JSON.stringify(html));
  assert.ok(Array.isArray(node.attrs), 'expected an element');
  return node.attrs;
}

// The whole attribute list must be exactly [{name, value}].
function assertSingleAttr(html, tag, name, value, label) {
  const attrs = parsedAttrs(html, tag);
  assert.strictEqual(
    attrs.length,
    1,
    (label || '') + ' expected exactly one attribute, got ' + JSON.stringify(attrs)
  );
  assert.strictEqual(attrs[0].name, name, (label || '') + ' attribute name changed');
  assert.strictEqual(
    attrs[0].value,
    value,
    (label || '') + ' attribute value changed: ' + JSON.stringify(attrs[0].value)
  );
}

function assertNoAttrs(html, tag, label) {
  const attrs = parsedAttrs(html, tag);
  assert.strictEqual(attrs.length, 0, (label || '') + ' expected no attributes');
}

function attrNames(html, tag) {
  return parsedAttrs(html, tag).map(function (attr) {
    return attr.name;
  });
}

function run(impl) {
  const xss = impl.xss;
  const opts = function (extra) {
    return Object.assign({ whiteList: { img: ['src', 'title'] } }, extra);
  };

  // ------------------------------------------------------------------
  // 1. Rewriting a legal URL value
  // ------------------------------------------------------------------
  let calls = 0;
  let seen = [];
  const rewritten = xss(
    '<img src="/img/logo.png" title="keep">',
    opts({
      transformAttr: function (tag, name, value) {
        calls++;
        seen.push([tag, name, value]);
        if (name === 'src') return 'https://cdn.example.com' + value.slice(4);
        return undefined;
      },
    })
  );
  assert.strictEqual(calls, 2, 'callback runs once per whitelisted attribute');
  assert.deepStrictEqual(seen, [
    ['img', 'src', '/img/logo.png'],
    ['img', 'title', 'keep'],
  ]);
  const plainAttrs = parsedAttrs(rewritten, 'img');
  assert.deepStrictEqual(plainAttrs, [
    { name: 'src', value: 'https://cdn.example.com/logo.png' },
    { name: 'title', value: 'keep' },
  ]);

  // Attribute names are passed lowercased, the value is the parser's value.
  let caseArgs = null;
  xss('<IMG TITLE="Mixed">', {
    whiteList: { img: ['title'] },
    transformAttr: function (tag, name, value) {
      caseArgs = [tag, name, value];
    },
  });
  assert.deepStrictEqual(caseArgs, ['img', 'title', 'Mixed']);

  // ------------------------------------------------------------------
  // 2. A dangerous scheme rewritten by the callback is still filtered
  // ------------------------------------------------------------------
  let dangerousCalls = 0;
  const dangerous = xss(
    '<img src="/ok.png">',
    opts({
      transformAttr: function (tag, name) {
        dangerousCalls++;
        return name === 'src' ? 'javascript:alert(1)' : undefined;
      },
    })
  );
  assert.strictEqual(dangerousCalls, 1);
  const dangerousAttrs = parsedAttrs(dangerous, 'img');
  assert.strictEqual(dangerousAttrs.length, 1);
  assert.strictEqual(dangerousAttrs[0].name, 'src');
  assert.strictEqual(
    dangerousAttrs[0].value,
    '',
    'safeAttrValue must reject the rewritten javascript: URL'
  );

  // The callback cannot smuggle HTML in either.
  const smuggled = xss(
    '<img title="t">',
    opts({
      transformAttr: function () {
        return '"><script>alert(1)</script>';
      },
    })
  );
  assertSingleAttr(smuggled, 'img', 'title', '"><script>alert(1)</script>', 'smuggle');

  // ------------------------------------------------------------------
  // 3. null deletes, the empty string keeps an empty attribute
  // ------------------------------------------------------------------
  let nullCalls = 0;
  const deleted = xss(
    '<img src="/a.png" title="gone">',
    opts({
      transformAttr: function (tag, name) {
        nullCalls++;
        return name === 'title' ? null : undefined;
      },
    })
  );
  assert.strictEqual(nullCalls, 2);
  assert.deepStrictEqual(attrNames(deleted, 'img'), ['src']);

  const deletedAll = xss(
    '<img title="gone">',
    opts({
      transformAttr: function () {
        return null;
      },
    })
  );
  assertNoAttrs(deletedAll, 'img', 'null removes the whole attribute');

  const empty = xss(
    '<img src="/a.png" title="t">',
    opts({
      transformAttr: function (tag, name) {
        return name === 'title' ? '' : undefined;
      },
    })
  );
  // An empty string is not deletion: the attribute is still emitted with no value.
  assert.deepStrictEqual(parsedAttrs(empty, 'img'), [
    { name: 'src', value: '/a.png' },
    { name: 'title', value: '' },
  ]);

  // ------------------------------------------------------------------
  // 4. No whitelist permission -> callback is never consulted
  // ------------------------------------------------------------------
  let deniedCalls = 0;
  const deniedAttr = xss('<img src="/a.png" title="t">', {
    whiteList: { img: ['title'] },
    transformAttr: function (tag, name) {
      deniedCalls++;
      // `src` is not allowed and never reaches the callback; only `title` does.
      return name === 'title' ? 'X' : undefined;
    },
  });
  assert.strictEqual(deniedCalls, 1, 'only the whitelisted title reaches the callback');
  assert.deepStrictEqual(parsedAttrs(deniedAttr, 'img'), [{ name: 'title', value: 'X' }]);
  assert.deepStrictEqual(attrNames(deniedAttr, 'img'), ['title'], 'the denied src must not appear');

  let deniedTagCalls = 0;
  const deniedTag = xss('<a title="t">x</a>', {
    whiteList: { img: ['title'] },
    transformAttr: function () {
      deniedTagCalls++;
      return 'X';
    },
  });
  assert.strictEqual(deniedTagCalls, 0, 'attributes of non-whitelisted tags never reach the callback');
  assert.ok(deniedTag.indexOf('<a') === -1, 'non-whitelisted tag must stay escaped');

  // transformAttr cannot rename an attribute or grant an attribute permission.
  const renameAttempt = xss('<img title="t" onclick="evil()">', {
    whiteList: { img: ['title'] },
    transformAttr: function () {
      return 'x';
    },
  });
  assert.deepStrictEqual(parsedAttrs(renameAttempt, 'img'), [{ name: 'title', value: 'x' }]);
  assert.ok(/onclick/.test(renameAttempt) === false, 'denied attribute must stay removed');

  // ------------------------------------------------------------------
  // 5. Inherited callbacks are never executed
  // ------------------------------------------------------------------
  let inheritedCalls = 0;
  const inheritedOptions = Object.create({
    transformAttr: function () {
      inheritedCalls++;
      return 'inherited';
    },
  });
  inheritedOptions.whiteList = { img: ['title'] };
  const inheritedOut = xss('<img title="t">', inheritedOptions);
  assert.strictEqual(inheritedCalls, 0, 'prototype transformAttr must not run');
  assertSingleAttr(inheritedOut, 'img', 'title', 't', 'inherited callback');

  // Non-function own values are ignored, just like an unset option.
  const nonFunction = xss('<img title="t">', {
    whiteList: { img: ['title'] },
    transformAttr: 'not a function',
  });
  assertSingleAttr(nonFunction, 'img', 'title', 't', 'non-function option');

  // ------------------------------------------------------------------
  // 6. Callback exceptions keep their identity; bad return types throw TypeError
  // ------------------------------------------------------------------
  const sentinel = freshError();
  assert.throws(
    function () {
      xss('<img title="t">', {
        whiteList: { img: ['title'] },
        transformAttr: function () {
          throw sentinel;
        },
      });
    },
    function (error) {
      return error === sentinel;
    },
    'the callback exception must be propagated unchanged'
  );

  [42, true, {}, ['x'], function () {}].forEach(function (bad) {
    assert.throws(
      function () {
        xss('<img title="t">', {
          whiteList: { img: ['title'] },
          transformAttr: function () {
            return bad;
          },
        });
      },
      function (error) {
        return error.name === 'TypeError';
      },
      'returning ' + Object.prototype.toString.call(bad) + ' must throw TypeError'
    );
  });

  let toStringCalls = 0;
  const coercible = {
    toString: function () {
      toStringCalls++;
      return 'coerced';
    },
  };
  assert.throws(
    function () {
      xss('<img title="t">', {
        whiteList: { img: ['title'] },
        transformAttr: function () {
          return coercible;
        },
      });
    },
    function (error) {
      return error.name === 'TypeError';
    },
    'objects must not be accepted through implicit toString'
  );
  assert.strictEqual(toStringCalls, 0, 'toString must not be called');

  // ------------------------------------------------------------------
  // 7. Exactly one callback and one safeAttrValue call per attribute
  // ------------------------------------------------------------------
  let countedCalls = 0;
  let safeCalls = 0;
  const counted = xss('<img src="/a.png" title="t" onclick="evil()">', {
    whiteList: { img: ['src', 'title'] },
    transformAttr: function () {
      countedCalls++;
    },
    safeAttrValue: function (tag, name, value, cssFilter) {
      safeCalls++;
      return xss.safeAttrValue(tag, name, value, cssFilter);
    },
  });
  assert.strictEqual(countedCalls, 2, 'callback must run once per input attribute');
  assert.strictEqual(safeCalls, 2, 'safeAttrValue must run once per attribute');
  assert.deepStrictEqual(parsedAttrs(counted, 'img'), [
    { name: 'src', value: '/a.png' },
    { name: 'title', value: 't' },
  ]);

  // ------------------------------------------------------------------
  // 8. Reusing one FilterXSS instance does not carry decisions over
  // ------------------------------------------------------------------
  let reuseCalls = 0;
  const instance = new impl.FilterXSS({
    whiteList: { img: ['title'] },
    transformAttr: function () {
      reuseCalls++;
      // Only the very first invocation deletes; later ones keep the value.
      return reuseCalls === 1 ? null : undefined;
    },
  });
  const first = instance.process('<img title="first">');
  const second = instance.process('<img title="second">');
  assertNoAttrs(first, 'img', 'first process should delete title');
  assertSingleAttr(second, 'img', 'title', 'second', 'second process must not inherit the deletion');
  assert.strictEqual(reuseCalls, 2, 'callback must be invoked on every process call');

  // ------------------------------------------------------------------
  // 9. filterXSSWithResult records null deletions with the pre-callback value
  // ------------------------------------------------------------------
  let resultCalls = 0;
  const removedResult = impl.withResult(
    '<img src="/a.png" title="a&amp;b">',
    opts({
      transformAttr: function (tag, name, value) {
        resultCalls++;
        if (name === 'title') {
          assert.strictEqual(value, 'a&amp;b', 'callback sees the parser value before entity handling');
          return null;
        }
        return undefined;
      },
    })
  );
  assert.strictEqual(resultCalls, 2, 'one callback per input attribute; filtering must not be executed a second time to collect records');
  assert.deepStrictEqual(parsedAttrs(removedResult.html, 'img'), [{ name: 'src', value: '/a.png' }]);
  assert.deepStrictEqual(plain(removedResult.removed), [
    { type: 'attr', tag: 'img', attr: 'title', value: 'a&amp;b' },
  ]);

  // Replacement and retention are not removals.
  const keptResult = impl.withResult(
    '<img src="/a.png" title="t">',
    opts({
      transformAttr: function (tag, name) {
        return name === 'src' ? 'https://cdn.example.com/x.png' : undefined;
      },
    })
  );
  assert.strictEqual(keptResult.removed.length, 0, 'rewrites and retained values are not recorded');
  assert.deepStrictEqual(parsedAttrs(keptResult.html, 'img'), [
    { name: 'src', value: 'https://cdn.example.com/x.png' },
    { name: 'title', value: 't' },
  ]);

  // Existing ignore-tag / ignore-attr records and user callbacks still work.
  let ignoredTagCalls = 0;
  let ignoredAttrCalls = 0;
  const mixedResult = impl.withResult(
    '<script>x</script><img src="/a.png" title="t" onclick="evil()">',
    {
      whiteList: { img: ['src', 'title'] },
      transformAttr: function (tag, name) {
        return name === 'title' ? null : undefined;
      },
      onIgnoreTag: function () {
        ignoredTagCalls++;
      },
      onIgnoreTagAttr: function () {
        ignoredAttrCalls++;
      },
    }
  );
  assert.strictEqual(ignoredTagCalls, 2, 'user onIgnoreTag still receives both script tags');
  assert.strictEqual(ignoredAttrCalls, 1, 'user onIgnoreTagAttr still receives onclick');
  // map() over a cross-realm array produces a cross-realm array too.
  const recordKeys = Array.from(mixedResult.removed).map(function (item) {
    return item.type + ':' + (item.tag || '') + ':' + (item.attr || '');
  });
  assert.deepStrictEqual(recordKeys, [
    'tag:script:',
    'tag:script:',
    'attr:img:title',
    'attr:img:onclick',
  ]);
  assert.deepStrictEqual(
    plain(mixedResult.removed[2]),
    { type: 'attr', tag: 'img', attr: 'title', value: 't' },
    'null deletion record keeps the existing attr shape and original value'
  );

  // ------------------------------------------------------------------
  // 10. Old onTagAttr raw-HTML shortcut keeps its meaning
  // ------------------------------------------------------------------
  let shortcutTransformCalls = 0;
  let shortcutSafeCalls = 0;
  const shortcut = xss('<a href="/x" title="t">z</a>', {
    whiteList: { a: ['href', 'title'] },
    onTagAttr: function (tag, name) {
      // Returning a string means "this is the trusted attribute HTML".
      if (name === 'href') return 'href="raw"';
      return undefined;
    },
    transformAttr: function () {
      shortcutTransformCalls++;
      return 'changed';
    },
    safeAttrValue: function (tag, name, value, cssFilter) {
      shortcutSafeCalls++;
      return xss.safeAttrValue(tag, name, value, cssFilter);
    },
  });
  assert.strictEqual(shortcutTransformCalls, 1, 'shortcut attributes skip transformAttr');
  assert.strictEqual(shortcutSafeCalls, 1, 'shortcut attributes skip safeAttrValue');
  const shortcutAttrs = parsedAttrs(shortcut, 'a');
  assert.deepStrictEqual(shortcutAttrs, [
    { name: 'href', value: 'raw' },
    { name: 'title', value: 'changed' },
  ]);

  // ------------------------------------------------------------------
  // 11. Quotes, entities and angle brackets in both serialization modes
  // ------------------------------------------------------------------
  const quoteCases = [
    { label: 'single quote', input: "a'b", expected: "a'b" },
    { label: 'double quote', input: 'a"b', expected: 'a"b' },
    { label: 'both quotes', input: 'a\'b"c', expected: 'a\'b"c' },
    // Existing entity: decoded once by the browser, not re-encoded.
    { label: 'existing entity', input: 'a&amp;b', expected: 'a&b' },
    // Numeric entities are normalized once by the current value path.
    { label: 'numeric apostrophe entity', input: '&#39;', expected: "'" },
    { label: 'numeric angle entity', input: '&#60;x&#62;', expected: '<x>' },
    { label: 'existing angle entity', input: '&lt;x&gt;', expected: '<x>' },
    { label: 'raw angle brackets', input: '<b>bold</b>', expected: '<b>bold</b>' },
  ];

  [false, true].forEach(function (singleQuoted) {
    quoteCases.forEach(function (testCase) {
      const extra = singleQuoted ? { singleQuotedAttributeValue: true } : {};
      const out = xss(
        '<img title="orig">',
        Object.assign(
          { whiteList: { img: ['title'] } },
          extra,
          {
            transformAttr: function () {
              return testCase.input;
            },
          }
        )
      );
      assertSingleAttr(
        out,
        'img',
        'title',
        testCase.expected,
        (singleQuoted ? 'single-quoted' : 'double-quoted') + ' / ' + testCase.label
      );
      // No nesting, no extra elements, no extra attributes from the value.
      assert.strictEqual(
        (out.match(/<img/g) || []).length,
        1,
        'exactly one img tag expected: ' + JSON.stringify(out)
      );
    });
  });

  // The standalone safeAttrValue export and the default double-quote syntax
  // remain unchanged.
  assert.strictEqual(xss.safeAttrValue('img', 'title', 'a"b<c'), 'a&quot;b&lt;c');
  assert.strictEqual(xss('<img title="ordinary">', { whiteList: { img: ['title'] } }), '<img title="ordinary">');
  assert.strictEqual(
    xss('<img title="ordinary">', {
      whiteList: { img: ['title'] },
      singleQuotedAttributeValue: true,
    }),
    "<img title='ordinary'>"
  );

  // Without the option the processing chain is unchanged.
  assert.strictEqual(
    xss('<img title="ordinary">', { whiteList: { img: ['title'] } }),
    xss('<img title="ordinary">', { whiteList: { img: ['title'] }, transformAttr: undefined })
  );
}

const implementations = [loadSource(), loadBundle('xss.js'), loadBundle('xss.min.js')];

for (const impl of implementations) {
  run(impl);
  console.log('attribute transform: ' + impl.name + ' OK');
}

// Prototype pollution of the host realm is only meaningful for the source
// implementation; the vm bundles have their own realm.
(function () {
  const xss = require('../lib');
  const previous = Object.prototype.transformAttr;
  let pollutionCalls = 0;
  Object.prototype.transformAttr = function () {
    pollutionCalls++;
    return 'polluted';
  };
  try {
    const out = xss('<img title="t">', { whiteList: { img: ['title'] } });
    assert.strictEqual(pollutionCalls, 0, 'polluted Object.prototype transformAttr must not run');
    assertSingleAttr(out, 'img', 'title', 't', 'prototype pollution');
    const result = xss.filterXSSWithResult('<img title="t">', { whiteList: { img: ['title'] } });
    assert.strictEqual(pollutionCalls, 0, 'pollution must not run through filterXSSWithResult');
    assert.deepStrictEqual(plain(result.removed), []);
    assert.strictEqual(
      Object.prototype.hasOwnProperty.call(xss, 'transformAttr'),
      false,
      'no global default transformAttr export'
    );
  } finally {
    if (previous === undefined) delete Object.prototype.transformAttr;
    else Object.prototype.transformAttr = previous;
  }
  console.log('attribute transform: prototype pollution OK');
})();

console.log('attribute transform: all implementations OK');
