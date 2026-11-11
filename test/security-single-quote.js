/**
 * Security regression checks for single-quoted attribute output.
 *
 * Run directly with:
 *   node test/security-single-quote.js
 *
 * This file deliberately uses a deterministic attribute tokenizer instead of
 * searching the output for a word such as "onmouseover": the attack text is
 * expected to remain inside the title value, so a substring search cannot
 * distinguish "one title attribute whose value contains that text" from
 * "a second onmouseover attribute".
 */

"use strict";

var assert = require("assert");
var xss = require("../lib");

var ATTACK_VALUE = "ok' onmouseover='alert(1)";

/**
 * Tokenize attributes in a start tag. Quoted values are consumed as a single
 * unit, so an encoded quote cannot be mistaken for an attribute boundary.
 *
 * @param {String} tagHtml e.g. `<a title='x'>`
 * @return {Object} { tagName, attributes: [{name, value, quote}] }
 */
function tokenizeAttributes(tagHtml) {
  assert.equal(tagHtml[0], "<", "expected a start tag");
  assert.equal(tagHtml[tagHtml.length - 1], ">", "expected a closed tag");

  var end = tagHtml.length - 1;
  var i = 1;
  while (i < end && /\s/.test(tagHtml[i])) i++;

  var nameStart = i;
  while (i < end && !/[\s/>]/.test(tagHtml[i])) i++;
  var tagName = tagHtml.slice(nameStart, i).toLowerCase();

  var attributes = [];
  while (i < end) {
    while (i < end && /[\s/]/.test(tagHtml[i])) i++;
    if (i >= end) break;

    var attrStart = i;
    while (i < end && !/[\s=/>]/.test(tagHtml[i])) i++;
    var attrName = tagHtml.slice(attrStart, i);
    assert.ok(attrName, "expected an attribute name");

    var j = i;
    while (j < end && /\s/.test(tagHtml[j])) j++;

    if (j < end && tagHtml[j] === "=") {
      j++;
      while (j < end && /\s/.test(tagHtml[j])) j++;

      var quote = "";
      var value;
      if (tagHtml[j] === "'" || tagHtml[j] === '"') {
        quote = tagHtml[j];
        var quoteEnd = tagHtml.indexOf(quote, j + 1);
        assert.ok(
          quoteEnd !== -1 && quoteEnd <= end,
          "unterminated quoted attribute value"
        );
        value = tagHtml.slice(j + 1, quoteEnd);
        j = quoteEnd + 1;
      } else {
        var valueStart = j;
        while (j < end && !/\s/.test(tagHtml[j]) && tagHtml[j] !== ">") j++;
        value = tagHtml.slice(valueStart, j);
      }

      attributes.push({
        name: attrName.toLowerCase(),
        value: value,
        quote: quote,
      });
      i = j;
    } else {
      attributes.push({
        name: attrName.toLowerCase(),
        value: "",
        quote: "",
      });
    }
  }

  return { tagName: tagName, attributes: attributes };
}

/**
 * Decode HTML entities exactly once. Because scanning continues after the
 * `&...;` that was just decoded, output such as `&amp;#39;` becomes the text
 * `&#39;` and is not decoded a second time.
 *
 * @param {String} text
 * @return {String}
 */
function decodeEntitiesOnce(text) {
  var out = "";

  for (var i = 0; i < text.length; i++) {
    if (text[i] === "&") {
      var semi = text.indexOf(";", i + 1);
      if (semi !== -1) {
        var decoded = decodeEntityBody(text.slice(i + 1, semi));
        if (decoded !== null) {
          out += decoded;
          i = semi;
          continue;
        }
      }
    }
    out += text[i];
  }

  return out;
}

function decodeEntityBody(body) {
  if (body === "amp") return "&";
  if (body === "lt") return "<";
  if (body === "gt") return ">";
  if (body === "quot") return '"';
  if (body === "apos") return "'";

  if (body[0] === "#") {
    var code =
      body[1] === "x" || body[1] === "X"
        ? parseInt(body.slice(2), 16)
        : parseInt(body.slice(1), 10);
    if (isFinite(code)) return String.fromCharCode(code);
  }

  return null;
}

/**
 * Assert that the first tag in `html` has exactly one attribute with the
 * expected name and decoded value.
 */
function assertSingleAttribute(html, name, expectedValue) {
  var tag = html.slice(0, html.indexOf(">") + 1);
  var tokenized = tokenizeAttributes(tag);

  assert.equal(
    tokenized.attributes.length,
    1,
    "expected exactly one attribute, got " +
      JSON.stringify(tokenized.attributes) +
      " in " +
      html
  );
  assert.equal(tokenized.attributes[0].name, name, "unexpected attribute name");
  assert.equal(
    decodeEntitiesOnce(tokenized.attributes[0].value),
    expectedValue,
    "unexpected decoded attribute value"
  );

  return tokenized;
}

