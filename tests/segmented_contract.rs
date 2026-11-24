//! Independent contract tests for segmented (banked) DWARF reading.
//!
//! Every byte vector below is hand encoded inside this file: the selectors,
//! offsets, function names, source lines and contribution orders differ from
//! the fixed `fixtures/banked-fw` sample on purpose. Expected values are
//! written literally next to the assertions and are never derived from the
//! analyzer under test.

use gimli::{
    ArangeEntry, BigEndian, DebugAbbrev, DebugAranges, DebugInfo, DebugLine, DebugLineOffset,
    Dwarf, EndianSlice, Expression, LittleEndian, LineInstruction, Operation, SectionId,
};
use std::collections::HashMap;

use crate::support::{Cases, Check};

/// Register the five required contract groups exactly once.
pub fn register(cases: &mut Cases) {
    cases.add("aranges_alignment", aranges_alignment);
    cases.add("line_selector", line_selector);
    cases.add("sequence_identity", sequence_identity);
    cases.add("flat_compatibility", flat_compatibility);
    cases.add("bank_attribution", bank_attribution);
}

// ---------------------------------------------------------------------------
// Hand encoding helpers.
// ---------------------------------------------------------------------------

fn u16le(value: u16) -> Vec<u8> {
    value.to_le_bytes().to_vec()
}
fn u32le(value: u32) -> Vec<u8> {
    value.to_le_bytes().to_vec()
}
fn u16be(value: u16) -> Vec<u8> {
    value.to_be_bytes().to_vec()
}
fn u32be(value: u32) -> Vec<u8> {
    value.to_be_bytes().to_vec()
}
fn u64le(value: u64) -> Vec<u8> {
    value.to_le_bytes().to_vec()
}
fn u64be(value: u64) -> Vec<u8> {
    value.to_be_bytes().to_vec()
}

fn uleb(mut value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let mut byte = (value & 0x7f) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            return out;
        }
    }
}

fn sleb(mut value: i64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        let done = value == 0 && byte & 0x40 == 0 || value == -1 && byte & 0x40 != 0;
        if done {
            out.push(byte);
            return out;
        }
        out.push(byte | 0x80);
    }
}

fn sized(value: u64, size: u8, big_endian: bool) -> Vec<u8> {
    match size {
        1 => vec![value as u8],
        2 if big_endian => u16be(value as u16),
        2 => u16le(value as u16),
        4 if big_endian => u32be(value as u32),
        4 => u32le(value as u32),
        8 if big_endian => u64be(value),
        8 => u64le(value),
        _ => panic!("unsupported field size {size}"),
    }
}

fn cstr(text: &str) -> Vec<u8> {
    let mut bytes = text.as_bytes().to_vec();
    bytes.push(0);
    bytes
}

/// One arange tuple as encoded on the wire: selector, begin offset, length.
type WireTuple = (u64, u64, u64);

/// Hand encode one `.debug_aranges` contribution with explicit padding and a
/// trailing null tuple.
fn arange_contribution(
    big_endian: bool,
    dwarf64: bool,
    address_size: u8,
    segment_size: u8,
    debug_info_offset: u64,
    tuples: &[WireTuple],
) -> Vec<u8> {
    let mut body = Vec::new();
    if big_endian {
        body.extend(u16be(2));
    } else {
        body.extend(u16le(2));
    }
    if dwarf64 {
        body.extend(sized(debug_info_offset, 8, big_endian));
    } else {
        body.extend(sized(debug_info_offset, 4, big_endian));
    }
    body.push(address_size);
    body.push(segment_size);

    let initial_length_size = if dwarf64 { 12u8 } else { 4 };
    let word_size = if dwarf64 { 8u8 } else { 4 };
    let header_length = initial_length_size + 2 + word_size + 1 + 1;
    let tuple_length = 2 * address_size + segment_size;
    let padding = if header_length % tuple_length == 0 {
        0
    } else {
        tuple_length - header_length % tuple_length
    };
    body.extend(std::iter::repeat_n(0u8, padding as usize));

    for (selector, begin, length) in tuples {
        if segment_size != 0 {
            body.extend(sized(*selector, segment_size, big_endian));
        }
        body.extend(sized(*begin, address_size, big_endian));
        body.extend(sized(*length, address_size, big_endian));
    }
    body.extend(std::iter::repeat_n(0u8, tuple_length as usize));

    let mut out = Vec::new();
    if dwarf64 {
        // The 0xffffffff marker is endian-independent; the real length follows
        // the contribution endianity.
        out.extend(u32le(0xffff_ffff));
        out.extend(sized(body.len() as u64, 8, big_endian));
    } else {
        out.extend(sized(body.len() as u64, 4, big_endian));
    }
    out.extend(body);
    out
}

/// DWARF 5 line program header parameters shared by the hand builders.
struct LineSpec {
    address_size: u8,
    segment_size: u8,
    big_endian: bool,
    directories: Vec<String>,
    files: Vec<String>,
}

impl LineSpec {
    fn wrap(&self, version: u16, prologue: &[u8], body: &[u8]) -> Vec<u8> {
        let mut rest = Vec::new();
        if self.big_endian {
            rest.extend(u16be(version));
        } else {
            rest.extend(u16le(version));
        }
        if version >= 5 {
            rest.push(self.address_size);
            rest.push(self.segment_size);
        }
        rest.extend(sized(prologue.len() as u64, 4, self.big_endian));
        rest.extend_from_slice(prologue);
        rest.extend_from_slice(body);

        let mut out = Vec::new();
        out.extend(sized(rest.len() as u64, 4, self.big_endian));
        out.extend(rest);
        out
    }

    fn line_encoding_bytes() -> Vec<u8> {
        let mut prologue = Vec::new();
        prologue.push(1); // minimum_instruction_length
        prologue.push(1); // maximum_operations_per_instruction (v5)
        prologue.push(1); // default_is_stmt
        prologue.push(0xfb); // line_base = -5
        prologue.push(14); // line_range
        prologue.push(13); // opcode_base
        prologue.extend_from_slice(&[0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1]); // DW_LNS 1..12 operand counts
        prologue
    }

