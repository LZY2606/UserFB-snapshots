/* eslint-env node */
/*
 * Mutation-testing runner for the table regressions.
 *
 * Phase 1 runs the full content/structure assertion suite against the
 * unmodified source in this working tree (it must stay green).
 *
 * Phase 2 builds three independent throwaway copies of the source, applies
 * exactly one localized behavior change to each copy, and re-runs the same
 * suite. A copy is only accepted if it loads, exposes a callable parse(),
 * and fails at least one assertion on the extracted article content or
 * structure. Syntax/import failures and suites that execute no assertions
 * are treated as runner failures, not as caught regressions.
 *
 * The working tree is never modified: every mutation lives in a temp dir.
 */

"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const REPO_ROOT = path.join(__dirname, "..");
const READABILITY_FILE = path.join(REPO_ROOT, "Readability.js");
const COPIED_FILES = [
  "Readability.js",
  "Readability-readerable.js",
  "JSDOMParser.js",
  "index.js",
];

const tableTests = require("./test-table-regressions.cjs");

function lineNumberOf(source, index) {
  return source.slice(0, index).split("\n").length;
}

// Each mutation pins the exact source block it disables. The block must
// match exactly once, so the runner fails loudly if the production code
// drifts away from the behavior these regressions are guarding.
const MUTATIONS = [
  {
    id: "mark",
    marker: "TABLE_MARK_CAUGHT",
    description:
      "caption/summary/th explicit signals no longer grant data-table identity",
    location: "Readability._markDataTables (Readability.js)",
    expectedFixtures: [
      "01-data-table-caption",
      "02-data-table-summary",
      "03-data-table-th",
      "04-nested-presentation",
      "05-nested-datatable0",
    ],
    anchor: `      var summary = table.getAttribute("summary");
      if (summary) {
        table._readabilityDataTable = true;
        continue;
      }

      var caption = table.getElementsByTagName("caption")[0];
      if (caption && caption.childNodes.length) {
        table._readabilityDataTable = true;
        continue;
      }

      // If the table has a descendant with any of these tags, consider a data table:
      var dataTableDescendants = ["col", "colgroup", "tfoot", "thead", "th"];
      var descendantExists = function (tag) {
        return !!table.getElementsByTagName(tag)[0];
      };
      if (dataTableDescendants.some(descendantExists)) {
        this.log("Data table because found data-y descendant");
        table._readabilityDataTable = true;
        continue;
      }
`,
    replacement: ``,
  },
  {
    id: "container",
    marker: "TABLE_CONTAINER_CAUGHT",
    description:
      "conditional div cleanup ignores descendant data tables",
    location: "Readability._cleanConditionally (Readability.js)",
    expectedFixtures: [
      "01-data-table-caption",
      "02-data-table-summary",
      "03-data-table-th",
      "04-nested-presentation",
      "05-nested-datatable0",
    ],
    anchor: `      // keep element if it has a data tables
      if (
        [...node.getElementsByTagName("table")].some(
          tbl => tbl._readabilityDataTable
        )
      ) {
        return false;
      }
`,
    replacement: ``,
  },
  {
    id: "unwrap",
    marker: "TABLE_UNWRAP_CAUGHT",
    description: "single-cell layout tables are no longer unwrapped",
    location: "Readability._prepArticle (Readability.js)",
    expectedFixtures: [
      "04-nested-presentation",
      "05-nested-datatable0",
      "06-single-cell-layout",
    ],
    anchor: `    // Remove single-cell tables
    this._forEachNode(
      this._getAllNodesWithTag(articleContent, ["table"]),
      function (table) {
        var tbody = this._hasSingleTagInsideElement(table, "TBODY")
          ? table.firstElementChild
          : table;
        if (this._hasSingleTagInsideElement(tbody, "TR")) {
          var row = tbody.firstElementChild;
          if (this._hasSingleTagInsideElement(row, "TD")) {
            var cell = row.firstElementChild;
            cell = this._setNodeTag(
              cell,
              this._everyNode(cell.childNodes, this._isPhrasingContent)
                ? "P"
                : "DIV"
            );
            table.parentNode.replaceChild(cell, table);
          }
        }
      }
    );
`,
    replacement: ``,
  },
];

function fail(message) {
  console.error(`RUNNER FAILURE: ${message}`);
  process.exit(1);
}

function baselinePhase() {
  const { ran, failures } = tableTests.runAllAssertions();
  if (failures.length) {
    for (const failure of failures) {
      console.error(
        `FAIL ${failure.fixture}/${failure.dom}/${failure.assertion}: ${failure.message}`
      );
    }
    fail("baseline assertions must pass on the unmodified source");
  }
  if (ran === 0) {
    fail("baseline ran no assertions");
  }
  console.log(
    `Baseline: ${ran} content/structure assertions passed across ${tableTests.FIXTURES.length} fixtures and 2 DOM implementations`
  );
  console.log("TABLE_BASELINE_OK");
}

