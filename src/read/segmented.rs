//! Segmented (banked) address lookup on top of the regular read path.
//!
//! The analysis in this module never invents a parser of its own: it drives the
//! real [`DebugAranges`], `.debug_info` DIE and [`DebugLine`] readers. A
//! segmented address is always represented as an independent `(selector,
//! offset)` pair. The selector identifies a bank while the offset stays
//! segment-relative; the two are never combined into a host pointer or a
//! physical address.
//!
//! The evidence attached to every result consists of section-absolute byte
//! offsets (plus sequence/row indices for line programs), so each conclusion
//! can be replayed independently against the raw section bytes.

use alloc::string::String;
use alloc::vec::Vec;

use crate::common::{DebugLineOffset, Encoding};
use crate::constants::{
    DW_AT_high_pc, DW_AT_low_pc, DW_AT_name, DW_AT_segment, DW_AT_stmt_list, DW_TAG_subprogram,
    DwTag,
};
use crate::endianity::Endianity;
use crate::read::{
    Attribute, AttributeValue, DebugAranges, DebugLine, Dwarf, EndianSlice, Error, Expression,
    LineProgramHeader, LineRow, Operation, Reader, ReaderOffset, Result, SectionId,
};

/// The raw, uncompressed DWARF sections needed for a segmented lookup.
///
/// All slices are kept as borrowed byte buffers; endianity is explicit so the
/// same driver handles either byte order.
#[derive(Clone, Copy, Debug)]
pub struct SegmentedSections<'a, Endian>
where
    Endian: Endianity,
{
    /// The byte order used by every section.
    pub endian: Endian,
    /// The raw `.debug_aranges` section.
    pub debug_aranges: &'a [u8],
    /// The raw `.debug_info` section.
    pub debug_info: &'a [u8],
    /// The raw `.debug_abbrev` section.
    pub debug_abbrev: &'a [u8],
    /// The raw `.debug_line` section.
    pub debug_line: &'a [u8],
}

/// A single segmented query: a selector and a segment-relative offset.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SegmentedQuery {
    /// The segment (bank) selector.
    pub selector: u64,
    /// The offset inside the segment.
    pub offset: u64,
}

/// The mapping of one selector to a raw bank image, as described by a
/// `banks.tsv` style manifest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BankMapping {
    /// The selector this mapping applies to.
    pub selector: u64,
    /// The bank file name (a single path component).
    pub file: String,
    /// The segment-relative base address of the mapped file region.
    pub base: u64,
    /// The mapped length in bytes.
    pub length: u64,
}

/// The effective segment identity of a DIE.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SegmentIdentity {
    /// An explicit empty expression selected the flat address space.
    Flat,
    /// A `DW_OP_constu` expression selected this segment.
    Selector(u64),
}

impl SegmentIdentity {
    fn selector(self) -> u64 {
        match self {
            SegmentIdentity::Flat => 0,
            SegmentIdentity::Selector(selector) => selector,
        }
    }
}

/// The segment attribute carried directly by one DIE.
#[derive(Clone, Copy, Debug)]
struct OwnedSegment {
    /// The effective identity.
    identity: SegmentIdentity,
    /// The absolute `.debug_info` offset of the DIE providing the attribute.
    provider: u64,
}

/// The information retained for one DIE while walking a compilation unit.
struct DieInfo {
    /// Absolute `.debug_info` offset.
    offset: u64,
    /// DFS depth relative to the root DIE.
    depth: isize,
    tag: DwTag,
    /// A directly present `DW_AT_segment`, if any.
    segment: Option<OwnedSegment>,
    name: Option<String>,
    low_pc: Option<u64>,
    high_pc: Option<u64>,
    /// Whether `DW_AT_high_pc` is a length constant rather than an address.
    high_pc_is_length: bool,
    /// `DW_AT_stmt_list` as a `.debug_line` offset, on the root DIE.
    stmt_list: Option<u64>,
}

/// One parsed arange tuple together with its byte-level evidence.
struct ArangeTuple {
    /// The `.debug_info` offset of the associated compilation unit.
    cu_info_offset: u64,
    /// The start of the aranges contribution in `.debug_aranges`.
    contribution: u64,
    /// The section-absolute offset of this tuple.
    tuple: u64,
    selector: u64,
    begin: u64,
    end: u64,
}