    fn dwarf5(&self, body: &[u8]) -> Vec<u8> {
        let mut prologue = Self::line_encoding_bytes();
        // Directory entry format: one LNCT_path/DW_FORM_string pair.
        prologue.push(1);
        prologue.extend(uleb(1));
        prologue.extend(uleb(8));
        prologue.extend(uleb(self.directories.len() as u64));
        for dir in &self.directories {
            prologue.extend(cstr(dir));
        }
        // File entry format: one LNCT_path/DW_FORM_string pair.
        prologue.push(1);
        prologue.extend(uleb(1));
        prologue.extend(uleb(8));
        prologue.extend(uleb(self.files.len() as u64));
        for file in &self.files {
            prologue.extend(cstr(file));
        }
        self.wrap(5, &prologue, body)
    }

    fn dwarf4_flat(&self, body: &[u8]) -> Vec<u8> {
        let mut prologue = Vec::new();
        prologue.push(1); // minimum_instruction_length
        prologue.push(1); // maximum_operations_per_instruction
        prologue.push(1); // default_is_stmt
        prologue.push(0xfb); // line_base
        prologue.push(14); // line_range
        prologue.push(13); // opcode_base
        prologue.extend_from_slice(&[0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1]); // DW_LNS 1..12 operand counts
        for dir in &self.directories {
            prologue.extend(cstr(dir));
        }
        prologue.push(0);
        for file in &self.files {
            prologue.extend(cstr(file));
            prologue.extend(uleb(0));
            prologue.extend(uleb(0));
            prologue.extend(uleb(0));
        }
        prologue.push(0);
        self.wrap(4, &prologue, body)
    }
}

/// Encode a DW_LNE_set_address extended opcode.
fn ext_set_address(segment_size: u8, address_size: u8, segment: u64, address: u64) -> Vec<u8> {
    let mut payload = vec![2u8]; // DW_LNE_set_address
    if segment_size != 0 {
        payload.extend(sized(segment, segment_size, false));
    }
    payload.extend(sized(address, address_size, false));
    let mut out = vec![0u8];
    out.extend(uleb(payload.len() as u64));
    out.extend(payload);
    out
}

fn ext_end_sequence() -> Vec<u8> {
    vec![0, 1, 1]
}

fn lns_copy() -> Vec<u8> {
    vec![1]
}

fn lns_advance_pc(advance: u64) -> Vec<u8> {
    let mut out = vec![2u8];
    out.extend(uleb(advance));
    out
}

fn lns_advance_line(delta: i64) -> Vec<u8> {
    let mut out = vec![3u8];
    out.extend(sleb(delta));
    out
}

fn lns_set_file(index: u64) -> Vec<u8> {
    let mut out = vec![4u8];
    out.extend(uleb(index));
    out
}

fn lns_negate_stmt() -> Vec<u8> {
    vec![6]
}

/// Special opcode for a line delta and operation advance, given line_base=-5
/// and line_range=14.
fn special(line_delta: i64, operation_advance: u64) -> u8 {
    let adjusted = (line_delta - -5) + 14 * operation_advance as i64;
    (13 + adjusted) as u8
}

// ---------------------------------------------------------------------------
// Group 1: aranges alignment, widths, formats, endianity and null handling.
// ---------------------------------------------------------------------------

fn aranges_alignment(check: &mut Check) {
    // First contribution: DWARF32, little endian, 4-byte offsets, 1-byte
    // selectors. Header length is 12 bytes while tuples are 9 bytes, so six
    // padding bytes are required before the first tuple.
    let first = arange_contribution(
        false,
        false,
        4,
        1,
        0x40,
        &[
            // A legal range whose offset is zero but whose selector is nonzero.
            (7, 0x0, 0x4),
            (7, 0x100, 0x10),
            (0, 0x200, 0x8),
        ],
    );
    // Second contribution in the same little-endian section: DWARF64 with
    // 2-byte offsets and selectors. Header length 24 is already a multiple of
    // the 6-byte tuples.
    let second_le = arange_contribution(false, true, 2, 2, 0x80, &[(0x1234, 0x40, 0x6)]);
    let second_start = first.len() as u64;
    let mut section = first;
    section.extend(second_le);

    let debug_aranges = DebugAranges::new(&section, LittleEndian);
    let mut headers = debug_aranges.headers();
    let header_a = headers.next().unwrap().unwrap();
    check.equal(header_a.segment_size(), 1);
    check.equal(header_a.debug_info_offset().0, 0x40);
    // 4-byte initial length + 8 header bytes + 6 bytes padding = 0x12.
    check.equal(header_a.entries_offset().0, 0x12);

    let mut entries = header_a.entries();
    let raw_zero = entries.next_raw().unwrap().unwrap();
    check.equal(raw_zero.segment(), 7);
    check.equal(raw_zero.address(), 0);
    check.equal(raw_zero.tuple_offset(), 0x12);
    // Raw entries keep the deferred end at zero.
    check.equal(raw_zero.range().end, 0);
    let converted = entries.convert_raw(raw_zero).unwrap().unwrap();
    check.equal(converted.range().end, 0x4);
    check.equal(converted.segment(), 7);

    let entry_b = entries.next().unwrap().unwrap();
    check.equal(entry_b.segment(), 7);
    check.equal(entry_b.address(), 0x100);
    check.equal(entry_b.length(), 0x10);
    check.equal(entry_b.range().end, 0x110);
    check.equal(entry_b.tuple_offset(), 0x12 + 9);

    let entry_c = entries.next().unwrap().unwrap();
    check.equal(entry_c.segment(), 0);
    check.equal(entry_c.address(), 0x200);
    // The all-zero null tuple is consumed automatically.
    check.require(entries.next().unwrap().is_none(), "null tuple terminates set");

    let header_b = headers.next().unwrap().unwrap();
    check.equal(header_b.segment_size(), 2);
    check.equal(header_b.debug_info_offset().0, 0x80);
    check.equal(header_b.encoding().format, gimli::Format::Dwarf64);
    let mut entries_b = header_b.entries();
    let big = entries_b.next().unwrap().unwrap();
    check.equal(big.segment(), 0x1234);
    check.equal(big.address(), 0x40);
    check.equal(big.length(), 0x6);
    check.equal(big.tuple_offset(), second_start + 24);
    check.require(entries_b.next().unwrap().is_none(), "second set terminates");
    check.require(headers.next().unwrap().is_none(), "no third contribution");

    // A separate big-endian section proves endianity is honored per section.
    let be_section = arange_contribution(true, true, 2, 2, 0x90, &[(0x1234, 0x40, 0x6)]);
    let be_aranges = DebugAranges::new(&be_section, BigEndian);
    let be_header = be_aranges.headers().next().unwrap().unwrap();
    let be_big = be_header.entries().next().unwrap().unwrap();
    check.equal(be_header.segment_size(), 2);
    check.equal(be_header.debug_info_offset().0, 0x90);
    check.equal(be_big.segment(), 0x1234);
    check.equal(be_big.address(), 0x40);

    // Independent byte-level replay at the reported tuple offsets.
    let tuple_off = big.tuple_offset() as usize;
    check.equal(section[tuple_off], 0x34);
    check.equal(section[tuple_off + 1], 0x12);
    check.equal(section[tuple_off + 2], 0x40);
    check.equal(section[tuple_off + 3], 0x00);
    let be_off = be_big.tuple_offset() as usize;
    check.equal(be_section[be_off], 0x12);
    check.equal(be_section[be_off + 1], 0x34);
    check.equal(be_section[be_off + 2], 0x00);
    check.equal(be_section[be_off + 3], 0x40);

    // Selector widths 4 and 8 must parse as well as 0, 1 and 2.
    for (segment_size, selector, encoded) in [
        (4u8, 0x0102_0304u64, vec![0x04, 0x03, 0x02, 0x01]),
        (
            8u8,
            0x0102_0304_0506_0708u64,
            vec![0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01],
        ),
    ] {
        let wide = arange_contribution(
            false,
            false,
            4,
            segment_size,
            0xa0,
            &[(selector, 0x900, 0x10)],
        );
        let wide_aranges = DebugAranges::new(&wide, LittleEndian);
        let wide_header = wide_aranges.headers().next().unwrap().unwrap();
        check.equal(wide_header.segment_size(), segment_size);
        let wide_entry = wide_header.entries().next().unwrap().unwrap();
        check.equal(wide_entry.segment(), selector);
        check.equal(wide_entry.address(), 0x900);
        check.equal(wide_entry.range().end, 0x910);
        let wide_off = wide_entry.tuple_offset() as usize;
        check.equal(
            wide[wide_off..wide_off + segment_size as usize].to_vec(),
            encoded,
        );
    }
}

