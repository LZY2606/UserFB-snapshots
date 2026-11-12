import { test } from "node:test";
import assert from "node:assert/strict";
import { parse } from "../packages/csv-parse/lib/index.js";
import { parse as parseSync } from "../packages/csv-parse/lib/sync.js";
import { parse as parseWeb } from "../packages/csv-parse/lib/stream.js";

// Shared configuration. `recover_records` is opt-in and only present when
// explicitly requested.
const recoverConfig = (quarantines, extra = {}) => ({
  columns: ["a", "b", "c"],
  record_delimiter: "\n",
  raw: true,
  info: true,
  recover_records: {
    field_count: 3,
    confirm: 2,
    on_quarantine: (report) =>
      quarantines.push({
        error: report.error,
        start: report.start,
        end: report.end,
        buffer: report.buffer,
      }),
  },
  ...extra,
});

const runSync = (data, options) => parseSync(data, options);

const runStream = async (data, options, chunkSize = null) => {
  const parser = parse(options);
  const records = [];
  const done = (async () => {
    for await (const record of parser) records.push(record);
  })();
  if (chunkSize === null) {
    parser.write(data);
  } else {
    for (let i = 0; i < data.length; i += chunkSize) {
      parser.write(data.subarray(i, i + chunkSize));
    }
  }
  parser.end();
  await done;
  return records;
};

const runWebStream = async (data, options, chunkSize = null) => {
  const stream = parseWeb(options);
  const writer = stream.writable.getWriter();
  if (chunkSize === null) {
    await writer.write(data);
  } else {
    for (let i = 0; i < data.length; i += chunkSize) {
      await writer.write(data.subarray(i, i + chunkSize));
    }
  }
  await writer.close();
  const records = [];
  for await (const record of stream.readable) records.push(record);
  return records;
};

// Concrete byte fixtures.
const fixtures = {
  // A normal record spanning several physical lines, no damage.
  multiline: Buffer.from('1,"a\nb",x\n2,cc,dd\n3,ee,ff\n'),
  // A damaged closing quote followed by two trusted records.
  badClosingQuote: Buffer.from("1,ok,x\n2,\"bad\"o,z\n3,aa,bb\n4,cc,dd\n"),
  // A pseudo three-column single line trapped inside a damaged quoted
  // region; it must not be mistaken for a recovery boundary.
  pseudoThenBad: Buffer.from(
    "1,ok,x\n2,\"start\nfake,three,cols\n3,end\"x,z\n4,aa,bb\n5,cc,dd\n",
  ),
  // EOF reached before `confirm` complete records can confirm a boundary.
  eofInsufficient: Buffer.from('1,ok,x\n2,"unclosed\nonly,one\n'),
  // EOF recovery where the last confirmed record has no trailing newline.
  eofNoTrailingNewline: Buffer.from("1,ok,x\n2,\"bad\"o,z\n3,aa,bb\n4,cc,dd"),
  // A record exceeding max_record_size, followed by two trusted records.
  maxRecordSize: Buffer.from("g1,a,b\nxxxxxxxxxxxx,yy,zz\ng2,c,d\ng3,e,f\n"),
  // columns:false, a two-field pseudo record then a damaged quoted line.
  columnsFalse: Buffer.from("1,2,3\nfake,row\nbad,\"q\nr1,r2,r3\nr4,r5,r6\n"),
  // Trusted records using escaped quotes and quoted newlines.
  escapedQuotes: Buffer.from('1,a,b\n2,"bad"o,z\n3,"a""b",c\n4,"l1\nl2",e\n'),
  // A damaged closing quote located in the first field, so no field of the
  // damaged record reaches the user `cast` callback.
  badQuoteFirstField: Buffer.from('1,ok,x\n"bad"o,z,9\n3,aa,bb\n4,cc,dd\n'),
  // Windows line endings.
  crlf: Buffer.from("1,ok,x\r\n2,\"bad\"o,z\r\n3,aa,bb\r\n4,cc,dd\r\n"),
  // Multibyte UTF-8 content.
  utf8: Buffer.from("1,好,x\n2,\"b\"ad,z\n3,é,bb\n4,終,dd\n"),
};