/// One subprogram with its effective segment identity.
struct FunctionInfo {
    name: String,
    die: u64,
    /// DIE that actually supplied the effective segment attribute.
    segment_provider: Option<u64>,
    selector: u64,
    begin: u64,
    end: u64,
}

/// One half-open source row coverage interval inside a line sequence.
#[derive(Clone)]
struct RowCoverage {
    begin: u64,
    end: u64,
    row: u64,
    file: Option<String>,
    line: Option<u64>,
}

/// The parsed line program evidence for one compilation unit and sequence.
struct LineEvidence {
    /// The CU header's `.debug_info` offset.
    cu_offset: u64,
    /// Section-absolute offset of the line program header.
    program: u64,
    /// 0-based sequence index within the contribution.
    sequence: u64,
    selector: u64,
    rows: Vec<RowCoverage>,
}

/// The parsed contents of one compilation unit.
struct UnitInfo {
    /// Absolute `.debug_info` offset of the CU header.
    cu_offset: u64,
    name: String,
    address_size: u8,
    functions: Vec<FunctionInfo>,
    stmt_list: Option<u64>,
}

/// A single fully attributed lookup result.
///
/// Optional fields are `None` when the corresponding evidence does not exist;
/// they render as `-` in the TSV report.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SegmentedLookupResult {
    /// 0-based index of the query that produced this row.
    pub query: usize,
    /// The queried selector.
    pub selector: u64,
    /// The queried segment-relative offset.
    pub offset: u64,
    /// Absolute `.debug_info` offset of the CU header.
    pub cu: Option<u64>,
    /// The CU name.
    pub cu_name: Option<String>,
    /// The subprogram name.
    pub function: Option<String>,
    /// Absolute `.debug_info` offset of the subprogram DIE.
    pub die: Option<u64>,
    /// Absolute `.debug_info` offset of the DIE providing the segment attribute.
    pub segment_die: Option<u64>,
    /// The source file path.
    pub file: Option<String>,
    /// The source line number.
    pub line: Option<u64>,
    /// Start of the matching aranges contribution.
    pub aranges: Option<u64>,
    /// Absolute offset of the matching arange tuple.
    pub tuple: Option<u64>,
    /// Section-absolute offset of the line program header.
    pub line_program: Option<u64>,
    /// 0-based sequence index.
    pub sequence: Option<u64>,
    /// 0-based row index within the sequence.
    pub row: Option<u64>,
    /// The raw bank file name.
    pub bank: Option<String>,
    /// The byte offset within the bank file.
    pub bank_offset: Option<u64>,
}

/// Parse a `banks.tsv` style manifest into [`BankMapping`] entries.
///
/// The manifest only locates raw bytes; it is never used to infer functions or
/// source attribution.
pub fn parse_bank_manifests(text: &str) -> Result<Vec<BankMapping>> {
    let mut mappings = Vec::new();
    for (index, line) in text.lines().enumerate() {
        if line.is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split('\t').collect();
        if index == 0 && fields.first().is_some_and(|first| *first == "selector") {
            continue;
        }
        if fields.len() != 4 {
            return Err(Error::UnsupportedEvaluation);
        }
        let selector = parse_number(fields[0]).ok_or(Error::UnsupportedEvaluation)?;
        let file = fields[1];
        if file.is_empty() || file.contains('/') {
            return Err(Error::UnsupportedEvaluation);
        }
        let base = parse_number(fields[2]).ok_or(Error::UnsupportedEvaluation)?;
        let length = parse_number(fields[3]).ok_or(Error::UnsupportedEvaluation)?;
        mappings.push(BankMapping {
            selector,
            file: String::from(file),
            base,
            length,
        });
    }
    Ok(mappings)
}

/// Parse a query TSV with a `selector\toffset` header into typed queries.
pub fn parse_queries(text: &str) -> Result<Vec<SegmentedQuery>> {
    let mut queries = Vec::new();
    for (index, line) in text.lines().enumerate() {
        if line.is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split('\t').collect();
        if index == 0 && fields.first().is_some_and(|first| *first == "selector") {
            continue;
        }
        if fields.len() != 2 {
            return Err(Error::UnsupportedEvaluation);
        }
        let selector = parse_number(fields[0]).ok_or(Error::UnsupportedEvaluation)?;
        let offset = parse_number(fields[1]).ok_or(Error::UnsupportedEvaluation)?;
        queries.push(SegmentedQuery { selector, offset });
    }
    Ok(queries)
}