// ---------------------------------------------------------------------------
// Group 2: line program selector widths and instruction variants.
// ---------------------------------------------------------------------------

fn line_selector(check: &mut Check) {
    // DWARF 5 program with a one-byte selector and two sequences.
    let body = {
        let mut body = Vec::new();
        body.extend(ext_set_address(1, 4, 0x21, 0x100));
        body.extend(lns_negate_stmt());
        body.extend(lns_copy());
        body.extend(lns_advance_pc(1));
        body.push(special(1, 0));
        body.extend(ext_end_sequence());
        body.extend(ext_set_address(1, 4, 0x34, 0x200));
        body.extend(lns_copy());
        body.extend(ext_end_sequence());
        body
    };
    let spec = LineSpec {
        address_size: 4,
        segment_size: 1,
        big_endian: false,
        directories: vec![String::from("firmware")],
        files: vec![String::from("a.c"), String::from("b.c")],
    };
    let section = spec.dwarf5(&body);
    let debug_line = DebugLine::new(&section, LittleEndian);
    let program = debug_line.program(DebugLineOffset(0), 4, None, None).unwrap();
    check.equal(program.header().segment_size(), 1);

    let mut instructions = program.header().instructions();
    let first = instructions.next_instruction(program.header()).unwrap().unwrap();
    check.equal(
        first,
        LineInstruction::SetSegmentedAddress {
            segment: 0x21,
            address: 0x100,
        },
    );
    let mut found_second = false;
    while let Some(instruction) = instructions
        .next_instruction(program.header())
        .unwrap()
    {
        if let LineInstruction::SetSegmentedAddress { segment, address } = instruction {
            check.equal(segment, 0x34);
            check.equal(address, 0x200);
            found_second = true;
        }
    }
    check.require(found_second, "second segmented set_address decoded");

    // Executing the program keeps selector fixed while offsets advance.
    let program = debug_line.program(DebugLineOffset(0), 4, None, None).unwrap();
    let (_, sequences) = program.sequences().unwrap();
    check.equal(sequences.len(), 2);
    check.equal(sequences[0].segment, 0x21);
    check.equal(sequences[1].segment, 0x34);

    // A two-byte selector encoded big endian must decode in wire order.
    let be_body = {
        let mut payload = vec![2u8];
        payload.extend(u16be(0x0a0b));
        payload.extend(u32be(0x0102_0304));
        let mut body = vec![0u8];
        body.extend(uleb(payload.len() as u64));
        body.extend(payload);
        body.extend(ext_end_sequence());
        body
    };
    let be_spec = LineSpec {
        address_size: 4,
        segment_size: 2,
        big_endian: true,
        directories: vec![String::from("firmware")],
        files: vec![String::from("be.c")],
    };
    let be_section = be_spec.dwarf5(&be_body);
    let be_line = DebugLine::new(&be_section, BigEndian);
    let be_program = be_line.program(DebugLineOffset(0), 4, None, None).unwrap();
    let be_first = be_program
        .header()
        .instructions()
        .next_instruction(be_program.header())
        .unwrap()
        .unwrap();
    check.equal(
        be_first,
        LineInstruction::SetSegmentedAddress {
            segment: 0x0a0b,
            address: 0x0102_0304,
        },
    );
}

// ---------------------------------------------------------------------------
// Group 3: sequence identity across reversed and repeated resumes.
// ---------------------------------------------------------------------------

