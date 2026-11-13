# HTML `<template>` tree construction in jsoup

jsoup's HTML parser implements the [WHATWG "in template" insertion
mode](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intemplate). Pages collected for
downstream expansion can therefore keep their unexpanded `<template>` structures instead of losing rows,
cells, or having nested forms close the outer form early.

## Representation

A `template` is an ordinary `org.jsoup.nodes.Element`. Its contents are its normal children — there is no
browser-style `content` property or `DocumentFragment` object. Everything that already works on an element
keeps working on a template:

- selectors: `doc.select("#t tr")`, `template.getElementsByTag("td")`
- traversal and cloning: `template.child(0)`, `template.clone()`
- serialisation: `template.outerHtml()`, `Jsoup.parse(template.outerHtml())`

## What the parser does

- **Head templates stay in head.** `<head><template>…</template></head>` keeps the template under `head`.
- **Each template keeps its own content mode.** Contents that open with `tr`, `td`/`th`, `col`,
  `caption`, `colgroup`, or `tbody`/`thead`/`tfoot` are handled with the matching table context. No `table`
  element is invented to hold them; a leading cell implies only its `tr`, exactly like the table rules imply
  rows in other contexts.
- **Ordinary content uses the "in body" rules.** `style`, `title`, `script`, `textarea`, and friends keep
  their own tokeniser states (raw text, RCDATA, script data, …).
- **Templates are boundaries.** Scope searches, table-stack clears, and active-formatting reconstruction
  stop at a template. Mismatched end tags inside a template cannot pop an outer table, cell, or formatting
  element.
- **Foster parenting is scoped.** Where a node is fostered depends on the nearest `table` versus the nearest
  `template` on the stack of open elements; content fostered inside a template never escapes in front of an
  outer table.
- **Nested templates are independent.** Closing a nested template restores the outer template's content
  mode; closing the outer one restores the real outer context. Unclosed templates at every depth are
  finalised at EOF, and the EOF token is processed only once.
- **`html`/`body` start tags inside a template** do not merge their attributes onto the document's root or
  active body.
- **Forms are isolated by template boundaries.** A form inside a template is preserved in the DOM, but
  neither its controls nor any control parsed while template content is open are registered on any
  `FormElement.elements()` list — including the template's own form. After leaving the template, controls
  associate with the outer form again. This is a parse-time association rule only; jsoup does not recompute
  form ownership after later DOM moves.
- **Fragments work too.** `Parser.parseFragment(html, templateElement, baseUri)` lets a template element act
  as the context, so bare rows, cells, and columns parse directly. The context element and its ancestors are
  never modified. Context-less fragments and nested templates inside fragments are handled as well.
- **State never leaks between parses.** Reusing one `Parser` instance resets template modes, formatting
  markers, and form state for every new document or fragment.
- **Errors use the existing mechanism.** A stray `</template>` and an unclosed template at EOF are reported
  through the normal `ParseErrorList`; `setTrackErrors(max)` keeps capping the count and each error keeps its
  reader position.

The XML parser is intentionally unchanged: in XML a template is just another element and the HTML insertion
modes do not apply. HTML documents without templates keep their previous behaviour.

## Scope

Only ordinary HTML elements are covered. SVG, MathML, script execution, and declarative shadow DOM are not
part of this support.

## Example

```java
Document doc = Jsoup.parse(
    "<head><template id=\"t\"><tr id=\"r\"><td id=\"c\">draft</td></tr></template></head>");

Element template = doc.head().getElementById("t");
Element row = template.child(0);          // tr, a direct child of the template
Element cell = row.child(0);              // td, a direct child of the tr
System.out.println(cell.text());          // draft
```

Form isolation:

```java
Document doc = Jsoup.parse(
    "<body><form id=\"outer\"><input id=\"a\">"
  + "<template id=\"t\"><form id=\"inner\"><input id=\"b\"></form></template>"
  + "<input id=\"c\"></form>");

FormElement outer = (FormElement) doc.getElementById("outer");
outer.elements().eachAttr("id");          // [a, c] — input b is never registered
doc.getElementById("inner");              // the inner form still exists under the template
```

Fragment parsing with a template context:

```java
Element context = Jsoup.parse("<template id=\"ctx\"></template>").getElementById("ctx");
List<Node> nodes = Parser.parseFragment("<tr><td>cell</td></tr>", context, "");
// nodes contains a single tr; no table is synthesised, and context is not modified
```

A runnable version of these calls lives at
`src/main/java/org/jsoup/examples/TemplateParsingExample.java`.

## Tests

The contract tests are in `contract/TemplateParsingTest.java` (default package), split into three groups
driven by `harness/TemplateHarness.java`:

- `modes` — template content modes, lexical elements, nested/sibling mode restoration
- `boundaries` — table/formatting/form boundaries, foster parenting, attribute merging
- `fragments` — context and context-less fragments, EOF finalisation, parser reuse, parse errors

Run everything through the fixed entry point:

```bash
bash harness/target.sh
```

It compiles the production and test sources, executes every registered contract case with real assertions,
and then runs the 13 retained original parser test methods.
