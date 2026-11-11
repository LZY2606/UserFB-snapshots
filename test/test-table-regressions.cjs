/* eslint-env node */
/*
 * Offline regressions for the table handling in Readability.parse().
 *
 * The assertions intentionally do NOT compare the two DOM implementations
 * against each other: for every synthetic page they independently state the
 * cells, links and hierarchy the extracted article must keep, and check the
 * public `content`, `textContent` and `length` fields of the parse result.
 * jsdom synthesizes an implicit <tbody> while the bundled JSDOMParser keeps
 * the authored structure, so rows are asserted through <tr> lookups and the
 * tbody count is explicitly allowed to differ.
 *
 * The pages live under test/table-regressions/ and contain no external
 * resources; every document is created fresh for each parse. JSON-LD is
 * disabled.
 */

"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const FIXTURE_ROOT = path.join(__dirname, "table-regressions");
const BASE_URL = "https://source.invalid/research/page.html";
const PROSE_MARKER =
  "measured observation, with details about the sample, instruments and result";
const PAGE_WRAPPER_ID = "readability-page-1";

function defaultDeps() {
  return {
    Readability: require("..").Readability,
    JSDOM: require("jsdom").JSDOM,
    JSDOMParser: require("../JSDOMParser"),
  };
}

// Each entry rebuilds the document from scratch, exactly as a fresh parse
// would see it. No document is ever shared between two parse() calls.
const DOM_BUILDERS = [
  {
    name: "jsdom",
    makeDocument(html, deps) {
      return new deps.JSDOM(html, { url: BASE_URL }).window.document;
    },
  },
  {
    name: "JSDOMParser",
    makeDocument(html, deps) {
      return new deps.JSDOMParser().parse(html, BASE_URL);
    },
  },
];

const FIXTURES = [
  "01-data-table-caption",
  "02-data-table-summary",
  "03-data-table-th",
  "04-nested-presentation",
  "05-nested-datatable0",
  "06-single-cell-layout",
  "07-link-farm-control",
  "08-plain-article-control",
  "09-junk-layout-table-control",
];

function loadFixture(name) {
  return fs.readFileSync(
    path.join(FIXTURE_ROOT, name, "source.html"),
    "utf-8"
  );
}

function countByTag(doc, tag) {
  return doc.getElementsByTagName(tag).length;
}

function mustGetById(doc, id) {
  const node = doc.getElementById(id);
  assert.ok(node, `expected an element with id="${id}" in the output`);
  return node;
}

function findAnchorByText(doc, text) {
  const anchors = doc.getElementsByTagName("a");
  for (const anchor of anchors) {
    if (anchor.textContent.trim() === text) {
      return anchor;
    }
  }
  return null;
}

function assertNoAnchorToHost(doc, host) {
  const anchors = doc.getElementsByTagName("a");
  for (const anchor of anchors) {
    assert.ok(
      !anchor.getAttribute("href").includes(host),
      `link to advertising host ${host} must have been cleaned, found ${anchor.getAttribute(
        "href"
      )}`
    );
  }
}

function assertTextsAbsent(result, texts) {
  for (const text of texts) {
    assert.ok(
      !result.textContent.includes(text),
      `textContent must not contain cleaned content "${text}"`
    );
  }
}

function assertTextsPresent(result, texts) {
  for (const text of texts) {
    assert.ok(
      result.textContent.includes(text),
      `textContent must retain "${text}"`
    );
  }
}

// Tag chain from the element up to (and including) the first ancestor with
// stopTag. Used to pin down the hierarchy independently of class names.
function tagChain(element, stopTag) {
  const chain = [];
  let node = element;
  while (node && node.tagName) {
    chain.push(node.tagName);
    if (node.tagName === stopTag) {
      break;
    }
    node = node.parentNode;
  }
  return chain;
}

// jsdom inserts an implicit tbody; the custom JSDOMParser does not. A page
// authored without tbody therefore has one tbody under jsdom and none under
// the custom parser, which is the documented, tolerated difference.
function assertTbodyAdaptation(table, domName, explicitTbody) {
  const tbodies = table.getElementsByTagName("tbody").length;
  if (explicitTbody) {
    assert.equal(tbodies, 1, "explicit tbody must survive in both DOMs");
  } else if (domName === "jsdom") {
    assert.equal(tbodies, 1, "jsdom must synthesize an implicit tbody");
  } else {
    assert.equal(tbodies, 0, "JSDOMParser must keep the authoring (no tbody)");
  }
}