fn sequence_identity(check: &mut Check) {
    // Two sequences at the same offsets but in different segments.
    let body = {
        let mut body = Vec::new();
        // Sequence 0: selector 3, offsets 0x300..0x308.
        body.extend(ext_set_address(1, 4, 3, 0x300));
        body.extend(lns_set_file(0));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(1));
        body.push(special(1, 0));
        body.extend(lns_advance_pc(1));
        body.extend(ext_end_sequence());
        // Sequence 1: selector 9, identical offsets, distinct file.
        body.extend(ext_set_address(1, 4, 9, 0x300));
        body.extend(lns_set_file(1));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(1));
        body.push(special(1, 0));
        body.extend(lns_advance_pc(1));
        body.extend(ext_end_sequence());
        body
    };
    let spec = LineSpec {
        address_size: 4,
        segment_size: 1,
        big_endian: false,
        directories: vec![String::from("src")],
        files: vec![String::from("same_a.c"), String::from("same_b.c")],
    };
    let section = spec.dwarf5(&body);
    let debug_line = DebugLine::new(&section, LittleEndian);
    let program = debug_line.program(DebugLineOffset(0), 4, None, None).unwrap();
    let (complete, sequences) = program.sequences().unwrap();
    check.equal(sequences.len(), 2);
    check.equal(sequences[0].segment, 3);
    check.equal(sequences[1].segment, 9);
    check.equal(sequences[0].start, 0x300);
    check.equal(sequences[1].start, 0x300);

    let collect = |sequence_index: usize| -> Vec<(u64, u64, bool)> {
        let mut rows = complete.resume_from(&sequences[sequence_index]);
        let mut out = Vec::new();
        while let Some((_, row)) = rows.next_row().unwrap() {
            out.push((row.segment(), row.address(), row.end_sequence()));
        }
        out
    };

    // Reverse-order resume.
    let reversed = collect(1);
    check.equal(reversed[0], (9, 0x300, false));
    check.equal(reversed[1], (9, 0x301, false));
    check.equal(reversed[2], (9, 0x302, true));
    let first = collect(0);
    check.equal(first[0], (3, 0x300, false));
    check.equal(first[1], (3, 0x301, false));
    // The terminating row keeps the current selector rather than resetting.
    check.equal(first[2], (3, 0x302, true));

    // Repeated resumes reproduce the same selector and rows.
    check.equal(collect(0), first.clone());
    check.equal(collect(0), first);

    // A fresh row starts at selector 0 and offset 0.
    let fresh_row = gimli::LineRow::new(complete.header());
    check.equal(fresh_row.segment(), 0);
    check.equal(fresh_row.address(), 0);
}

// ---------------------------------------------------------------------------
// Group 4: flat (segmentless) compatibility of the old interfaces.
// ---------------------------------------------------------------------------

fn flat_compatibility(check: &mut Check) {
    // Classic flat aranges, identical in shape to the upstream baseline.
    let aranges = arange_contribution(false, false, 4, 0, 0x40, &[(0, 0x1000, 0x20)]);
    let debug_aranges = DebugAranges::new(&aranges, LittleEndian);
    let header = debug_aranges.headers().next().unwrap().unwrap();
    check.equal(header.segment_size(), 0);
    let entry: ArangeEntry = header.entries().next().unwrap().unwrap();
    check.equal(entry.segment(), 0);
    check.equal(entry.address(), 0x1000);
    check.equal(entry.range().begin, 0x1000);
    check.equal(entry.range().end, 0x1020);

    // DWARF 4 line programs have no segment selector field at all.
    let body = {
        let mut body = Vec::new();
        body.extend(ext_set_address(0, 4, 0, 0x500));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(2));
        body.extend(ext_end_sequence());
        body
    };
    let spec = LineSpec {
        address_size: 4,
        segment_size: 0,
        big_endian: false,
        directories: vec![String::from("dir")],
        files: vec![String::from("flat.c")],
    };
    let line_section = spec.dwarf4_flat(&body);
    let debug_line = DebugLine::new(&line_section, LittleEndian);
    let program = debug_line.program(DebugLineOffset(0), 4, None, None).unwrap();
    check.equal(program.header().segment_size(), 0);
    let instruction = program
        .header()
        .instructions()
        .next_instruction(program.header())
        .unwrap()
        .unwrap();
    check.equal(instruction, LineInstruction::SetAddress(0x500));
    let (complete, sequences) = program.sequences().unwrap();
    check.equal(sequences[0].segment, 0);
    check.equal(sequences[0].start, 0x500);
    check.equal(sequences[0].end, 0x502);
    let mut rows = complete.resume_from(&sequences[0]);
    let (_, row) = rows.next_row().unwrap().unwrap();
    check.equal(row.segment(), 0);
    check.equal(row.address(), 0x500);

    // End-to-end flat lookups through the shared production API.
    let sample = build_synthetic_sample(false);
    // Independently compute the root DIE offsets through the real header reader
    // instead of hardcoding them.
    let debug_info = DebugInfo::new(&sample.info, LittleEndian);
    let mut unit_headers = Vec::new();
    {
        let mut iter = debug_info.units();
        while let Some(header) = iter.next().unwrap() {
            unit_headers.push(header);
        }
    }
    let root2 = unit_headers[1]
        .root_offset()
        .to_debug_info_offset(&unit_headers[1])
        .unwrap()
        .0;
    let results = gimli::segmented_lookup(
        &sample.sections(LittleEndian),
        &[
            gimli::SegmentedQuery {
                selector: 0,
                offset: 0x500,
            },
            gimli::SegmentedQuery {
                selector: 0,
                offset: 0x800,
            },
        ],
        &sample.banks,
    )
    .unwrap();
    check.equal(results.len(), 2);
    // Explicit empty expression: flat space, but a provider DIE is reported.
    check.equal(results[0].function.as_deref(), Some("flat_task"));
    check.equal(results[0].segment_die, Some(root2 as u64));
    // No segment attribute anywhere in the ancestry: no provider at all.
    check.equal(results[1].function.as_deref(), Some("plain_task"));
    check.require(
        results[1].segment_die.is_none(),
        "attribute-free flat function has no segment DIE",
    );
    check.equal(results[1].die, Some(sample.plain_task_die));

    // An unsupported multi-operation segment expression must fail loudly.
    let bad = build_synthetic_sample(true);
    let bad_result = gimli::segmented_lookup(
        &bad.sections(LittleEndian),
        &[gimli::SegmentedQuery {
            selector: 7,
            offset: 0x204,
        }],
        &bad.banks,
    );
    check.require(bad_result.is_err(), "non-constu expressions are rejected");
}

// ---------------------------------------------------------------------------
// Synthetic end-to-end sample shared by groups 4 and 5.
// ---------------------------------------------------------------------------

