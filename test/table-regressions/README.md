# Table regression fixtures

Synthetic, fully offline pages for the table-handling regressions in
`../test-table-regressions.cjs`. Every page is a self-contained research
article with long ordinary prose, an identifiable data/control block, and no
external resources. Each parse builds a fresh document with either jsdom or
the bundled `JSDOMParser`; JSON-LD is disabled.

## DOM adaptation

jsdom inserts an implicit `<tbody>` around table rows while `JSDOMParser`
keeps the authored structure. Assertions therefore count rows through `<tr>`
and only compare the `<tbody>` count against the DOM-specific expectation
(implicit under jsdom, absent under `JSDOMParser`, preserved when authored
explicitly). Everything else — cells, links, attributes, hierarchy,
`content`, `textContent` and `length` — is asserted independently for each
DOM instead of comparing the two DOMs against each other.

## Fixtures and the behavior each assertion protects

### 01-data-table-caption — caption table in a negative-weight short div

A short 2x2 table whose first cell is a link, marked as a data table only by
its non-empty `<caption>`, wrapped in a `class="promo widget"` div (both
names hit the negative class-weight regex).

- **caption table is retained as a table** — the caption grants data-table
  identity so conditional table cleaning cannot drop it.
- **caption and all four cells survive conditional cleaning** — actual cell
  contents are kept even though the table is link-heavy and short; row count
  and the implicit-tbody adaptation are pinned.
- **link cell keeps its anchor and href** — a data table that is mostly
  links still keeps its real anchor and href.
- **negative-weight short wrapper div survives with the table** — the
  descendant-data-table rule keeps the short, negatively-scored container
  instead of deleting it together with the table; hierarchy is
  `table > div > article`.

### 02-data-table-summary — `summary` table in a negative-weight short div

Same container scenario, with the data-table identity coming from the
`summary` attribute instead of a caption (2x3 table, one link cell).

- **summary table is retained as a table** — `summary` grants data-table
  identity.
- **summary attribute and all six cells survive** — the attribute itself and
  every cell, including the absence of caption/th markup, are asserted.
- **link cell keeps its anchor and href** — link-heavy data cells are kept.
- **negative-weight short wrapper div survives with the table** — the
  container is protected by its descendant data table.

### 03-data-table-th — `thead`/`th` table with merged cells

Data-table identity comes from `thead`/`th`; one cell uses `rowspan="2"` and
another `colspan="2"`.

- **th-headed table is retained as a table** — `th`/`thead` descendants
  grant data-table identity; header and body row counts and the explicit
  tbody are pinned.
- **header labels and cell contents survive** — the actual `Name`/`Score`
  headers and body text remain.
- **rowspan/colspan merge attributes are preserved** — merged-cell
  attributes survive export (the regression does not require a browser-style
  table layout algorithm).
- **negative-weight short wrapper div survives with the table** — same
  container protection, and no stray anchors are introduced.

### 04-nested-presentation — data table inside `role="presentation"`

The outer single-cell table declares itself presentational and wraps a
`th`-headed inner data table plus a short link-only advertising div.

- **only the inner data table remains; outer layout table is flattened** —
  the outer table's presentation role is not overridden by the nested data
  table's markers; it is exported as a plain div while exactly one table
  (the inner one) remains.
- **inner table identity does not leak onto the flattened outer table** —
  hierarchy `table > div > article`, and every surviving `td` belongs to the
  inner table.
- **inner cells and link survive** — the inner data table's four cells and
  its anchor/href are kept.
- **short advertising content next to the data table is still cleaned** —
  sitting next to a data table does not sponsor the unrelated ad; the ad
  text and every link to the advertising host are removed.

### 05-nested-datatable0 — data table inside `datatable="0"`

The same nested scenario with the other explicit layout-table signal. The
four assertions are identical in intent: the outer table stays a layout
table and is flattened, the inner data table keeps its cells and link, and
the short advertising block is still cleaned.

### 06-single-cell-layout — single-cell layout table with real prose

A one-row/one-cell table holding substantial prose and an inline link; it
has no data-table signal.

- **single-cell layout table is exported without table markup** — no
  `table`/`tbody`/`tr`/`td` remains and the table id disappears.
- **cell becomes a paragraph directly under the article** — the cell is
  unwrapped to a `P` (phrasing content) with hierarchy `p > article`.
- **prose and inline link survive the unwrap** — flattening must not lose
  text or the inline anchor/href.

### 07-link-farm-control — link-dense unrelated block

A `related`-class list of five off-topic links.

- **link-dense unrelated block is cleaned wholesale** — proves the suite
  does not keep link-heavy junk; the block, its list and all junk links
  disappear while the surrounding prose stays.

### 08-plain-article-control — ordinary long article without tables

- **ordinary article keeps its paragraphs and inline reference** — proves
  ordinary extraction (paragraph count, one real inline reference link,
  prose) is unchanged by the table work.

### 09-junk-layout-table-control — link-only table without data signals

A 2x2 table of advertising links with no caption, summary, `th` or size
signal.

- **link-only table without data signals is not kept as a table** — proves
  the suite never preserves every `table`: this layout/advertising table and
  its links are removed, contrasting with fixtures 01–05.

## Common checks on every fixture

Every fixture, under both DOMs, additionally verifies the public
`Readability.parse()` contract: a non-null result with non-empty `content`
wrapped in the readability page element, retained article prose,
`length === textContent.length`, a result above the character threshold, and
no exported `script` (JSON-LD stays disabled).
