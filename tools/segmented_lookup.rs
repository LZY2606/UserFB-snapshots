//! Thin command line driver for [`gimli::segmented_lookup`].
//!
//! Usage: `lookup <sample-directory> <query-tsv>`. The sample directory must
//! contain the four raw DWARF sections and a `banks.tsv` manifest; nothing
//! about the concrete fixture names, offsets or expected answers is hardcoded.
//! The analysis itself lives in the gimli read crate and is shared with the
//! contract tests.

use std::path::Path;
use std::process::ExitCode;

use gimli::{
    parse_bank_manifests, parse_queries, segmented_lookup, BankMapping, LittleEndian,
    SegmentedLookupResult, SegmentedQuery, SegmentedSections,
};

const HEADER: &str = "query\tselector\toffset\tcu\tcu_name\tfunction\tdie\tsegment_die\tfile\tline\taranges\ttuple\tline_program\tsequence\trow\tbank\tbank_offset";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: {} <sample-directory> <query-tsv>", args[0]);
        return ExitCode::from(2);
    }
    let sample_dir = Path::new(&args[1]);
    let query_path = Path::new(&args[2]);

    if let Err(error) = run(sample_dir, query_path) {
        eprintln!("segmented_lookup: {error}");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}

fn run(sample_dir: &Path, query_path: &Path) -> std::io::Result<()> {
    let read_section = |name: &str| std::fs::read(sample_dir.join(name));
    let debug_aranges = read_section("debug_aranges")?;
    let debug_info = read_section("debug_info")?;
    let debug_abbrev = read_section("debug_abbrev")?;
    let debug_line = read_section("debug_line")?;
    let bank_text = std::fs::read_to_string(sample_dir.join("banks.tsv"))?;
    let query_text = std::fs::read_to_string(query_path)?;

    let banks: Vec<BankMapping> =
        parse_bank_manifests(&bank_text).map_err(|error| std::io::Error::other(format!("{error:?}")))?;
    let queries: Vec<SegmentedQuery> =
        parse_queries(&query_text).map_err(|error| std::io::Error::other(format!("{error:?}")))?;

    let sections = SegmentedSections {
        endian: LittleEndian,
        debug_aranges: &debug_aranges,
        debug_info: &debug_info,
        debug_abbrev: &debug_abbrev,
        debug_line: &debug_line,
    };
    let results = segmented_lookup(&sections, &queries, &banks)
        .map_err(|error| std::io::Error::other(format!("{error:?}")))?;

    let mut stdout = String::new();
    stdout.push_str(HEADER);
    stdout.push('\n');
    for result in &results {
        stdout.push_str(&render_row(result));
        stdout.push('\n');
    }
    print!("{stdout}");
    Ok(())
}

fn hex(value: u64) -> String {
    format!("0x{value:x}")
}

fn maybe_hex(value: Option<u64>) -> String {
    value.map_or_else(|| String::from("-"), hex)
}

fn maybe_decimal(value: Option<u64>) -> String {
    value.map_or_else(|| String::from("-"), |value| value.to_string())
}

fn maybe_text(value: &Option<String>) -> &str {
    value.as_deref().unwrap_or("-")
}

fn render_row(result: &SegmentedLookupResult) -> String {
    [
        result.query.to_string(),
        hex(result.selector),
        hex(result.offset),
        maybe_hex(result.cu),
        maybe_text(&result.cu_name).to_string(),
        maybe_text(&result.function).to_string(),
        maybe_hex(result.die),
        maybe_hex(result.segment_die),
        maybe_text(&result.file).to_string(),
        maybe_decimal(result.line),
        maybe_hex(result.aranges),
        maybe_hex(result.tuple),
        maybe_hex(result.line_program),
        maybe_decimal(result.sequence),
        maybe_decimal(result.row),
        maybe_text(&result.bank).to_string(),
        maybe_hex(result.bank_offset),
    ]
    .join("\t")
}