const expectedQuarantines = {
  badClosingQuote: [
    { start: 7, end: 18, code: "CSV_INVALID_CLOSING_QUOTE" },
  ],
  pseudoThenBad: [
    { start: 7, end: 42, code: "CSV_INVALID_CLOSING_QUOTE" },
  ],
  eofInsufficient: [{ start: 7, end: 28, code: "CSV_QUOTE_NOT_CLOSED" }],
  eofNoTrailingNewline: [
    { start: 7, end: 18, code: "CSV_INVALID_CLOSING_QUOTE" },
  ],
  maxRecordSize: [{ start: 7, end: 26, code: "CSV_MAX_RECORD_SIZE" }],
  columnsFalse: [
    { start: 6, end: 22, code: "CSV_RECORD_INCONSISTENT_FIELDS_LENGTH" },
  ],
  escapedQuotes: [
    { start: 6, end: 17, code: "CSV_INVALID_CLOSING_QUOTE" },
  ],
  badQuoteFirstField: [
    { start: 7, end: 18, code: "CSV_INVALID_CLOSING_QUOTE" },
  ],
  crlf: [{ start: 8, end: 20, code: "CSV_INVALID_CLOSING_QUOTE" }],
  utf8: [{ start: 8, end: 18, code: "CSV_INVALID_CLOSING_QUOTE" }],
};