function makeMutatedCopy(mutation) {
  const original = fs.readFileSync(READABILITY_FILE, "utf-8");
  const occurrences = original.split(mutation.anchor).length - 1;
  if (occurrences !== 1) {
    fail(
      `mutation "${mutation.id}" anchor must match exactly once, matched ${occurrences}`
    );
  }

  const anchorIndex = original.indexOf(mutation.anchor);
  const startLine = lineNumberOf(original, anchorIndex);
  const endLine = lineNumberOf(
    original,
    anchorIndex + mutation.anchor.length
  );

  const dir = fs.mkdtempSync(
    path.join(os.tmpdir(), "readability-table-mutation-")
  );
  for (const file of COPIED_FILES) {
    fs.copyFileSync(path.join(REPO_ROOT, file), path.join(dir, file));
  }
  fs.writeFileSync(
    path.join(dir, "Readability.js"),
    original.replace(mutation.anchor, mutation.replacement)
  );
  return { dir, startLine, endLine };
}

function mutationPhase(mutation) {
  const { dir, startLine, endLine } = makeMutatedCopy(mutation);
  let mutatedReadability;
  let mutatedJSDOMParser;
  try {
    // A throw here means the copy does not load (syntax/import error).
    mutatedReadability = require(path.join(dir, "index.js")).Readability;
    mutatedJSDOMParser = require(path.join(dir, "JSDOMParser.js"));
  } catch (err) {
    fs.rmSync(dir, { recursive: true, force: true });
    fail(`mutation "${mutation.id}" copy failed to load: ${err.message}`);
  }

  if (typeof mutatedReadability !== "function") {
    fs.rmSync(dir, { recursive: true, force: true });
    fail(`mutation "${mutation.id}" does not export a Readability constructor`);
  }

  // Prove parse() is callable before running the suite.
  let smokeParsed;
  try {
    const smokeParagraphs =
      "<p>Smoke paragraph content for parsing, with enough text to be parseable.</p>".repeat(
        10
      );
    const smokeDoc = new mutatedJSDOMParser().parse(
      `<html><head><title>smoke</title></head><body>${smokeParagraphs}</body></html>`,
      tableTests.BASE_URL
    );
    smokeParsed = new mutatedReadability(smokeDoc, {
      disableJSONLD: true,
    }).parse();
  } catch (err) {
    fs.rmSync(dir, { recursive: true, force: true });
    fail(`mutation "${mutation.id}" parse() threw: ${err.message}`);
  }
  if (!smokeParsed || typeof smokeParsed.content !== "string") {
    fs.rmSync(dir, { recursive: true, force: true });
    fail(`mutation "${mutation.id}" parse() did not return a result`);
  }

  const { ran, failures } = tableTests.runAllAssertions({
    Readability: mutatedReadability,
    JSDOM: require("jsdom").JSDOM,
    JSDOMParser: mutatedJSDOMParser,
  });

  fs.rmSync(dir, { recursive: true, force: true });

  if (ran === 0) {
    fail(`mutation "${mutation.id}" executed no assertions`);
  }
  if (!failures.length) {
    fail(
      `mutation "${mutation.id}" did not break any assertion; the regression is not caught`
    );
  }

  // Only content/structure failures count: a parse-level failure would mean
  // the mutation broke the engine rather than the table behavior.
  const parseFailures = failures.filter(
    (failure) => failure.assertion === "parse"
  );
  if (parseFailures.length) {
    fail(
      `mutation "${mutation.id}" broke parse() itself: ${parseFailures
        .map((failure) => `${failure.fixture}/${failure.dom}`)
        .join(", ")}`
    );
  }

  const hitFixtures = [...new Set(failures.map((failure) => failure.fixture))].sort();
  const unexpected = hitFixtures.filter(
    (fixture) => !mutation.expectedFixtures.includes(fixture)
  );
  if (unexpected.length) {
    fail(
      `mutation "${mutation.id}" hit unexpected fixtures: ${unexpected.join(", ")}`
    );
  }
  for (const fixture of mutation.expectedFixtures) {
    if (!hitFixtures.includes(fixture)) {
      fail(
        `mutation "${mutation.id}" failed to regress expected fixture ${fixture}`
      );
    }
  }

  const hitDoms = [...new Set(failures.map((failure) => failure.dom))].sort();
  for (const dom of tableTests.DOM_BUILDERS.map((builder) => builder.name)) {
    if (!hitDoms.includes(dom)) {
      fail(`mutation "${mutation.id}" did not fail under DOM ${dom}`);
    }
  }

  console.log("---");
  console.log(`Mutation "${mutation.id}": ${mutation.description}`);
  console.log(
    `Modified location: ${mutation.location}:${startLine}-${endLine} (${failures.length} assertion failures, block deleted)`
  );
  for (const failure of failures) {
    console.log(
      `  caught: ${failure.fixture}/${failure.dom}/${failure.assertion} -> ${failure.message}`
    );
  }
  console.log(mutation.marker);
}

function main() {
  baselinePhase();
  for (const mutation of MUTATIONS) {
    mutationPhase(mutation);
  }
  console.log("---");
  console.log(
    "All three localized table regressions were caught by content/structure assertions."
  );
}

main();