pub(crate) struct SyntheticSample {
    pub(crate) abbrev: Vec<u8>,
    pub(crate) info: Vec<u8>,
    pub(crate) line: Vec<u8>,
    pub(crate) aranges: Vec<u8>,
    pub(crate) banks: Vec<gimli::BankMapping>,
    pub(crate) cu2_offset: u64,
    pub(crate) root1_die: u64,
    pub(crate) alpha_die: u64,
    pub(crate) beta_die: u64,
    pub(crate) silent_die: u64,
    pub(crate) plain_task_die: u64,
    pub(crate) cu1_aranges_offset: u64,
}

impl SyntheticSample {
    fn sections<'a, Endian: gimli::Endianity>(
        &'a self,
        endian: Endian,
    ) -> gimli::SegmentedSections<'a, Endian> {
        gimli::SegmentedSections {
            endian,
            debug_aranges: &self.aranges,
            debug_info: &self.info,
            debug_abbrev: &self.abbrev,
            debug_line: &self.line,
        }
    }
}

pub(crate) fn build_synthetic_sample(bad_segment: bool) -> SyntheticSample {
    let mut abbrev = Vec::new();
    // Code 1: compile unit with name, stmt_list and a segment exprloc.
    abbrev.extend(uleb(1));
    abbrev.extend(uleb(0x11)); // DW_TAG_compile_unit
    abbrev.push(1); // DW_CHILDREN_yes
    abbrev.extend(uleb(0x03));
    abbrev.extend(uleb(0x08)); // DW_FORM_string
    abbrev.extend(uleb(0x10)); // DW_AT_stmt_list
    abbrev.extend(uleb(0x17)); // DW_FORM_sec_offset
    abbrev.extend(uleb(0x46)); // DW_AT_segment
    abbrev.extend(uleb(0x18)); // DW_FORM_exprloc
    abbrev.extend(&[0, 0]);
    // Code 2: subprogram: name, low_pc, high_pc length.
    abbrev.extend(uleb(2));
    abbrev.extend(uleb(0x2e));
    abbrev.push(0);
    abbrev.extend(uleb(0x03));
    abbrev.extend(uleb(0x08));
    abbrev.extend(uleb(0x11)); // DW_AT_low_pc
    abbrev.extend(uleb(0x01)); // DW_FORM_addr
    abbrev.extend(uleb(0x12)); // DW_AT_high_pc
    abbrev.extend(uleb(0x06)); // DW_FORM_data4
    abbrev.extend(&[0, 0]);
    // Code 3: subprogram with an explicit segment exprloc.
    abbrev.extend(uleb(3));
    abbrev.extend(uleb(0x2e));
    abbrev.push(0);
    abbrev.extend(uleb(0x03));
    abbrev.extend(uleb(0x08));
    abbrev.extend(uleb(0x11));
    abbrev.extend(uleb(0x01));
    abbrev.extend(uleb(0x12));
    abbrev.extend(uleb(0x06));
    abbrev.extend(uleb(0x46));
    abbrev.extend(uleb(0x18));
    abbrev.extend(&[0, 0]);
    // Code 4: compile unit without any segment attribute.
    abbrev.extend(uleb(4));
    abbrev.extend(uleb(0x11));
    abbrev.push(1);
    abbrev.extend(uleb(0x03));
    abbrev.extend(uleb(0x08));
    abbrev.extend(uleb(0x10));
    abbrev.extend(uleb(0x17));
    abbrev.extend(&[0, 0]);
    abbrev.push(0);

    // Line program 1: selector 7 (line 12/13 across 0x200..0x210) then
    // selector 9 (line 27 across 0x300..0x310).
    let line1_body = {
        let mut body = Vec::new();
        body.extend(ext_set_address(1, 4, 7, 0x200));
        body.extend(lns_set_file(0));
        body.extend(lns_advance_line(11)); // initial line 1 -> 12
        body.extend(lns_copy()); // row0 at 0x200
        body.extend(lns_advance_pc(8));
        body.push(special(1, 0)); // row1 line 13 at 0x208
        body.extend(lns_advance_pc(8)); // terminator at 0x210
        body.extend(ext_end_sequence());
        body.extend(ext_set_address(1, 4, 9, 0x300));
        body.extend(lns_set_file(1));
        body.extend(lns_advance_line(26)); // reset line 1 -> 27
        body.extend(lns_copy()); // row0 at 0x300
        body.extend(lns_advance_pc(16)); // terminator at 0x310
        body.extend(ext_end_sequence());
        body
    };
    let line1 = LineSpec {
        address_size: 4,
        segment_size: 1,
        big_endian: false,
        directories: vec![String::from("firmware")],
        files: vec![String::from("alpha.c"), String::from("beta.c")],
    }
    .dwarf5(&line1_body);
    let line_program_1 = 0u64;

    // Line program 2: flat rows for flat_task (line 5) and orphan code (line 9).
    let line2_body = {
        let mut body = Vec::new();
        body.extend(ext_set_address(0, 4, 0, 0x500));
        body.extend(lns_set_file(0));
        body.extend(lns_advance_line(4));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(8));
        body.extend(ext_end_sequence());
        body.extend(ext_set_address(0, 4, 0, 0x700));
        body.extend(lns_set_file(1));
        body.extend(lns_advance_line(8));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(8));
        body.extend(ext_end_sequence());
        body
    };
    let line2 = LineSpec {
        address_size: 4,
        segment_size: 0,
        big_endian: false,
        directories: vec![String::from("firmware")],
        files: vec![String::from("gamma.c"), String::from("orphan.c")],
    }
    .dwarf5(&line2_body);
    let line_program_2 = line1.len() as u64;

    // Line program 3: attribute-free flat CU, plain_task line 3.
    let line3_body = {
        let mut body = Vec::new();
        body.extend(ext_set_address(0, 4, 0, 0x800));
        body.extend(lns_set_file(0));
        body.extend(lns_advance_line(2));
        body.extend(lns_copy());
        body.extend(lns_advance_pc(8));
        body.extend(ext_end_sequence());
        body
    };
    let line3 = LineSpec {
        address_size: 4,
        segment_size: 0,
        big_endian: false,
        directories: vec![String::from("firmware")],
        files: vec![String::from("plain.c")],
    }
    .dwarf5(&line3_body);
    let line_program_3 = (line1.len() + line2.len()) as u64;

    let mut line = line1;
    line.extend(line2);
    line.extend(line3);

    // CU 1: segment 7 on the root, beta overrides to 9.
    let cu1_offset = 0u64;
    let mut cu1 = Vec::new();
    cu1.extend(u32le(0));
    cu1.extend(u16le(5));
    cu1.push(0x01);
    cu1.push(4);
    cu1.extend(u32le(0));
    let root1_die = cu1_offset + cu1.len() as u64;
    cu1.extend(uleb(1));
    cu1.extend(cstr("alpha.c"));
    cu1.extend(u32le(line_program_1 as u32));
    if bad_segment {
        // constu 7 followed by DW_OP_lit0: an unsupported multi-op expression.
        cu1.extend(uleb(3));
        cu1.push(0x10);
        cu1.push(7);
        cu1.push(0x30);
    } else {
        cu1.extend(uleb(2));
        cu1.push(0x10);
        cu1.push(7);
    }
    let alpha_die = cu1_offset + cu1.len() as u64;
    cu1.extend(uleb(2));
    cu1.extend(cstr("alpha_step"));
    cu1.extend(u32le(0x200));
    cu1.extend(u32le(0x10));
    let beta_die = cu1_offset + cu1.len() as u64;
    cu1.extend(uleb(3));
    cu1.extend(cstr("beta_reset"));
    cu1.extend(u32le(0x300));
    cu1.extend(u32le(0x10));
    cu1.extend(uleb(2));
    cu1.push(0x10);
    cu1.push(9);
    cu1.push(0);
    let cu1_length = cu1.len() as u32 - 4;
    cu1[0..4].copy_from_slice(&cu1_length.to_le_bytes());

    // CU 2: explicit empty segment expression selects the flat space.
    let cu2_offset = cu1_offset + cu1.len() as u64;
    let mut cu2 = Vec::new();
    cu2.extend(u32le(0));
    cu2.extend(u16le(5));
    cu2.push(0x01);
    cu2.push(4);
    cu2.extend(u32le(0));
    cu2.extend(uleb(1));
    cu2.extend(cstr("gamma.c"));
    cu2.extend(u32le(line_program_2 as u32));
    cu2.extend(uleb(0)); // empty exprloc
    cu2.extend(uleb(2));
    cu2.extend(cstr("flat_task"));
    cu2.extend(u32le(0x500));
    cu2.extend(u32le(0x08));
    let silent_die = cu2_offset + cu2.len() as u64;
    // silent_work has a range but no source rows at all.
    cu2.extend(uleb(2));
    cu2.extend(cstr("silent_work"));
    cu2.extend(u32le(0x600));
    cu2.extend(u32le(0x08));
    cu2.push(0);
    let cu2_length = cu2.len() as u32 - 4;
    cu2[0..4].copy_from_slice(&cu2_length.to_le_bytes());

    // CU 3: no segment attribute, so functions resolve to flat space with no
    // segment provider DIE.
    let cu3_offset = cu2_offset + cu2.len() as u64;
    let mut cu3 = Vec::new();
    cu3.extend(u32le(0));
    cu3.extend(u16le(5));
    cu3.push(0x01);
    cu3.push(4);
    cu3.extend(u32le(0));
    cu3.extend(uleb(4));
    cu3.extend(cstr("plain.c"));
    cu3.extend(u32le(line_program_3 as u32));
    let plain_task_die = cu3_offset + cu3.len() as u64;
    cu3.extend(uleb(2));
    cu3.extend(cstr("plain_task"));
    cu3.extend(u32le(0x800));
    cu3.extend(u32le(0x08));
    cu3.push(0);
    let cu3_length = cu3.len() as u32 - 4;
    cu3[0..4].copy_from_slice(&cu3_length.to_le_bytes());

    let mut info = cu1;
    info.extend(cu2);
    info.extend(cu3);

    // Aranges contributions deliberately shuffled: CU2, then CU3, then CU1.
    let mut aranges = arange_contribution(false, false, 4, 0, cu2_offset, &[
        (0, 0x500, 0x08),
        (0, 0x600, 0x08),
        (0, 0x700, 0x08),
    ]);
    aranges.extend(arange_contribution(false, false, 4, 0, cu3_offset, &[(
        0,
        0x800,
        0x08,
    )]));
    let cu1_aranges_offset = aranges.len() as u64;
    aranges.extend(arange_contribution(false, false, 4, 1, cu1_offset, &[
        (7, 0x200, 0x10),
        (9, 0x300, 0x10),
    ]));

    let banks = vec![
        gimli::BankMapping {
            selector: 7,
            file: String::from("bank-7.bin"),
            base: 0,
            length: 0x1000,
        },
        gimli::BankMapping {
            selector: 9,
            file: String::from("bank-9.bin"),
            base: 0,
            length: 0x1000,
        },
        gimli::BankMapping {
            selector: 0,
            file: String::from("bank-flat.bin"),
            base: 0,
            length: 0x1000,
        },
        gimli::BankMapping {
            selector: 99,
            file: String::from("bank-99.bin"),
            base: 0,
            length: 0x1000,
        },
    ];

    SyntheticSample {
        abbrev,
        info,
        line,
        aranges,
        banks,
        cu2_offset,
        root1_die,
        alpha_die,
        beta_die,
        silent_die,
        plain_task_die,
        cu1_aranges_offset,
    }
}