test("QuarantineBoundary", async (t) => {
  await t.test("delivers precise half open byte ranges on sync", () => {
    for (const [name, data] of Object.entries(fixtures)) {
      const quarantines = [];
      const options =
        name === "columnsFalse"
          ? recoverConfig(quarantines, { columns: false })
          : name === "maxRecordSize"
            ? recoverConfig(quarantines, { max_record_size: 10 })
            : recoverConfig(quarantines);
      runSync(data, options);
      const expected = expectedQuarantines[name];
      if (expected === undefined) {
        assert.equal(quarantines.length, 0, `${name}: no quarantine expected`);
        continue;
      }
      assert.equal(quarantines.length, expected.length, `${name}: count`);
      quarantines.forEach((report, i) => {
        assert.equal(report.start, expected[i].start, `${name}: start`);
        assert.equal(report.end, expected[i].end, `${name}: end`);
        assert.equal(report.error.code, expected[i].code, `${name}: code`);
        assert.ok(report.start < report.end, `${name}: non empty range`);
        assert.deepEqual(
          report.buffer,
          data.subarray(report.start, report.end),
          `${name}: buffer matches the range`,
        );
      });
    }
  });

  await t.test("keeps an independent buffer copy", () => {
    const data = fixtures.badClosingQuote;
    const quarantines = [];
    runSync(data, recoverConfig(quarantines));
    const report = quarantines[0];
    assert.ok(Buffer.isBuffer(report.buffer));
    const originalByte = data[report.start];
    report.buffer[0] = report.buffer[0] ^ 0xff;
    assert.equal(
      data[report.start],
      originalByte,
      "mutating the report buffer must not touch the input",
    );
  });

  await t.test("selects the earliest confirmable boundary, never a pseudo line", () => {
    const data = fixtures.pseudoThenBad;
    const quarantines = [];
    const records = runSync(data, recoverConfig(quarantines));
    const report = quarantines[0];
    // The pseudo line `fake,three,cols` sits at bytes 15..30 and is inside
    // the quarantined range; the boundary is only accepted at byte 42.
    assert.equal(report.start, 7);
    assert.equal(report.end, 42);
    assert.ok(report.buffer.toString().includes("fake,three,cols"));
    assert.deepEqual(records.map((r) => r.record), [
      { a: "1", b: "ok", c: "x" },
      { a: "4", b: "aa", c: "bb" },
      { a: "5", b: "cc", c: "dd" },
    ]);
  });

  await t.test("quarantines until EOF when confirmation is impossible", () => {
    const data = fixtures.eofInsufficient;
    const quarantines = [];
    const records = runSync(data, recoverConfig(quarantines));
    const report = quarantines[0];
    assert.equal(report.start, 7);
    assert.equal(report.end, data.length, "range reaches EOF");
    assert.equal(report.error.code, "CSV_QUOTE_NOT_CLOSED");
    assert.deepEqual(records.map((r) => r.record), [
      { a: "1", b: "ok", c: "x" },
    ]);
  });

  await t.test("accepts a newline-less final record at EOF", () => {
    const data = fixtures.eofNoTrailingNewline;
    const quarantines = [];
    const records = runSync(data, recoverConfig(quarantines));
    assert.deepEqual(records.map((r) => r.record), [
      { a: "1", b: "ok", c: "x" },
      { a: "3", b: "aa", c: "bb" },
      { a: "4", b: "cc", c: "dd" },
    ]);
    assert.deepEqual(
      quarantines.map((q) => [q.start, q.end]),
      [[7, 18]],
    );
  });

  await t.test("produces non overlapping ranges on repeated damage", () => {
    const data = Buffer.from(
      'g1,a,b\nbad,"x"o\ng2,c,d\ng3,e,f\nbad2,"y"o\ng4,g,h\ng5,i,j\n',
    );
    const quarantines = [];
    const records = runSync(data, recoverConfig(quarantines));
    assert.deepEqual(
      quarantines.map((q) => [q.start, q.end]),
      [
        [7, 16],
        [30, 40],
      ],
    );
    assert.ok(quarantines[0].end <= quarantines[1].start);
    assert.deepEqual(records.map((r) => r.record), [
      { a: "g1", b: "a", c: "b" },
      { a: "g2", b: "c", c: "d" },
      { a: "g3", b: "e", c: "f" },
      { a: "g4", b: "g", c: "h" },
      { a: "g5", b: "i", c: "j" },
    ]);
  });

  await t.test("is item for item identical when no record is damaged", () => {
    for (const data of [fixtures.multiline, fixtures.escapedQuotes.subarray(0, 6)]) {
      const enabled = runSync(data, recoverConfig([]));
      const disabled = runSync(data, {
        columns: ["a", "b", "c"],
        record_delimiter: "\n",
        raw: true,
        info: true,
      });
      assert.deepEqual(enabled, disabled);
    }
  });

  await t.test("rejects incompatible configuration before reading input", () => {
    const valid = {
      field_count: 3,
      confirm: 2,
      on_quarantine: () => {},
    };
    const build = (recover, extra = {}) => () =>
      parse({
        columns: ["a", "b", "c"],
        record_delimiter: "\n",
        recover_records: recover,
        ...extra,
      });
    const code = "CSV_INVALID_OPTION_RECOVER_RECORDS";
    assert.throws(build("nope"), (err) => err.code === code);
    for (const field_count of [0, -1, 1.5, "3", Infinity, Number.MAX_SAFE_INTEGER + 1]) {
      assert.throws(
        build({ ...valid, field_count }),
        (err) => err.code === code,
        `field_count=${String(field_count)}`,
      );
    }
    for (const confirm of [0, 1, 2.5, "2"]) {
      assert.throws(
        build({ ...valid, confirm }),
        (err) => err.code === code,
        `confirm=${String(confirm)}`,
      );
    }
    assert.throws(build({ field_count: 3, confirm: 2 }), /on_quarantine/);
    assert.throws(
      build({ ...valid, on_quarantine: {} }),
      /on_quarantine/,
    );
    assert.throws(build(valid, { delimiter_auto: true }), /delimiter_auto/);
    assert.throws(build(valid, { columns: true }), /dynamic columns/);
    assert.throws(build(valid, { relax_quotes: true }), /relax_quotes/);
    assert.throws(
      build(valid, { relax_column_count: true }),
      /relax_column_count/,
    );
    assert.throws(
      build(valid, { relax_column_count_less: true }),
      /relax_column_count/,
    );
    assert.throws(
      build(valid, { skip_records_with_error: true }),
      /skip_records_with_error/,
    );
    assert.throws(
      build(valid, { columns: ["a", "b"] }),
      /field_count/,
    );
    assert.throws(build(valid, { delimiter: [",", ";"] }), /field delimiter/);
    assert.throws(
      build(valid, { record_delimiter: ["\n", "\r\n"] }),
      /record delimiter/,
    );
    assert.throws(build(valid, { encoding: "latin1" }), /UTF-8/);
  });
});