fn parse_number(text: &str) -> Option<u64> {
    if let Some(hex) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).ok()
    } else {
        text.parse::<u64>().ok()
    }
}

/// Run the full segmented analysis for every query, preserving input order.
pub fn segmented_lookup<Endian>(
    sections: &SegmentedSections<'_, Endian>,
    queries: &[SegmentedQuery],
    banks: &[BankMapping],
) -> Result<Vec<SegmentedLookupResult>>
where
    Endian: Endianity,
{
    let aranges = parse_aranges(sections)?;
    let units = parse_units(sections)?;
    let lines = parse_lines(sections, &units)?;

    let mut results = Vec::new();
    for (index, query) in queries.iter().enumerate() {
        let bank = banks
            .iter()
            .find(|mapping| {
                mapping.selector == query.selector
                    && query.offset >= mapping.base
                    && query.offset < mapping.base.saturating_add(mapping.length)
            })
            .map(|mapping| {
                (
                    mapping.file.clone(),
                    query.offset.saturating_sub(mapping.base),
                )
            });

        // All arange tuples covering this exact (selector, offset) pair.
        let matching_aranges: Vec<&ArangeTuple> = aranges
            .iter()
            .filter(|tuple| {
                tuple.selector == query.selector
                    && query.offset >= tuple.begin
                    && query.offset < tuple.end
            })
            .collect();

        if matching_aranges.is_empty() {
            results.push(SegmentedLookupResult {
                query: index,
                selector: query.selector,
                offset: query.offset,
                cu: None,
                cu_name: None,
                function: None,
                die: None,
                segment_die: None,
                file: None,
                line: None,
                aranges: None,
                tuple: None,
                line_program: None,
                sequence: None,
                row: None,
                bank: bank.clone().map(|(file, _)| file),
                bank_offset: bank.map(|(_, offset)| offset),
            });
            continue;
        }

        // Identical CU/function/sequence/row identities are emitted only once.
        let mut seen: Vec<(u64, Option<u64>, Option<u64>, Option<u64>)> = Vec::new();
        for candidate in &matching_aranges {
            let Some(unit) = units
                .iter()
                .find(|unit| unit.cu_offset == candidate.cu_info_offset)
            else {
                // The aranges referenced a CU absent from .debug_info. Record
                // the CU-level evidence without inventing DIE data.
                let key = (candidate.cu_info_offset, None, None, None);
                if !seen.contains(&key) {
                    seen.push(key);
                    results.push(SegmentedLookupResult {
                        query: index,
                        selector: query.selector,
                        offset: query.offset,
                        cu: Some(candidate.cu_info_offset),
                        cu_name: None,
                        function: None,
                        die: None,
                        segment_die: None,
                        file: None,
                        line: None,
                        aranges: Some(candidate.contribution),
                        tuple: Some(candidate.tuple),
                        line_program: None,
                        sequence: None,
                        row: None,
                        bank: bank.clone().map(|(file, _)| file),
                        bank_offset: bank.clone().map(|(_, offset)| offset),
                    });
                }
                continue;
            };

            let functions: Vec<&FunctionInfo> = unit
                .functions
                .iter()
                .filter(|function| {
                    function.selector == query.selector
                        && query.offset >= function.begin
                        && query.offset < function.end
                })
                .collect();

            let unit_lines: Vec<&LineEvidence> = lines
                .iter()
                .filter(|evidence| evidence.cu_offset == unit.cu_offset)
                .collect();

            if functions.is_empty() {
                // No subprogram covers the address, but CU-level rows may.
                let mut emitted_line = false;
                for evidence in &unit_lines {
                    if evidence.selector != query.selector {
                        continue;
                    }
                    for coverage in &evidence.rows {
                        if query.offset >= coverage.begin && query.offset < coverage.end {
                            emitted_line = true;
                            push_result(
                                &mut results,
                                &mut seen,
                                index,
                                query,
                                unit,
                                None,
                                candidate,
                                Some(evidence),
                                Some(coverage),
                                &bank,
                            );
                        }
                    }
                }
                if !emitted_line {
                    push_result(
                        &mut results,
                        &mut seen,
                        index,
                        query,
                        unit,
                        None,
                        candidate,
                        None,
                        None,
                        &bank,
                    );
                }
            } else {
                for function in &functions {
                    let mut emitted_line = false;
                    for evidence in &unit_lines {
                        if evidence.selector != query.selector {
                            continue;
                        }
                        for coverage in &evidence.rows {
                            if query.offset >= coverage.begin && query.offset < coverage.end {
                                emitted_line = true;
                                push_result(
                                    &mut results,
                                    &mut seen,
                                    index,
                                    query,
                                    unit,
                                    Some(function),
                                    candidate,
                                    Some(evidence),
                                    Some(coverage),
                                    &bank,
                                );
                            }
                        }
                    }
                    if !emitted_line {
                        // A function without a source row covering the address.
                        push_result(
                            &mut results,
                            &mut seen,
                            index,
                            query,
                            unit,
                            Some(function),
                            candidate,
                            None,
                            None,
                            &bank,
                        );
                    }
                }
            }
        }
    }

    // Sort by query, then CU, DIE, sequence, row; missing values sort last.
    results.sort_by_key(|result| {
        (
            result.query,
            result.cu.unwrap_or(u64::MAX),
            result.die.unwrap_or(u64::MAX),
            result.sequence.unwrap_or(u64::MAX),
            result.row.unwrap_or(u64::MAX),
        )
    });
    Ok(results)
}

