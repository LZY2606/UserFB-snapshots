# CSV parser for Node.js and the web

[![Build Status](https://img.shields.io/github/actions/workflow/status/adaltas/node-csv/nodejs.yml?branch=master)](https://github.com/adaltas/node-csv/actions)
[![NPM](https://img.shields.io/npm/dm/csv-parse)](https://www.npmjs.com/package/csv-parse)
[![NPM](https://img.shields.io/npm/v/csv-parse)](https://www.npmjs.com/package/csv-parse)

The [`csv-parse` package](https://csv.js.org/parse/) is a parser converting CSV text input into arrays or objects. It is part of the [CSV project](https://csv.js.org/).

It implements the Node.js [`stream.Transform` API](http://nodejs.org/api/stream.html#stream_class_stream_transform). It also provides a simple callback-based API for convenience. It is both extremely easy to use and powerful. It was first released in 2010 and is used against big data sets by a large community.

## Documentation

- [Project homepage](https://csv.js.org/parse/)
- [API](https://csv.js.org/parse/api/)
- [Options](https://csv.js.org/parse/options/)
- [Info properties](https://csv.js.org/parse/info/)
- [Common errors](https://csv.js.org/parse/errors/)
- [Examples](https://csv.js.org/project/examples/)

## Main features

- Flexible with lot of [options](https://csv.js.org/parse/options/)
- Multiple [distributions](https://csv.js.org/parse/distributions/): Node.js, Web, ECMAScript modules and CommonJS
- Follow the Node.js streaming API
- Simplicity with the optional callback API
- Support delimiters, quotes, escape characters and comments
- Line breaks discovery
- Support big datasets
- Complete test coverage and lot of samples for inspiration
- No external dependencies
- Work nicely with the [csv-generate](https://csv.js.org/generate/), [stream-transform](https://csv.js.org/transform/) and [csv-stringify](https://csv.js.org/stringify/) packages
- MIT License

## Usage

Run `npm install csv` to install the full CSV module or run `npm install csv-parse` if you are only interested by the CSV parser.

Use the callback and sync APIs for simplicity or the stream based API for scalability.

## Example

The [API](https://csv.js.org/parse/api/) is available in multiple flavors. This example illustrates the stream API.

```js
import assert from "assert";
import { parse } from "csv-parse";

const records = [];
// Initialize the parser
const parser = parse({
  delimiter: ":",
});
// Use the readable stream api to consume records
parser.on("readable", function () {
  let record;
  while ((record = parser.read()) !== null) {
    records.push(record);
  }
});
// Catch any error
parser.on("error", function (err) {
  console.error(err.message);
});
// Test that the parsed records matched the expected records
parser.on("end", function () {
  assert.deepStrictEqual(records, [
    ["root", "x", "0", "0", "root", "/root", "/bin/bash"],
    ["someone", "x", "1022", "1022", "", "/home/someone", "/bin/bash"],
  ]);
});
// Write data to the stream
parser.write("root:x:0:0:root:/root:/bin/bash\n");
parser.write("someone:x:1022:1022::/home/someone:/bin/bash\n");
// Close the readable stream
parser.end();
```

## Recovering damaged records

When an offline batch contains a corrupted record, the opt-in
`recover_records` option quarantines the damaged bytes and resumes parsing at
the next trustworthy record boundary instead of aborting the whole dataset.
It is available on the Node.js stream API, the callback API and the
synchronous API, and the legacy strict behavior is unchanged when the option
is absent.

```js
import { parse } from "csv-parse";

const parser = parse({
  columns: ["a", "b", "c"],
  record_delimiter: "\n",
  recover_records: {
    // Every confirmed record must contain exactly this number of fields.
    field_count: 3,
    // A candidate boundary is accepted only after this many consecutive,
    // strictly valid logical records (minimum 2).
    confirm: 2,
    // Called once per quarantine with the original error and the damaged
    // byte range.
    on_quarantine(report) {
      // report.error: the original CsvError
      // report.start / report.end: absolute, half open byte offsets
      // report.buffer: independent Buffer copy of report[start, end)
    },
  },
});
```

After a syntax error, a field count mismatch or a `max_record_size` error, the
parser keeps every byte from the start of the damaged record. Each physical
record delimiter ending after the error position is then tried, in byte
order, as a candidate start. A candidate is accepted only when `confirm`
consecutive complete logical records parse with a strict syntax, the expected
`field_count` and a compliant record size; quoted newlines are supported, so
the confirmation count counts logical records, not physical lines. The
earliest valid candidate wins, field contents are never used to guess.

Candidate probing never calls `cast`, `on_record` or any other user callback
and tentative records do not increment `info.records`. Once a boundary is
selected, `on_quarantine` receives one report and the confirmed records go
through the normal path exactly once. If EOF is reached without enough
confirmation records, the bytes from the damaged record start to EOF are
delivered as a final quarantine and parsing ends successfully, preserving the
records already emitted. Offsets are byte offsets over the input stream
(including the BOM), never decoded string lengths.

The mode currently requires UTF-8, a single field delimiter, a single record
delimiter and standard quote/escape handling, with `columns` disabled or set
to a fixed array whose length equals `field_count`. It cannot be combined
with delimiter auto-discovery (`delimiter_auto`), dynamic headers
(`columns: true`), `relax_quotes`, `relax_column_count` or
`skip_records_with_error`; incompatible configurations are rejected before
any input is read. Errors thrown by `cast`, `on_record` or `on_quarantine`
still terminate the parsing as usual.

## Contributors

The project is sponsored by [Adaltas](https://www.adaltas.com), an Big Data consulting firm based in Paris, France.

- David Worms: <https://github.com/wdavidw>