test("ProbePurity", async (t) => {
  await t.test("never invokes user callbacks while probing candidates", () => {
    // The damaged record fails inside its first field, so the main parser
    // itself casts no field for it. Every cast therefore belongs to a
    // published record: 3 records x 3 fields.
    const data = fixtures.badQuoteFirstField;
    let casts = 0;
    let onRecords = 0;
    let onSkips = 0;
    const quarantines = [];
    const records = runSync(
      data,
      recoverConfig(quarantines, {
        cast: (value) => {
          casts += 1;
          return value;
        },
        on_record: (record) => {
          onRecords += 1;
          return record;
        },
        on_skip: () => {
          onSkips += 1;
        },
      }),
    );
    assert.equal(records.length, 3);
    assert.equal(casts, 9, "only the 9 published fields are cast");
    assert.equal(onRecords, 3, "on_record runs once per published record");
    assert.equal(onSkips, 0, "on_skip is not used by the recovery mode");
    assert.equal(quarantines.length, 1);
  });

  await t.test("probed records do not increment info.records", () => {
    const data = fixtures.pseudoThenBad;
    const quarantines = [];
    const records = runSync(data, recoverConfig(quarantines));
    assert.deepEqual(
      records.map((r) => r.info.records),
      [1, 2, 3],
      "record numbering is continuous over delivered records only",
    );
    // Line numbers are absolute physical lines of the original input; the
    // confirmed boundary is on physical line 4.
    assert.deepEqual(
      records.map((r) => r.info.lines),
      [1, 4, 5],
    );
  });

  await t.test("confirmed records go through the formal path exactly once", async () => {
    const data = fixtures.badClosingQuote;
    for (const chunkSize of [1, 3, 8, 100]) {
      const quarantines = [];
      let onRecords = 0;
      const records = await runStream(
        data,
        recoverConfig(quarantines, {
          on_record: (record) => {
            onRecords += 1;
            return record;
          },
        }),
        chunkSize,
      );
      assert.equal(records.length, 3, `chunkSize=${chunkSize}: no duplicate delivery`);
      assert.equal(onRecords, 3, `chunkSize=${chunkSize}: one formal pass only`);
      assert.equal(quarantines.length, 1);
      const seen = new Set();
      for (const record of records) {
        assert.ok(!seen.has(record.raw), "raw content delivered twice");
        seen.add(record.raw);
      }
    }
  });

  await t.test("a prefix published before damage is never re-delivered", async () => {
    const data = fixtures.pseudoThenBad;
    const quarantines = [];
    const records = await runStream(data, recoverConfig(quarantines), 1);
    const raws = records.map((r) => r.raw);
    assert.deepEqual(raws, ["1,ok,x\n", "4,aa,bb\n", "5,cc,dd\n"]);
    assert.equal(raws.filter((raw) => raw === "1,ok,x\n").length, 1);
  });
});