#[allow(clippy::too_many_arguments)]
fn push_result(
    results: &mut Vec<SegmentedLookupResult>,
    seen: &mut Vec<(u64, Option<u64>, Option<u64>, Option<u64>)>,
    query_index: usize,
    query: &SegmentedQuery,
    unit: &UnitInfo,
    function: Option<&FunctionInfo>,
    arange: &ArangeTuple,
    line: Option<&LineEvidence>,
    coverage: Option<&RowCoverage>,
    bank: &Option<(String, u64)>,
) {
    let die = function.map(|info| info.die);
    let sequence = line.map(|evidence| evidence.sequence);
    let row = coverage.map(|info| info.row);
    let key = (unit.cu_offset, die, sequence, row);
    if seen.contains(&key) {
        return;
    }
    seen.push(key);
    results.push(SegmentedLookupResult {
        query: query_index,
        selector: query.selector,
        offset: query.offset,
        cu: Some(unit.cu_offset),
        cu_name: Some(unit.name.clone()),
        function: function.map(|info| info.name.clone()),
        die,
        segment_die: function.and_then(|info| info.segment_provider),
        file: coverage.and_then(|info| info.file.clone()),
        line: coverage.and_then(|info| info.line),
        aranges: Some(arange.contribution),
        tuple: Some(arange.tuple),
        line_program: line.map(|evidence| evidence.program),
        sequence,
        row,
        bank: bank.clone().map(|(file, _)| file),
        bank_offset: bank.clone().map(|(_, offset)| offset),
    });
}