// Checks shared by every fixture: the public result fields stay consistent
// and the ordinary article prose is never lost.
function commonChecks(ctx) {
  const { result, out } = ctx;
  assert.ok(result, "Readability.parse() must return a result object");
  assert.equal(typeof result.content, "string");
  assert.ok(result.content.length > 0, "content must be a non-empty string");
  assert.ok(
    result.content.includes(`id="${PAGE_WRAPPER_ID}"`),
    "content must be the serialized readability page wrapper"
  );
  assert.ok(
    result.textContent.includes(PROSE_MARKER),
    "textContent must retain the ordinary article prose"
  );
  assert.equal(
    result.length,
    result.textContent.length,
    "length must equal the textContent length"
  );
  assert.ok(result.length > 500, "article must clear the character threshold");
  assert.equal(
    countByTag(out, "script"),
    0,
    "no script (including JSON-LD) may be exported"
  );
}

// Each assertion is a named, independent statement of what the extracted
// article must look like. See test/table-regressions/README.md for the
// behavior each one protects.
function buildSpecs() {
  return {
    "01-data-table-caption": [
      {
        name: "caption table is retained as a table",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 1);
          const table = mustGetById(ctx.out, "data-caption");
          assert.equal(table.tagName, "TABLE");
        },
      },
      {
        name: "caption and all four cells survive conditional cleaning",
        fn(ctx) {
          const table = mustGetById(ctx.out, "data-caption");
          const captions = table.getElementsByTagName("caption");
          assert.equal(captions.length, 1);
          assert.equal(
            captions[0].textContent.trim(),
            "Observed values from the field survey"
          );
          assert.equal(countByTag(table, "td"), 4);
          assert.equal(countByTag(table, "tr"), 2);
          assert.equal(countByTag(table, "th"), 0);
          assertTbodyAdaptation(table, ctx.domName, false);
          assertTextsPresent(ctx.result, [
            "CELL_A",
            "CELL_B",
            "CELL_C",
            "CELL_D",
          ]);
        },
      },
      {
        name: "link cell keeps its anchor and href",
        fn(ctx) {
          const anchor = findAnchorByText(ctx.out, "CELL_A");
          assert.ok(anchor, "CELL_A anchor must be retained");
          assert.equal(
            anchor.getAttribute("href"),
            "https://source.invalid/sample/a"
          );
          assert.equal(anchor.parentNode.tagName, "TD");
          assert.equal(countByTag(ctx.out, "a"), 1);
        },
      },
      {
        name: "negative-weight short wrapper div survives with the table",
        fn(ctx) {
          const table = mustGetById(ctx.out, "data-caption");
          assert.deepEqual(tagChain(table, "ARTICLE"), [
            "TABLE",
            "DIV",
            "ARTICLE",
          ]);
          assert.equal(table.parentNode.children.length, 1);
        },
      },
    ],

    "02-data-table-summary": [
      {
        name: "summary table is retained as a table",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 1);
          const table = mustGetById(ctx.out, "data-summary");
          assert.equal(table.tagName, "TABLE");
        },
      },
      {
        name: "summary attribute and all six cells survive",
        fn(ctx) {
          const table = mustGetById(ctx.out, "data-summary");
          assert.equal(
            table.getAttribute("summary"),
            "Quarterly readings taken at two field sites"
          );
          assert.equal(countByTag(table, "caption"), 0);
          assert.equal(countByTag(table, "th"), 0);
          assert.equal(countByTag(table, "td"), 6);
          assert.equal(countByTag(table, "tr"), 2);
          assertTbodyAdaptation(table, ctx.domName, false);
          assertTextsPresent(ctx.result, [
            "SUM_A",
            "SUM_B",
            "SUM_C",
            "SUM_D",
            "SUM_E",
            "SUM_F",
          ]);
        },
      },
      {
        name: "link cell keeps its anchor and href",
        fn(ctx) {
          const anchor = findAnchorByText(ctx.out, "SUM_A");
          assert.ok(anchor, "SUM_A anchor must be retained");
          assert.equal(
            anchor.getAttribute("href"),
            "https://source.invalid/sample/m1"
          );
          assert.equal(anchor.parentNode.tagName, "TD");
          assert.equal(countByTag(ctx.out, "a"), 1);
        },
      },
      {
        name: "negative-weight short wrapper div survives with the table",
        fn(ctx) {
          const table = mustGetById(ctx.out, "data-summary");
          assert.deepEqual(tagChain(table, "ARTICLE"), [
            "TABLE",
            "DIV",
            "ARTICLE",
          ]);
        },
      },
    ],

    "03-data-table-th": [
      {
        name: "th-headed table is retained as a table",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 1);
          const table = mustGetById(ctx.out, "data-th");
          assert.equal(table.tagName, "TABLE");
          assert.equal(countByTag(table, "thead"), 1);
          assert.equal(countByTag(table, "th"), 2);
          assert.equal(countByTag(table, "td"), 3);
          assert.equal(countByTag(table, "tr"), 3);
          assertTbodyAdaptation(table, ctx.domName, true);
        },
      },
      {
        name: "header labels and cell contents survive",
        fn(ctx) {
          const headers = ctx.out
            .getElementById("data-th")
            .getElementsByTagName("th");
          assert.deepEqual(
            [headers[0].textContent, headers[1].textContent],
            ["Name", "Score"]
          );
          assertTextsPresent(ctx.result, ["TH_A", "TH_B", "TH_C"]);
        },
      },
      {
        name: "rowspan/colspan merge attributes are preserved",
        fn(ctx) {
          const cells = ctx.out
            .getElementById("data-th")
            .getElementsByTagName("td");
          const byText = (text) =>
            [...cells].find((cell) => cell.textContent.trim() === text);
          assert.equal(byText("TH_A").getAttribute("rowspan"), "2");
          assert.equal(byText("TH_B").getAttribute("colspan"), "2");
        },
      },
      {
        name: "negative-weight short wrapper div survives with the table",
        fn(ctx) {
          const table = mustGetById(ctx.out, "data-th");
          assert.deepEqual(tagChain(table, "ARTICLE"), [
            "TABLE",
            "DIV",
            "ARTICLE",
          ]);
          assert.equal(countByTag(ctx.out, "a"), 0);
        },
      },
    ],

    "04-nested-presentation": nestedSpecs({
      fixture: "04-nested-presentation",
      outerId: "layout-presentation",
      innerId: "inner-presentation",
      cells: ["INNER_A", "INNER_B", "INNER_C", "INNER_D"],
      anchorText: "INNER_A",
      anchorHref: "https://source.invalid/series/x",
      adText: "BUY_NOW_PRESENTATION",
    }),

    "05-nested-datatable0": nestedSpecs({
      fixture: "05-nested-datatable0",
      outerId: "layout-datatable0",
      innerId: "inner-datatable0",
      cells: ["DT0_A", "DT0_B", "DT0_C", "DT0_D"],
      anchorText: "DT0_A",
      anchorHref: "https://source.invalid/series/y",
      adText: "BUY_NOW_DATATABLE0",
    }),

    "06-single-cell-layout": [
      {
        name: "single-cell layout table is exported without table markup",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 0);
          for (const tag of ["tbody", "tr", "td"]) {
            assert.equal(countByTag(ctx.out, tag), 0);
          }
          assert.equal(ctx.out.getElementById("single-cell-table"), null);
        },
      },
      {
        name: "cell becomes a paragraph directly under the article",
        fn(ctx) {
          const cell = mustGetById(ctx.out, "single-cell");
          assert.equal(cell.tagName, "P");
          assert.deepEqual(tagChain(cell, "ARTICLE"), ["P", "ARTICLE"]);
        },
      },
      {
        name: "prose and inline link survive the unwrap",
        fn(ctx) {
          const cell = mustGetById(ctx.out, "single-cell");
          assert.ok(cell.textContent.includes(PROSE_MARKER));
          assert.ok(cell.textContent.includes("SINGLE_CELL_TEXT"));
          const anchor = findAnchorByText(ctx.out, "SINGLE_LINK");
          assert.ok(anchor, "inline anchor must survive the unwrap");
          assert.equal(
            anchor.getAttribute("href"),
            "https://source.invalid/note/s"
          );
          assert.equal(anchor.parentNode.tagName, "P");
          assertTextsPresent(ctx.result, ["SINGLE_CELL_TEXT", "SINGLE_LINK"]);
        },
      },
    ],

    "07-link-farm-control": [
      {
        name: "link-dense unrelated block is cleaned wholesale",
        fn(ctx) {
          assert.equal(ctx.out.getElementById("link-farm"), null);
          assert.equal(countByTag(ctx.out, "ul"), 0);
          assert.equal(countByTag(ctx.out, "a"), 0);
          assert.equal(countByTag(ctx.out, "table"), 0);
          assertTextsAbsent(ctx.result, [
            "JUNK_LINK_1",
            "JUNK_LINK_2",
            "JUNK_LINK_3",
            "JUNK_LINK_4",
            "JUNK_LINK_5",
          ]);
        },
      },
    ],

    "08-plain-article-control": [
      {
        name: "ordinary article keeps its paragraphs and inline reference",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 0);
          assert.equal(countByTag(ctx.out, "p"), 5);
          const anchor = findAnchorByText(ctx.out, "REFERENCE_LINK");
          assert.ok(anchor, "inline reference link must be retained");
          assert.equal(
            anchor.getAttribute("href"),
            "https://source.invalid/ref"
          );
          assert.equal(countByTag(ctx.out, "a"), 1);
          assertTextsPresent(ctx.result, ["REFERENCE_LINK", PROSE_MARKER]);
        },
      },
    ],

    "09-junk-layout-table-control": [
      {
        name: "link-only table without data signals is not kept as a table",
        fn(ctx) {
          assert.equal(countByTag(ctx.out, "table"), 0);
          assert.equal(ctx.out.getElementById("junk-layout-table"), null);
          assert.equal(countByTag(ctx.out, "a"), 0);
          assertTextsAbsent(ctx.result, [
            "JUNK_A",
            "JUNK_B",
            "JUNK_C",
            "JUNK_D",
          ]);
        },
      },
    ],
  };
}