test("ChunkReentry", async (t) => {
  const compareAcrossCuts = async (data, options, tName) => {
    const syncQuarantines = [];
    const syncRecords = runSync(data, options(syncQuarantines));
    // Every single cut point.
    for (let cut = 1; cut <= data.length; cut++) {
      const streamQuarantines = [];
      const parser = parse(options(streamQuarantines));
      const streamRecords = [];
      const done = (async () => {
        for await (const record of parser) streamRecords.push(record);
      })();
      parser.write(data.subarray(0, cut));
      parser.write(data.subarray(cut));
      parser.end();
      await done;
      assert.deepEqual(
        streamRecords.map((r) => r.record),
        syncRecords.map((r) => r.record),
        `${tName} cut=${cut}: records`,
      );
      assert.deepEqual(
        streamQuarantines.map((q) => [q.start, q.end, q.error.code]),
        syncQuarantines.map((q) => [q.start, q.end, q.error.code]),
        `${tName} cut=${cut}: quarantine ranges`,
      );
      assert.deepEqual(
        streamRecords.map((r) => r.raw),
        syncRecords.map((r) => r.raw),
        `${tName} cut=${cut}: raw`,
      );
      assert.deepEqual(
        streamRecords.map((r) => [r.info.bytes, r.info.lines, r.info.records]),
        syncRecords.map((r) => [r.info.bytes, r.info.lines, r.info.records]),
        `${tName} cut=${cut}: info`,
      );
    }
    // Byte by byte.
    const byteQuarantines = [];
    const byteRecords = await runStream(data, options(byteQuarantines), 1);
    assert.deepEqual(
      byteRecords.map((r) => r.record),
      syncRecords.map((r) => r.record),
      `${tName} bytewise: records`,
    );
    assert.deepEqual(
      byteQuarantines.map((q) => [q.start, q.end]),
      syncQuarantines.map((q) => [q.start, q.end]),
      `${tName} bytewise: ranges`,
    );
    return { syncRecords, syncQuarantines };
  };

  await t.test("survives every cut point and bytewise input", async () => {
    const cases = [
      ["multiline", fixtures.multiline, (q) => recoverConfig(q)],
      ["badClosingQuote", fixtures.badClosingQuote, (q) => recoverConfig(q)],
      ["pseudoThenBad", fixtures.pseudoThenBad, (q) => recoverConfig(q)],
      ["eofInsufficient", fixtures.eofInsufficient, (q) => recoverConfig(q)],
      [
        "eofNoTrailingNewline",
        fixtures.eofNoTrailingNewline,
        (q) => recoverConfig(q),
      ],
      ["escapedQuotes", fixtures.escapedQuotes, (q) => recoverConfig(q)],
      ["crlf", fixtures.crlf, (q) => recoverConfig(q)],
      ["utf8", fixtures.utf8, (q) => recoverConfig(q)],
      [
        "maxRecordSize",
        fixtures.maxRecordSize,
        (q) => recoverConfig(q, { max_record_size: 10 }),
      ],
      [
        "columnsFalse",
        fixtures.columnsFalse,
        (q) => recoverConfig(q, { columns: false }),
      ],
    ];
    for (const [name, data, options] of cases) {
      const { syncRecords, syncQuarantines } = await compareAcrossCuts(
        data,
        options,
        name,
      );
      // raw and info must reference the original input positions, not a
      // position local to a tentative probe buffer.
      for (const record of syncRecords) {
        const rawLength = Buffer.byteLength(record.raw);
        assert.equal(
          data
            .subarray(record.info.bytes - rawLength, record.info.bytes)
            .toString(),
          record.raw,
          `${name}: raw located at original absolute byte offset`,
        );
      }
      for (const report of syncQuarantines) {
        assert.deepEqual(
          report.buffer,
          data.subarray(report.start, report.end),
          `${name}: quarantine bytes`,
        );
      }
    }
  });

  await t.test("keeps absolute offsets with a UTF-8 BOM", async () => {
    const data = Buffer.concat([
      Buffer.from([239, 187, 191]),
      fixtures.badClosingQuote,
    ]);
    const options = (q) => recoverConfig(q, { bom: true });
    const { syncQuarantines } = await compareAcrossCuts(data, options, "bom");
    assert.deepEqual(
      syncQuarantines.map((q) => [q.start, q.end]),
      [[10, 21]],
    );
  });

  await t.test("works through the web stream API", async () => {
    for (const [name, data] of [
      ["badClosingQuote", fixtures.badClosingQuote],
      ["pseudoThenBad", fixtures.pseudoThenBad],
      ["eofInsufficient", fixtures.eofInsufficient],
    ]) {
      const quarantines = [];
      const records = await runWebStream(
        data,
        recoverConfig(quarantines),
        1,
      );
      const syncQuarantines = [];
      const syncRecords = runSync(data, recoverConfig(syncQuarantines));
      assert.deepEqual(
        records.map((r) => r.record),
        syncRecords.map((r) => r.record),
        `${name}: web stream records`,
      );
      assert.deepEqual(
        quarantines.map((q) => [q.start, q.end]),
        syncQuarantines.map((q) => [q.start, q.end]),
        `${name}: web stream ranges`,
      );
    }
  });
});