/// Drive the real `DebugAranges` reader and retain byte offsets.
fn parse_aranges<Endian>(sections: &SegmentedSections<'_, Endian>) -> Result<Vec<ArangeTuple>>
where
    Endian: Endianity,
{
    let debug_aranges = DebugAranges::new(sections.debug_aranges, sections.endian);
    let mut headers = debug_aranges.headers();
    let mut tuples = Vec::new();
    while let Some(header) = headers.next()? {
        let contribution = header.offset().0.into_u64();
        let cu_info_offset = header.debug_info_offset().0.into_u64();
        let mut entries = header.entries();
        while let Some(raw) = entries.next_raw()? {
            let tuple_offset = raw.tuple_offset();
            if let Some(entry) = entries.convert_raw(raw)? {
                tuples.push(ArangeTuple {
                    cu_info_offset,
                    contribution,
                    tuple: tuple_offset,
                    selector: entry.segment(),
                    begin: entry.range().begin,
                    end: entry.range().end,
                });
            }
        }
    }
    Ok(tuples)
}

/// Walk `.debug_info` through the real unit/DIE readers, resolving inherited
/// segment attributes and half-open subprogram ranges.
fn parse_units<Endian>(sections: &SegmentedSections<'_, Endian>) -> Result<Vec<UnitInfo>>
where
    Endian: Endianity,
{
    let loader = |id: SectionId| -> core::result::Result<EndianSlice<'_, Endian>, ()> {
        let bytes = match id {
            SectionId::DebugAranges => sections.debug_aranges,
            SectionId::DebugInfo => sections.debug_info,
            SectionId::DebugAbbrev => sections.debug_abbrev,
            SectionId::DebugLine => sections.debug_line,
            _ => &[],
        };
        Ok(EndianSlice::new(bytes, sections.endian))
    };
    let dwarf = Dwarf::load(loader).map_err(|()| Error::UnsupportedEvaluation)?;

    let mut unit_headers = Vec::new();
    let mut headers = dwarf.debug_info.units();
    while let Some(header) = headers.next()? {
        unit_headers.push(header);
    }

    let mut units = Vec::new();
    for header in unit_headers {
        let unit = dwarf.unit(header)?;
        let cu_offset = unit
            .debug_info_offset()
            .ok_or(Error::UnsupportedEvaluation)?
            .0
            .into_u64();
        let address_size = unit.address_size();

        let mut dies: Vec<DieInfo> = Vec::new();
        let mut cursor = unit.entries();
        while cursor.next_dfs()?.is_some() {
            let Some(entry) = cursor.current() else {
                break;
            };
            let offset = entry
                .offset()
                .to_debug_info_offset(&unit.header)
                .ok_or(Error::UnsupportedEvaluation)?
                .0
                .into_u64();

            let segment = match entry.attr(DW_AT_segment) {
                Some(attr) => Some(resolve_segment(attr, offset, unit.encoding())?),
                None => None,
            };
            let name = entry
                .attr(DW_AT_name)
                .and_then(|attr| attr.string_value(&dwarf.debug_str))
                .and_then(|reader| reader.to_string().ok())
                .map(String::from);
            let low_pc = match entry.attr(DW_AT_low_pc).map(|attr| attr.value()) {
                Some(AttributeValue::Addr(address)) => Some(address),
                _ => None,
            };
            let (high_pc, high_pc_is_length) = match entry
                .attr(DW_AT_high_pc)
                .map(|attr| attr.value())
            {
                Some(AttributeValue::Addr(address)) => (Some(address), false),
                Some(other) => (other.udata_value(), true),
                None => (None, false),
            };
            let stmt_list = entry
                .attr(DW_AT_stmt_list)
                .and_then(|attr| attr.offset_value())
                .map(|offset| offset.into_u64());

            dies.push(DieInfo {
                offset,
                depth: entry.depth(),
                tag: entry.tag(),
                segment,
                name,
                low_pc,
                high_pc,
                high_pc_is_length,
                stmt_list,
            });
        }

        // Resolve segment inheritance while preserving DFS order.
        let mut stack: Vec<(isize, OwnedSegment)> = Vec::new();
        let mut functions = Vec::new();
        let mut cu_name = String::new();
        let mut stmt_list = None;
        for die in &dies {
            while stack
                .last()
                .is_some_and(|(depth, _)| *depth >= die.depth)
            {
                stack.pop();
            }
            if let Some(owned) = die.segment {
                stack.push((die.depth, owned));
            }
            let effective = stack.last().map(|(_, owned)| *owned);

            if die.depth == 0 {
                if let Some(name) = &die.name {
                    cu_name = name.clone();
                }
                stmt_list = die.stmt_list;
            }
            if die.tag == DW_TAG_subprogram {
                let (Some(name), Some(begin), Some(high_pc)) =
                    (&die.name, die.low_pc, die.high_pc)
                else {
                    continue;
                };
                let end = if die.high_pc_is_length {
                    begin.checked_add(high_pc).ok_or(Error::AddressOverflow)?
                } else {
                    high_pc
                };
                functions.push(FunctionInfo {
                    name: name.clone(),
                    die: die.offset,
                    segment_provider: effective.map(|owned| owned.provider),
                    selector: effective
                        .map(|owned| owned.identity.selector())
                        .unwrap_or(0),
                    begin,
                    end,
                });
            }
        }

        units.push(UnitInfo {
            cu_offset,
            name: cu_name,
            address_size,
            functions,
            stmt_list,
        });
    }
    Ok(units)
}

/// Restricted `DW_AT_segment` evaluation: one `DW_OP_constu`, or an explicit
/// empty expression for the flat space. Anything else is rejected loudly
/// rather than being silently treated as zero.
fn resolve_segment<R: Reader>(
    attr: &Attribute<R>,
    provider: u64,
    encoding: Encoding,
) -> Result<OwnedSegment> {
    let Expression(bytes) = attr
        .exprloc_value()
        .ok_or(Error::UnsupportedEvaluation)?;
    if bytes.is_empty() {
        return Ok(OwnedSegment {
            identity: SegmentIdentity::Flat,
            provider,
        });
    }
    let mut operations = Expression(bytes).operations(encoding);
    let first = operations.next()?.ok_or(Error::UnsupportedEvaluation)?;
    if operations.next()?.is_some() {
        return Err(Error::UnsupportedEvaluation);
    }
    match first {
        Operation::UnsignedConstant { value } => Ok(OwnedSegment {
            identity: SegmentIdentity::Selector(value),
            provider,
        }),
        _ => Err(Error::UnsupportedEvaluation),
    }
}

/// Parse each CU's line program through the real `DebugLine` reader and build
/// half-open row coverage intervals that never cross an end-of-sequence row.
fn parse_lines<Endian>(
    sections: &SegmentedSections<'_, Endian>,
    units: &[UnitInfo],
) -> Result<Vec<LineEvidence>>
where
    Endian: Endianity,
{
    let debug_line = DebugLine::new(sections.debug_line, sections.endian);
    let mut evidence = Vec::new();
    for unit in units {
        let Some(stmt_list) = unit.stmt_list else {
            continue;
        };
        let program = debug_line.program(
            DebugLineOffset(
                <EndianSlice<'_, Endian> as Reader>::Offset::from_u64(stmt_list)?,
            ),
            unit.address_size,
            None,
            None,
        )?;
        let program_offset = program.header().offset().0.into_u64();
        let (complete, sequences) = program.sequences()?;
        for (sequence_index, sequence) in sequences.iter().enumerate() {
            let mut rows = complete.resume_from(sequence);
            let mut coverages = Vec::new();
            let mut pending: Option<(u64, u64, Option<String>, Option<u64>)> = None;
            let mut row_index = 0u64;
            while let Some((header, row)) = rows.next_row()? {
                // The terminating row retains the sequence's current selector.
                if row.segment() != sequence.segment {
                    return Err(Error::UnsupportedEvaluation);
                }
                if row.end_sequence() {
                    if let Some((begin, idx, file, line)) = pending.take() {
                        coverages.push(RowCoverage {
                            begin,
                            end: row.address(),
                            row: idx,
                            file,
                            line,
                        });
                    }
                } else {
                    if let Some((begin, idx, file, line)) = pending.take() {
                        coverages.push(RowCoverage {
                            begin,
                            end: row.address(),
                            row: idx,
                            file,
                            line,
                        });
                    }
                    let (file, line) = row_file_and_line(header, row)?;
                    pending = Some((row.address(), row_index, file, line));
                }
                row_index += 1;
            }
            // A program missing its end_sequence closes the final row at its
            // own address, producing no coverage beyond it.
            if let Some((begin, idx, file, line)) = pending.take() {
                coverages.push(RowCoverage {
                    begin,
                    end: begin,
                    row: idx,
                    file,
                    line,
                });
            }
            evidence.push(LineEvidence {
                cu_offset: unit.cu_offset,
                program: program_offset,
                sequence: sequence_index as u64,
                selector: sequence.segment,
                rows: coverages,
            });
        }
    }
    Ok(evidence)
}

fn row_file_and_line<R: Reader>(
    header: &LineProgramHeader<R>,
    row: &LineRow,
) -> Result<(Option<String>, Option<u64>)> {
    // A line number of zero means the instruction cannot be attributed to a
    // source line; treat the whole source location as unknown rather than
    // emitting a file without a line.
    let Some(line) = row.line() else {
        return Ok((None, None));
    };
    let Some(file_entry) = row.file(header) else {
        return Ok((None, None));
    };
    let AttributeValue::String(name_reader) = file_entry.path_name() else {
        return Err(Error::ExpectedStringAttributeValue);
    };
    let name = name_reader.to_string_lossy()?;
    let path = match file_entry.directory(header) {
        Some(AttributeValue::String(dir_reader)) => {
            let dir = dir_reader.to_string_lossy()?;
            if dir.is_empty() {
                name.into_owned()
            } else {
                alloc::format!("{dir}/{name}")
            }
        }
        _ => name.into_owned(),
    };
    Ok((Some(path), Some(line.get())))
}