function nestedSpecs(config) {
  return [
    {
      name: "only the inner data table remains; outer layout table is flattened",
      fn(ctx) {
        assert.equal(countByTag(ctx.out, "table"), 1);
        assert.equal(ctx.out.getElementById(config.outerId), null);
        const inner = mustGetById(ctx.out, config.innerId);
        assert.equal(inner.tagName, "TABLE");
        assert.equal(countByTag(inner, "thead"), 1);
        assert.equal(countByTag(inner, "th"), 2);
        assert.equal(countByTag(inner, "td"), 4);
        assert.equal(countByTag(inner, "tr"), 3);
      },
    },
    {
      name: "inner table identity does not leak onto the flattened outer table",
      fn(ctx) {
        const inner = mustGetById(ctx.out, config.innerId);
        // The presentation/datatable=0 outer table becomes a plain div, so
        // the data table's only ancestor chain is table > div > article.
        assert.deepEqual(tagChain(inner, "ARTICLE"), [
          "TABLE",
          "DIV",
          "ARTICLE",
        ]);
        const allCells = ctx.out.getElementsByTagName("td");
        assert.equal(allCells.length, 4);
        for (const cell of allCells) {
          assert.equal(
            cell.parentNode.parentNode.parentNode.id,
            config.innerId,
            "every surviving cell must belong to the inner data table"
          );
        }
      },
    },
    {
      name: "inner cells and link survive",
      fn(ctx) {
        assertTextsPresent(ctx.result, config.cells);
        const anchor = findAnchorByText(ctx.out, config.anchorText);
        assert.ok(anchor, "data table anchor must be retained");
        assert.equal(anchor.getAttribute("href"), config.anchorHref);
        assert.equal(anchor.parentNode.tagName, "TD");
        assert.equal(countByTag(ctx.out, "a"), 1);
      },
    },
    {
      name: "short advertising content next to the data table is still cleaned",
      fn(ctx) {
        assertTextsAbsent(ctx.result, [config.adText]);
        assertNoAnchorToHost(ctx.out, "ads.invalid");
      },
    },
  ];
}