test("LegacyStrict", async (t) => {
  await t.test("strict closing quote error is unchanged without the option", () => {
    assert.throws(
      () =>
        parseSync(
          "ok,a,b\nbad,\"x\"oops,z\na,b,c\nc,d,e\n",
          { columns: ["c1", "c2", "c3"], record_delimiter: "\n" },
        ),
      (err) => err.code === "CSV_INVALID_CLOSING_QUOTE",
    );
  });

  await t.test("strict unclosed quote error is unchanged without the option", () => {
    assert.throws(
      () => parseSync(fixtures.eofInsufficient, {
        columns: ["a", "b", "c"],
        record_delimiter: "\n",
      }),
      (err) => err.code === "CSV_QUOTE_NOT_CLOSED",
    );
  });

  await t.test("strict column count errors are unchanged without the option", () => {
    assert.throws(
      () =>
        parseSync("a,b,c\n1,2\n", {
          columns: ["a", "b", "c"],
          record_delimiter: "\n",
        }),
      (err) => err.code === "CSV_RECORD_INCONSISTENT_COLUMNS",
    );
    assert.throws(
      () => parseSync("1,2,3\n4,5\n", { record_delimiter: "\n" }),
      (err) => err.code === "CSV_RECORD_INCONSISTENT_FIELDS_LENGTH",
    );
  });

  await t.test("strict max record size error is unchanged without the option", () => {
    assert.throws(
      () =>
        parseSync(fixtures.maxRecordSize, {
          columns: ["a", "b", "c"],
          record_delimiter: "\n",
          max_record_size: 10,
        }),
      (err) => err.code === "CSV_MAX_RECORD_SIZE",
    );
  });

  await t.test("skip_records_with_error keeps its original behavior", () => {
    const skipped = [];
    const records = parseSync(fixtures.badClosingQuote, {
      columns: ["a", "b", "c"],
      record_delimiter: "\n",
      skip_records_with_error: true,
      on_skip: (err) => skipped.push(err.code),
    });
    assert.ok(skipped.includes("CSV_INVALID_CLOSING_QUOTE"));
    // The legacy mode parses on along the current state, it does not publish
    // the quarantine report introduced by recover_records.
    assert.ok(Array.isArray(records));
  });

  await t.test("errors thrown by user callbacks still terminate", () => {
    const data = fixtures.badClosingQuote;
    assert.throws(
      () =>
        parseSync(
          data,
          recoverConfig([], { cast: () => { throw new Error("boom-cast"); } }),
        ),
      /boom-cast/,
    );
    assert.throws(
      () =>
        parseSync(
          data,
          recoverConfig([], {
            on_record: () => {
              throw new Error("boom-record");
            },
          }),
        ),
      /boom-record/,
    );
    assert.throws(
      () =>
        parseSync(data, {
          columns: ["a", "b", "c"],
          record_delimiter: "\n",
          recover_records: {
            field_count: 3,
            confirm: 2,
            on_quarantine: () => {
              throw new Error("boom-quarantine");
            },
          },
        }),
      /boom-quarantine/,
    );
  });
});
