// Generates a minimal TrueType font used by checks/check_source_ranges.mjs.
// The font exists because no font in test/data provides a ligature
// substitution (GSUB type 4) with an IgnoreMarks lookup flag, which is needed
// to verify that a mark skipped during ligature matching keeps its own source
// range instead of being folded into the ligature's.
//
// The font has five glyphs, all with empty outlines:
//   0: .notdef
//   1: 'a'        (U+0061)
//   2: 'b'        (U+0062)
//   3: acutecomb  (U+0301, a combining mark)
//   4: ab_lig     (ungencoded ligature of 'a' + 'b')
//
// Its GSUB table has a single 'liga' feature for the 'latn' script with one
// ligature substitution lookup flagged IgnoreMarks: sub a b by ab_lig.
//
// Usage: node make-source-test-font.mjs <output-path>

import { writeFileSync } from 'node:fs';

class Builder {
  constructor() {
    this.parts = [];
    this.length = 0;
  }

  u8(v) {
    return this.bytes([v & 0xff]);
  }

  u16(v) {
    return this.bytes([(v >> 8) & 0xff, v & 0xff]);
  }

  i16(v) {
    return this.u16(v & 0xffff);
  }

  u32(v) {
    return this.bytes([
      (v >>> 24) & 0xff,
      (v >>> 16) & 0xff,
      (v >>> 8) & 0xff,
      v & 0xff,
    ]);
  }

  tag(s) {
    return this.bytes([...s].map((c) => c.charCodeAt(0)));
  }

  bytes(arr) {
    this.parts.push(Uint8Array.from(arr));
    this.length += arr.length;
    return this;
  }

  pad4() {
    while (this.length % 4 !== 0) {
      this.u8(0);
    }
    return this;
  }

  build() {
    let out = new Uint8Array(this.length);
    let offset = 0;
    for (let part of this.parts) {
      out.set(part, offset);
      offset += part.length;
    }
    return out;
  }
}

function buildHead() {
  let b = new Builder();
  b.u32(0x00010000); // version
  b.u32(0x00010000); // fontRevision
  b.u32(0); // checkSumAdjustment
  b.u32(0x5f0f3cf5); // magicNumber
  b.u16(0); // flags
  b.u16(1000); // unitsPerEm
  b.u32(0).u32(0); // created
  b.u32(0).u32(0); // modified
  b.i16(0).i16(0).i16(0).i16(0); // bbox
  b.u16(0); // macStyle
  b.u16(8); // lowestRecPPEM
  b.i16(2); // fontDirectionHint
  b.i16(0); // indexToLocFormat: short offsets
  b.i16(0); // glyphDataFormat
  return b.build();
}

function buildHhea() {
  let b = new Builder();
  b.u32(0x00010000); // version
  b.i16(800); // ascent
  b.i16(-200); // descent
  b.i16(0); // lineGap
  b.u16(500); // advanceWidthMax
  b.i16(0).i16(0).i16(0); // minLSB, minRSB, xMaxExtent
  b.i16(1).i16(0).i16(0); // caretSlopeRise, caretSlopeRun, caretOffset
  b.i16(0).i16(0).i16(0).i16(0); // reserved
  b.i16(0); // metricDataFormat
  b.u16(5); // numberOfMetrics
  return b.build();
}

function buildMaxp() {
  let b = new Builder();
  b.u32(0x00010000); // version
  b.u16(5); // numGlyphs
  for (let i = 0; i < 13; i++) b.u16(0);
  return b.build();
}

function buildHmtx() {
  let b = new Builder();
  for (let i = 0; i < 5; i++) {
    b.u16(500).i16(0); // advance, lsb
  }
  return b.build();
}

function buildLoca() {
  // Glyph 0 has a 10-byte empty header; glyphs 1-4 are empty (zero length).
  let b = new Builder();
  for (let offset of [0, 5, 5, 5, 5, 5]) b.u16(offset); // short: actual / 2
  return b.build();
}

function buildGlyf() {
  let b = new Builder();
  b.i16(0).i16(0).i16(0).i16(0).i16(0); // numberOfContours=0, empty bbox
  return b.build();
}

function buildCmap() {
  // A single Unicode BMP (platform 0, encoding 3) format 4 subtable mapping
  // U+0301 -> 3, U+0061 -> 1, U+0062 -> 2 via idDelta. Segments are sorted
  // by endCode as the spec requires.
  let sub = new Builder();
  sub.u16(4); // format
  sub.u16(40); // length
  sub.u16(0); // language
  sub.u16(6); // segCountX2 (3 segments)
  sub.u16(4); // searchRange
  sub.u16(1); // entrySelector
  sub.u16(2); // rangeShift
  sub.u16(0x0062).u16(0x0301).u16(0xffff); // endCode
  sub.u16(0); // reservedPad
  sub.u16(0x0061).u16(0x0301).u16(0xffff); // startCode
  sub
    .i16(1 - 0x0061)
    .i16(3 - 0x0301)
    .i16(1); // idDelta
  sub.u16(0).u16(0).u16(0); // idRangeOffset

  let b = new Builder();
  b.u16(0); // version
  b.u16(1); // numTables
  b.u16(0).u16(3).u32(12); // platform 0, encoding 3, offset
  b.bytes(sub.build());
  return b.build();
}