// ---------------------------------------------------------------------------
// Group 5: attribution on the synthetic sample and the fixed fixture.
// ---------------------------------------------------------------------------

fn bank_attribution(check: &mut Check) {
    bank_attribution_synthetic(check);
    bank_attribution_fixture(check);
}

fn bank_attribution_synthetic(check: &mut Check) {
    let sample = build_synthetic_sample(false);
    let sections = sample.sections(LittleEndian);

    let queries = vec![
        gimli::SegmentedQuery {
            selector: 7,
            offset: 0x204,
        },
        gimli::SegmentedQuery {
            selector: 9,
            offset: 0x304,
        },
        gimli::SegmentedQuery {
            selector: 7,
            offset: 0x210,
        }, // sequence right boundary
        gimli::SegmentedQuery {
            selector: 0,
            offset: 0x604,
        }, // function without rows
        gimli::SegmentedQuery {
            selector: 0,
            offset: 0x704,
        }, // rows without a function
        gimli::SegmentedQuery {
            selector: 99,
            offset: 0x200,
        }, // unknown segment
    ];
    let results = gimli::segmented_lookup(&sections, &queries, &sample.banks).unwrap();

    // Query 0: inherited selector 7, alpha_step, line 12.
    let r0 = &results[0];
    check.equal(r0.function.as_deref(), Some("alpha_step"));
    check.equal(r0.die, Some(sample.alpha_die));
    check.equal(r0.segment_die, Some(sample.root1_die));
    check.equal(r0.file.as_deref(), Some("firmware/alpha.c"));
    check.equal(r0.line, Some(12));
    check.equal(r0.aranges, Some(sample.cu1_aranges_offset));
    check.equal(r0.bank.as_deref(), Some("bank-7.bin"));

    // Query 1: overriding selector 9, beta_reset, line 27.
    let r1 = &results[1];
    check.equal(r1.function.as_deref(), Some("beta_reset"));
    check.equal(r1.die, Some(sample.beta_die));
    check.equal(r1.segment_die, Some(sample.beta_die));
    check.equal(r1.file.as_deref(), Some("firmware/beta.c"));
    check.equal(r1.line, Some(27));

    // Query 2: the half-open right boundary is excluded everywhere.
    let r2 = &results[2];
    check.require(r2.cu.is_none(), "half-open end excludes boundary address");
    check.require(r2.function.is_none(), "no function past the range end");
    check.require(r2.file.is_none(), "end_sequence row is not reused");
    check.equal(r2.bank.as_deref(), Some("bank-7.bin"));

    // Query 3: function exists but no source row covers the address.
    let r3 = &results[3];
    check.equal(r3.function.as_deref(), Some("silent_work"));
    check.equal(r3.die, Some(sample.silent_die));
    check.require(r3.file.is_none(), "function without rows leaves file unknown");
    check.require(r3.line.is_none(), "function without rows leaves line unknown");
    check.equal(r3.cu, Some(sample.cu2_offset));

    // Query 4: CU and source row exist, but no subprogram covers the address.
    let r4 = &results[4];
    check.require(r4.function.is_none(), "CU may have rows without a function");
    check.require(r4.die.is_none(), "no DIE is invented for functionless code");
    check.equal(r4.cu, Some(sample.cu2_offset));
    check.equal(r4.file.as_deref(), Some("firmware/orphan.c"));
    check.equal(r4.line, Some(9));

    // Query 5: unknown segment keeps only the bank file location.
    let r5 = &results[5];
    check.require(r5.cu.is_none(), "unknown segment has no CU evidence");
    check.require(r5.aranges.is_none(), "unknown segment has no arange tuple");
    check.equal(r5.bank.as_deref(), Some("bank-99.bin"));
    check.equal(r5.bank_offset, Some(0x200));

    // Byte-level replay of arange evidence, independent of the analyzer.
    let first_header = DebugAranges::new(&sample.aranges, LittleEndian)
        .headers()
        .next()
        .unwrap()
        .unwrap();
    let tuple0 = first_header.entries().next().unwrap().unwrap();
    let tuple_offset = tuple0.tuple_offset() as usize;
    check.equal(sample.aranges[tuple_offset], 0x00);
    check.equal(sample.aranges[tuple_offset + 1], 0x05);
    check.equal(sample.aranges[tuple_offset + 4], 0x08);

    // Replay the line evidence triple for query 0 from the raw program.
    let debug_line = DebugLine::new(&sample.line, LittleEndian);
    let program = debug_line
        .program(DebugLineOffset(r0.line_program.unwrap() as usize), 4, None, None)
        .unwrap();
    let (complete, sequences) = program.sequences().unwrap();
    let sequence = &sequences[r0.sequence.unwrap() as usize];
    let mut rows = complete.resume_from(sequence);
    let mut replayed = Vec::new();
    while let Some((header, row)) = rows.next_row().unwrap() {
        let file = row
            .file(header)
            .and_then(|entry| match entry.path_name() {
                gimli::AttributeValue::String(reader) => reader.to_string().ok().map(String::from),
                _ => None,
            })
            .unwrap_or_default();
        replayed.push((row.segment(), row.address(), row.line().map(|v| v.get()), file));
    }
    let target_row = replayed[r0.row.unwrap() as usize].clone();
    check.equal(target_row.0, 7);
    check.equal(target_row.1, 0x200);
    check.equal(target_row.2, Some(12));
    check.equal(target_row.3, String::from("alpha.c"));

    // The provider DIE's exprloc bytes really exist in .debug_info.
    let mut found = false;
    for window in sample.info.windows(3) {
        if window == [2, 0x10, 7] {
            found = true;
        }
    }
    check.require(found, "segment exprloc bytes exist in debug_info");

    // The restricted expression rule via the public Operation parser.
    let expr = EndianSlice::new(&[0x10, 0x07, 0x30][..], LittleEndian);
    let mut ops = Expression(expr).operations(gimli::Encoding {
        address_size: 4,
        format: gimli::Format::Dwarf32,
        version: 5,
    });
    let first = ops.next().unwrap().unwrap();
    check.require(
        matches!(first, Operation::UnsignedConstant { value: 7 }),
        "single DW_OP_constu decodes",
    );
    check.require(
        ops.next().unwrap().is_some(),
        "the extra DW_OP_lit0 is what makes the analyzer reject the exprloc",
    );
}