function processSingle(html) {
  return new xss.FilterXSS({ singleQuotedAttributeValue: true }).process(html);
}

function processDouble(html) {
  return new xss.FilterXSS().process(html);
}

function run() {
  // --- Attack inputs: one title attribute, no second attribute. ----------
  var attackInputs = [
    { input: '<a title="ok\' onmouseover=\'alert(1)">x</a>', value: ATTACK_VALUE },
    { input: '<a title="ok&#39; onmouseover=&#39;alert(1)">x</a>', value: ATTACK_VALUE },
    { input: '<a title="ok&#x27; onmouseover=&#x27;alert(1)">x</a>', value: ATTACK_VALUE },
    { input: '<a title="ok&apos; onmouseover=&apos;alert(1)">x</a>', value: ATTACK_VALUE },
    { input: "<a title='ok&#39; onmouseover=&#39;alert(1)'>x</a>", value: ATTACK_VALUE },
    // An unquoted source value has no whitespace to split on. The apostrophe
    // entities still must not create an onmouseover attribute.
    { input: "<a title=ok&#39;onmouseover=&#39;alert(1)>x</a>", value: "ok'onmouseover='alert(1)" },
  ];

  attackInputs.forEach(function (testCase) {
    var single = processSingle(testCase.input);
    var tokenized = assertSingleAttribute(single, "title", testCase.value);
    assert.equal(tokenized.attributes[0].quote, "'");
    assert.ok(
      tokenized.attributes.every(function (attr) {
        return attr.name !== "onmouseover";
      }),
      "attack output must not contain an onmouseover attribute"
    );

    // The default double-quoted mode stays safe and keeps one attribute too.
    var double = processDouble(testCase.input);
    assertSingleAttribute(double, "title", testCase.value);
  });

  // Exact output for the two primary attack inputs.
  assert.equal(
    processSingle('<a title="ok\' onmouseover=\'alert(1)">x</a>'),
    "<a title='ok&#39; onmouseover=&#39;alert(1)'>x</a>"
  );
  assert.equal(
    processSingle('<a title="ok&#39; onmouseover=&#39;alert(1)">x</a>'),
    "<a title='ok&#39; onmouseover=&#39;alert(1)'>x</a>"
  );

  // --- Entity conversion boundaries. ------------------------------------
  // Only one decode pass: &amp;#39; stays text, the later &#39; is an apostrophe.
  var doubleEncoded = processSingle(
    '<a title="ok&amp;#39; onmouseover=&#39;alert(1)">x</a>'
  );
  assert.equal(
    doubleEncoded,
    "<a title='ok&amp;#39; onmouseover=&#39;alert(1)'>x</a>"
  );
  var doubleEncodedToken = assertSingleAttribute(
    doubleEncoded,
    "title",
    "ok&#39; onmouseover='alert(1)"
  );
  assert.ok(
    doubleEncodedToken.attributes[0].value.indexOf("&amp;#39;") !== -1,
    "&amp;#39; must not be decoded twice"
  );

  // Existing double-quote entity: decoded to " and re-encoded for the active
  // single-quote output, in both modes.
  var doubleQuoteEntity = '<a title="ok&quot; onmouseover=&quot;alert(1)">x</a>';
  var doubleQuoteValue = 'ok" onmouseover="alert(1)';
  assert.equal(
    processSingle(doubleQuoteEntity),
    "<a title='ok&quot; onmouseover=&quot;alert(1)'>x</a>"
  );
  assertSingleAttribute(processSingle(doubleQuoteEntity), "title", doubleQuoteValue);
  assertSingleAttribute(processDouble(doubleQuoteEntity), "title", doubleQuoteValue);

  // Decimal and hexadecimal double quote entities take the same path.
  var decimalDoubleQuote = '<a title="ok&#34; onmouseover=&#34;alert(1)">x</a>';
  var hexDoubleQuote = '<a title="ok&#x22; onmouseover=&#x22;alert(1)">x</a>';
  [decimalDoubleQuote, hexDoubleQuote].forEach(function (input) {
    assert.equal(
      processSingle(input),
      "<a title='ok&quot; onmouseover=&quot;alert(1)'>x</a>"
    );
    assertSingleAttribute(processSingle(input), "title", doubleQuoteValue);
    assertSingleAttribute(processDouble(input), "title", doubleQuoteValue);
  });

  // Normal Chinese text is untouched.
  assert.equal(processSingle('<a title="中文">x</a>'), "<a title='中文'>x</a>");
  assertSingleAttribute(processSingle('<a title="中文">x</a>'), "title", "中文");

  // Literal replacement-template text must not be expanded by
  // String.prototype.replace. `$&`, `$'` and a backtick all stay literal.
  var dollarInput = "<a title=\"$& $' `\">x</a>";
  assert.equal(processDouble(dollarInput), "<a title=\"$& $' `\">x</a>");
  assertSingleAttribute(processDouble(dollarInput), "title", "$& $' `");
  assert.equal(
    processSingle(dollarInput),
    "<a title='$& $&#39; `'>x</a>"
  );
  assertSingleAttribute(processSingle(dollarInput), "title", "$& $' `");

  // Standalone values make the "no template expansion" assertion direct.
  assert.equal(processDouble('<a title="$&">x</a>'), '<a title="$&">x</a>');
  assert.equal(
    processDouble("<a title=\"$'\">x</a>"),
    "<a title=\"$'\">x</a>"
  );

  // --- Default double-quoted mode keeps its existing output. ------------
  assert.equal(
    processDouble('<a title="xx">x</a>'),
    '<a title="xx">x</a>'
  );
  assert.equal(
    processSingle('<a title="xx">x</a>'),
    "<a title='xx'>x</a>"
  );
  assert.equal(
    processDouble('<a title="ok\' onmouseover=\'alert(1)">x</a>'),
    '<a title="ok\' onmouseover=\'alert(1)">x</a>'
  );

  // --- Callback paths. ---------------------------------------------------
  // A custom safeAttrValue result is still escaped for the active quote.
  var customSingle = new xss.FilterXSS({
    singleQuotedAttributeValue: true,
    safeAttrValue: function () {
      return ATTACK_VALUE;
    },
  }).process('<a title="x">x</a>');
  assert.equal(
    customSingle,
    "<a title='ok&#39; onmouseover=&#39;alert(1)'>x</a>"
  );
  assertSingleAttribute(customSingle, "title", ATTACK_VALUE);

  var customDouble = new xss.FilterXSS({
    safeAttrValue: function () {
      return 'ok" onmouseover="alert(1)';
    },
  }).process('<a title="x">x</a>');
  assert.equal(
    customDouble,
    '<a title="ok&quot; onmouseover=&quot;alert(1)">x</a>'
  );
  assertSingleAttribute(customDouble, "title", 'ok" onmouseover="alert(1)');

  // Raw HTML returned by hooks is trusted by contract and is not re-escaped.
  var hookAttr = new xss.FilterXSS({
    singleQuotedAttributeValue: true,
    onTagAttr: function (tag, name) {
      if (name === "title") return "title='hook' onmouseover='trusted'";
    },
  }).process('<a title="x">link</a>');
  assert.equal(hookAttr, "<a title='hook' onmouseover='trusted'>link</a>");

  var hookIgnoredAttr = new xss.FilterXSS({
    singleQuotedAttributeValue: true,
    onIgnoreTagAttr: function (tag, name) {
      if (name === "data-x") return "data-x='hook' onmouseover='trusted'";
    },
  }).process('<a data-x="1">link</a>');
  assert.equal(hookIgnoredAttr, "<a data-x='hook' onmouseover='trusted'>link</a>");

  var hookTag = new xss.FilterXSS({
    singleQuotedAttributeValue: true,
    onTag: function (tag, html, options) {
      if (tag === "a" && !options.isClosing) {
        return "<a title='hook' onmouseover='trusted'>";
      }
    },
  }).process('<a title="x">link</a>');
  assert.equal(hookTag, "<a title='hook' onmouseover='trusted'>link</a>");

  // --- Existing URL and stripIgnoreTag behavior. ------------------------
  var filteredHref = processSingle(
    '<a href="javascript:alert(1)" title="ok">link</a>'
  );
  assert.equal(filteredHref, "<a href title='ok'>link</a>");
  var filteredHrefTag = tokenizeAttributes(filteredHref.slice(0, filteredHref.indexOf(">") + 1));
  assert.equal(filteredHrefTag.attributes[0].name, "href");
  assert.equal(filteredHrefTag.attributes[0].value, "");
  assert.equal(filteredHrefTag.attributes[1].name, "title");
  assert.equal(filteredHrefTag.attributes[1].value, "ok");

  assert.equal(
    new xss.FilterXSS({
      singleQuotedAttributeValue: true,
      stripIgnoreTag: true,
    }).process('<x>yy</x><a title="xx">link</a>'),
    "yy<a title='xx'>link</a>"
  );

  console.log("single-quote boundary: PASS");
}

if (require.main === module) {
  run();
}

module.exports = {
  tokenizeAttributes: tokenizeAttributes,
  decodeEntitiesOnce: decodeEntitiesOnce,
};