function parseOnce(ReadabilityCtor, builder, html, deps) {
  // A brand-new document for every parse, under both implementations.
  const sourceDoc = builder.makeDocument(html, deps);
  const result = new ReadabilityCtor(sourceDoc, {
    disableJSONLD: true,
  }).parse();
  assert.ok(result, "parse() must not return null");
  const out = builder.makeDocument(result.content, deps);
  return { result, out };
}

/**
 * Run every fixture under every DOM builder and collect assertion failures
 * instead of throwing on the first one, so mutation runs can report every
 * regression they trigger.
 *
 * @returns {{ran: number, failures: Array<{fixture:string, dom:string,
 *   assertion:string, message:string}>}}
 */
function runAllAssertions(deps = defaultDeps()) {
  const specs = buildSpecs();
  const failures = [];
  let ran = 0;

  for (const fixture of FIXTURES) {
    const html = loadFixture(fixture);
    for (const builder of DOM_BUILDERS) {
      let ctx;
      try {
        ctx = parseOnce(deps.Readability, builder, html, deps);
        ctx.domName = builder.name;
      } catch (err) {
        failures.push({
          fixture,
          dom: builder.name,
          assertion: "parse",
          message: err && err.message ? err.message : String(err),
        });
        continue;
      }

      for (const spec of [{ name: "common result fields", fn: commonChecks }, ...specs[fixture]]) {
        ran += 1;
        try {
          spec.fn(ctx);
        } catch (err) {
          failures.push({
            fixture,
            dom: builder.name,
            assertion: spec.name,
            message: err && err.message ? err.message : String(err),
          });
        }
      }
    }
  }

  return { ran, failures };
}

module.exports = {
  BASE_URL,
  DOM_BUILDERS,
  FIXTURES,
  FIXTURE_ROOT,
  loadFixture,
  parseOnce,
  runAllAssertions,
};

if (require.main === module) {
  const { ran, failures } = runAllAssertions();
  if (failures.length) {
    for (const failure of failures) {
      console.error(
        `FAIL ${failure.fixture}/${failure.dom}/${failure.assertion}: ${failure.message}`
      );
    }
    process.exit(1);
  }
  console.log(`TABLE_BASELINE_OK (${ran} assertions across ${FIXTURES.length} fixtures and 2 DOMs)`);
}