/// Locate the fixed fixture without relying on cargo environment variables.
fn fixture_dir() -> std::path::PathBuf {
    if let Ok(override_dir) = std::env::var("UFB_FIXTURE_DIR") {
        return std::path::PathBuf::from(override_dir);
    }
    let mut current = std::env::current_dir().unwrap();
    loop {
        let candidate = current.join("fixtures/banked-fw");
        if candidate.is_dir() {
            return candidate;
        }
        if !current.pop() {
            panic!("cannot locate fixtures/banked-fw from the current directory");
        }
    }
}

fn bank_attribution_fixture(check: &mut Check) {
    let base = fixture_dir();
    let read = |name: &str| std::fs::read(base.join(name)).unwrap();
    let debug_aranges_bytes = read("debug_aranges");
    let debug_info_bytes = read("debug_info");
    let debug_abbrev_bytes = read("debug_abbrev");
    let debug_line_bytes = read("debug_line");
    let banks_text = std::fs::read_to_string(base.join("banks.tsv")).unwrap();
    let queries_text = std::fs::read_to_string(base.join("queries.tsv")).unwrap();

    let banks = gimli::parse_bank_manifests(&banks_text).unwrap();
    let queries = gimli::parse_queries(&queries_text).unwrap();
    let sections = gimli::SegmentedSections {
        endian: LittleEndian,
        debug_aranges: &debug_aranges_bytes,
        debug_info: &debug_info_bytes,
        debug_abbrev: &debug_abbrev_bytes,
        debug_line: &debug_line_bytes,
    };
    let results = gimli::segmented_lookup(&sections, &queries, &banks).unwrap();

    // Independent expectations written from the manual byte decode.
    let expected: HashMap<usize, (&str, &str, u64, &str)> = HashMap::from([
        (0, ("sample_read", "firmware/sensor.c", 4, "bank-0021.bin")),
        (1, ("bank_service", "firmware/dispatch.c", 8, "bank-0034.bin")),
        (5, ("control_tick", "firmware/control.c", 4, "bank-0034.bin")),
    ]);
    let by_query: HashMap<usize, &gimli::SegmentedLookupResult> =
        results.iter().map(|result| (result.query, result)).collect();

    for (query_index, (function, file, line, bank)) in expected {
        let result = by_query[&query_index];
        check.equal(result.function.as_deref(), Some(function));
        check.equal(result.file.as_deref(), Some(file));
        check.equal(result.line, Some(line));
        check.equal(result.bank.as_deref(), Some(bank));
        check.require(result.segment_die.is_some(), "fixed functions name a segment DIE");
    }

    // Queries with only partial information keep their CU and bank evidence.
    for index in [2usize, 3, 7, 9] {
        let result = by_query[&index];
        check.require(result.cu.is_some(), "CU evidence survives partial attribution");
        check.require(result.bank.is_some(), "bank location is independently available");
    }
    // Query 9 is the legal offset-zero range.
    check.equal(by_query[&9].tuple, Some(0x24));
    check.equal(by_query[&9].bank_offset, Some(0));
    // Unmatched queries invent no debug evidence.
    for index in [10usize, 11, 4, 6, 8] {
        let result = by_query[&index];
        check.require(result.cu.is_none(), "no CU for unmatched query");
        check.require(result.aranges.is_none(), "no tuple for unmatched query");
    }
    check.require(
        by_query[&10].bank.is_none(),
        "selector 0x55 has no bank mapping",
    );

    // Raw byte evidence: the tuple reported for query 0 encodes 0x21/0x1000.
    let tuple_offset = by_query[&0].tuple.unwrap() as usize;
    check.equal(debug_aranges_bytes[tuple_offset], 0x21);
    check.equal(
        debug_aranges_bytes[tuple_offset + 1..tuple_offset + 5].to_vec(),
        vec![0x00u8, 0x10, 0x00, 0x00],
    );

    // Replay the line triple through the real reader and hit the reported row.
    let result0 = by_query[&0];
    let debug_line = DebugLine::new(&debug_line_bytes, LittleEndian);
    let program = debug_line
        .program(
            DebugLineOffset(result0.line_program.unwrap() as usize),
            4,
            None,
            None,
        )
        .unwrap();
    let (complete, sequences) = program.sequences().unwrap();
    let sequence = &sequences[result0.sequence.unwrap() as usize];
    let mut rows = complete.resume_from(sequence);
    let mut count = 0u64;
    let mut hit = None;
    while let Some((header, row)) = rows.next_row().unwrap() {
        if count == result0.row.unwrap() {
            let file = row
                .file(header)
                .and_then(|entry| match entry.path_name() {
                    gimli::AttributeValue::String(reader) => {
                        reader.to_string().ok().map(String::from)
                    }
                    _ => None,
                })
                .unwrap_or_default();
            hit = Some((row.address(), row.segment(), row.line().unwrap().get(), file));
        }
        count += 1;
    }
    check.equal(hit, Some((0x1004, 0x21, 4, String::from("sensor.c"))));

    // The reported segment DIE resolves through the real DIE reader.
    let debug_abbrev = DebugAbbrev::new(&debug_abbrev_bytes, LittleEndian);
    let debug_info = DebugInfo::new(&debug_info_bytes, LittleEndian);
    let header = debug_info.units().next().unwrap().unwrap();
    let abbreviations = header.abbreviations(&debug_abbrev).unwrap();
    let segment_die = result0.segment_die.unwrap();
    let unit_offset = gimli::UnitOffset(segment_die as usize - header.offset().0);
    let die = header.entry(&abbreviations, unit_offset).unwrap();
    check.equal(die.tag(), gimli::DW_TAG_compile_unit);
    let segment_attr = die.attr(gimli::DW_AT_segment).unwrap();
    let Expression(bytes) = segment_attr.exprloc_value().unwrap();
    check.equal(bytes.slice(), &[0x10, 0x21]);

    // Bank offsets stay inside the referenced files.
    for result in &results {
        if let (Some(bank), Some(offset)) = (&result.bank, result.bank_offset) {
            let data = read(bank);
            check.require((offset as usize) < data.len(), "bank offset is in bounds");
        }
    }

    // The real Dwarf loader parses every unit in the section.
    let loader = |id: SectionId| -> Result<EndianSlice<'_, gimli::LittleEndian>, ()> {
        let empty: &[u8] = &[];
        let bytes = match id {
            SectionId::DebugAranges => &debug_aranges_bytes,
            SectionId::DebugInfo => &debug_info_bytes,
            SectionId::DebugAbbrev => &debug_abbrev_bytes,
            SectionId::DebugLine => &debug_line_bytes,
            _ => empty,
        };
        Ok(EndianSlice::new(bytes, LittleEndian))
    };
    let dwarf = Dwarf::load(loader).unwrap();
    let unit_count = dwarf.debug_info.units().count();
    check.require(unit_count >= 2, "real Dwarf loader parses both fixture CUs");
}