function buildName() {
  let b = new Builder();
  b.u16(0).u16(0).u16(6); // format 0, no records, string storage at 6
  return b.build();
}

function buildPost() {
  let b = new Builder();
  b.u32(0x00030000); // format 3.0 (no glyph names)
  b.u32(0); // italicAngle
  b.i16(0).i16(0); // underlinePosition, underlineThickness
  b.u32(0).u32(0).u32(0).u32(0).u32(0); // isFixedPitch + memory usage
  return b.build();
}

function buildGSUB() {
  // ScriptList: one 'latn' script with a default LangSys referencing feature 0.
  let scriptList = new Builder();
  scriptList.u16(1); // scriptCount
  scriptList.tag('latn').u16(8); // record -> Script at offset 8
  scriptList.u16(4).u16(0); // Script: defaultLangSys at 4, no lang sys records
  scriptList.u16(0).u16(0xffff).u16(1).u16(0); // LangSys: required=none, feature 0

  // FeatureList: one 'liga' feature referencing lookup 0.
  let featureList = new Builder();
  featureList.u16(1); // featureCount
  featureList.tag('liga').u16(8); // record -> Feature at offset 8
  featureList.u16(0).u16(1).u16(0); // Feature: no params, lookup 0

  // LookupList: one ligature substitution lookup with the IgnoreMarks flag.
  let ligature = new Builder();
  ligature.u16(4).u16(2).u16(2); // ligGlyph=ab_lig, compCount=2, components=[b]

  let ligatureSet = new Builder();
  ligatureSet.u16(1).u16(4); // ligatureCount=1, first at offset 4
  ligatureSet.bytes(ligature.build());

  let coverage = new Builder();
  coverage.u16(1).u16(1).u16(1); // format 1, one glyph: 'a' (gid 1)

  let subtable = new Builder();
  subtable.u16(1); // substFormat 1
  subtable.u16(8 + ligatureSet.length); // coverage offset
  subtable.u16(1); // ligSetCount
  subtable.u16(8); // ligatureSet offset
  subtable.bytes(ligatureSet.build());
  subtable.bytes(coverage.build());

  let lookup = new Builder();
  lookup.u16(4); // lookupType: ligature substitution
  lookup.u16(0x0008); // lookupFlag: IgnoreMarks
  lookup.u16(1).u16(8); // subTableCount=1, first at offset 8
  lookup.bytes(subtable.build());

  let lookupList = new Builder();
  lookupList.u16(1).u16(4); // lookupCount=1, first at offset 4
  lookupList.bytes(lookup.build());

  let gsub = new Builder();
  gsub.u32(0x00010000); // version 1.0
  gsub.u16(10); // scriptList offset
  gsub.u16(10 + scriptList.length); // featureList offset
  gsub.u16(10 + scriptList.length + featureList.length); // lookupList offset
  gsub.bytes(scriptList.build());
  gsub.bytes(featureList.build());
  gsub.bytes(lookupList.build());
  return gsub.build();
}

// Builds the font and returns it as a Buffer.
export function buildSourceTestFont() {
  let tables = {
    GSUB: buildGSUB(),
    cmap: buildCmap(),
    glyf: buildGlyf(),
    head: buildHead(),
    hhea: buildHhea(),
    hmtx: buildHmtx(),
    loca: buildLoca(),
    maxp: buildMaxp(),
    name: buildName(),
    post: buildPost(),
  };

  let tags = Object.keys(tables).sort();
  let numTables = tags.length;
  let maxPower = 1;
  let entrySelector = 0;
  while (maxPower * 2 <= numTables) {
    maxPower *= 2;
    entrySelector++;
  }

  let font = new Builder();
  font.u32(0x00010000); // scaler type
  font.u16(numTables);
  font.u16(maxPower * 16); // searchRange
  font.u16(entrySelector);
  font.u16(numTables * 16 - maxPower * 16); // rangeShift

  let offset = 12 + numTables * 16;
  let records = [];
  let data = new Builder();
  for (let tag of tags) {
    let table = tables[tag];
    records.push({ tag, offset, length: table.length });
    data.bytes(table).pad4();
    offset = 12 + numTables * 16 + data.length;
  }

  for (let record of records) {
    font.tag(record.tag);
    font.u32(0); // checksum (not verified by fontkit)
    font.u32(record.offset);
    font.u32(record.length);
  }
  font.bytes(data.build());

  return Buffer.from(font.build());
}

if (import.meta.url === `file://${process.argv[1]}`) {
  let out = process.argv[2];
  if (!out) {
    console.error('usage: node make-source-test-font.mjs <output-path>');
    process.exit(1);
  }

  writeFileSync(out, buildSourceTestFont());
  console.log(`wrote ${out}`);
}
