//! Regression tests for malformed PE exception directories and unwind info.
//!
//! Every input here is a locally synthesized byte array: nothing is executed or loaded. The goal
//! is to pin down the error propagation chain from exception directory mapping over function
//! queries down to unwind operation enumeration, for both the low level `ExceptionData` entry and
//! the full `PE::parse_with_opts` entry.

use goblin::pe::PE;
use goblin::pe::data_directories::DataDirectory;
use goblin::pe::exception::{
    Arm64RuntimeFunction, ExceptionData, Register, RuntimeFunction, UnwindOperation,
};
use goblin::pe::header::COFF_MACHINE_ARM64;
use goblin::pe::header::COFF_MACHINE_X86_64;
use goblin::pe::options::{ParseMode, ParseOptions};
use goblin::pe::section_table::SectionTable;

const FILE_ALIGNMENT: u32 = 0x200;
const SECTION_VA: u32 = 0x1000;
const SECTION_RAW: usize = 0x200;
const OPT_HEADER_OFFSET: usize = 0x58;
const SECTION_TABLE_OFFSET: usize = 0x148;

/// A section that maps every RVA to the same file offset, so the low level `ExceptionData` entry
/// can be exercised without switching the global RVA resolution option off.
fn identity_section(byte_len: u32) -> SectionTable {
    SectionTable {
        name: *b".pdata\0\0",
        virtual_size: byte_len,
        virtual_address: 0,
        size_of_raw_data: byte_len,
        pointer_to_raw_data: 0,
        ..SectionTable::default()
    }
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

/// Builds a minimal but parseable PE image whose single `.pdata` section starts at file offset
/// 0x200 / RVA 0x1000 and contains `section_data`. The exception directory points at that section.
fn build_pe(
    machine: u16,
    exception_dir: DataDirectory,
    section_data: &[u8],
    declared_raw_size: u32,
) -> Vec<u8> {
    let mut image = vec![0u8; SECTION_RAW + section_data.len()];

    // DOS header: only the signature and e_lfanew are needed.
    image[0] = b'M';
    image[1] = b'Z';
    put_u32(&mut image, 0x3c, 0x40);

    // PE signature and COFF header.
    put_u32(&mut image, 0x40, 0x0000_4550);
    put_u16(&mut image, 0x44, machine);
    put_u16(&mut image, 0x46, 1); // one section
    put_u16(&mut image, 0x54, 240); // SizeOfOptionalHeader (PE32+)
    put_u16(&mut image, 0x56, 0x2002); // DLL / executable image

    // Optional header (PE32+).
    put_u16(&mut image, OPT_HEADER_OFFSET, 0x20b); // MAGIC_64
    image[OPT_HEADER_OFFSET + 2] = 14; // major linker version
    put_u32(&mut image, OPT_HEADER_OFFSET + 20, 0x1000); // base of code
    put_u32(&mut image, OPT_HEADER_OFFSET + 24, 0x4000_0000); // low 32 bits of image base
    put_u32(&mut image, OPT_HEADER_OFFSET + 28, 0x0000_0001); // high 32 bits of image base
    put_u32(&mut image, OPT_HEADER_OFFSET + 32, 0x1000); // section alignment
    put_u32(&mut image, OPT_HEADER_OFFSET + 36, FILE_ALIGNMENT); // file alignment
    put_u32(&mut image, OPT_HEADER_OFFSET + 56, 0x3000); // size of image
    put_u32(&mut image, OPT_HEADER_OFFSET + 60, SECTION_RAW as u32); // size of headers
    put_u16(&mut image, OPT_HEADER_OFFSET + 68, 3); // subsystem: console
    put_u32(&mut image, OPT_HEADER_OFFSET + 108, 16); // number of data directories

    // Data directory #3 is the exception table.
    let exception_dir_offset = OPT_HEADER_OFFSET + 112 + 3 * 8;
    put_u32(
        &mut image,
        exception_dir_offset,
        exception_dir.virtual_address,
    );
    put_u32(&mut image, exception_dir_offset + 4, exception_dir.size);

    // Section table (.pdata).
    let section = SECTION_TABLE_OFFSET;
    image[section..section + 6].copy_from_slice(b".pdata");
    put_u32(&mut image, section + 8, 0x1000); // virtual size
    put_u32(&mut image, section + 12, SECTION_VA); // virtual address
    put_u32(&mut image, section + 16, declared_raw_size); // size of raw data
    put_u32(&mut image, section + 20, SECTION_RAW as u32); // pointer to raw data
    put_u32(&mut image, section + 36, 0x4000_0040); // initialized, readable data

    image[SECTION_RAW..].copy_from_slice(section_data);
    image
}

/// Six x64 runtime functions, including legal indirect chains and both kinds of cycles.
fn x64_pdata() -> Vec<u8> {
    let entries = [
        // offset 0x00 / RVA 0x1000: direct UNWIND_INFO at RVA 0x1080
        RuntimeFunction {
            begin_address: 0x100,
            end_address: 0x200,
            unwind_info_address: 0x1080,
        },
        // offset 0x0c / RVA 0x100c: redirect to entry 0
        RuntimeFunction {
            begin_address: 0x300,
            end_address: 0x400,
            unwind_info_address: 0x1001,
        },
        // offset 0x18 / RVA 0x1018: redirect to entry 1, then entry 0 (multi-level chain)
        RuntimeFunction {
            begin_address: 0x500,
            end_address: 0x600,
            unwind_info_address: 0x100d,
        },
        // offset 0x24 / RVA 0x1024: redirects to itself
        RuntimeFunction {
            begin_address: 0x700,
            end_address: 0x800,
            unwind_info_address: 0x1025,
        },
        // offset 0x30 / RVA 0x1030: part of a two-record cycle
        RuntimeFunction {
            begin_address: 0x900,
            end_address: 0xa00,
            unwind_info_address: 0x103d,
        },
        // offset 0x3c / RVA 0x103c: points back at entry 4
        RuntimeFunction {
            begin_address: 0xb00,
            end_address: 0xc00,
            unwind_info_address: 0x1031,
        },
    ];

    let mut bytes = Vec::with_capacity(entries.len() * 12);
    for entry in entries {
        bytes.extend_from_slice(&entry.begin_address.to_le_bytes());
        bytes.extend_from_slice(&entry.end_address.to_le_bytes());
        bytes.extend_from_slice(&entry.unwind_info_address.to_le_bytes());
    }
    bytes
}

/// Legal UNWIND_INFO with three declared slots: a multi-slot ALLOC_LARGE followed by a push.
fn x64_unwind_info() -> Vec<u8> {
    vec![
        0x01, 0x04, 0x03,
        0x00, // version 1, prolog size 4, three code slots, no frame register
        0x04, 0x01, // UWOP_ALLOC_LARGE, info 0: size in the following u16 slot
        0x07, 0x00, // size = 7 * 8 = 56
        0x01, 0x70, // UWOP_PUSH_NONVOL RDI
        0x00, 0x00, // alignment padding (odd slot count)
    ]
}

#[test]
fn diagnostics_contract() {
    // ------------------------------------------------------------------
    // Low level entry: plain byte arrays and ExceptionData, via an identity-mapped section
    // ------------------------------------------------------------------

    // The section maps RVAs 0..128 straight onto file offsets 0..128 for the low level entry.
    let sections = [identity_section(128)];

    // Normal directory: half-open function ranges and lookups keep working.
    let normal = [0x10u8, 0, 0, 0, 0x20, 0, 0, 0, 0x30, 0, 0, 0];
    let dir = DataDirectory {
        virtual_address: 0,
        size: 12,
    };
    let ed = ExceptionData::parse_with_opts(
        &normal,
        dir,
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect("a complete 12-byte directory must parse");
    assert_eq!(ed.len(), 1);
    assert!(!ed.is_empty());
    assert_eq!(ed.functions().count(), 1);
    assert_eq!(
        ed.find_function(0x18)
            .expect("lookup inside the range must succeed")
            .expect("the range covers RVA 0x18")
            .begin_address,
        0x10
    );
    // The end address is exclusive.
    assert!(
        ed.find_function(0x20)
            .expect("boundary lookup must succeed")
            .is_none()
    );
    assert_eq!(
        ed.get_function(0)
            .expect("index 0 must be readable")
            .begin_address,
        0x10
    );

    // The last valid index works, the one past it reports its location instead of panicking.
    let boundary = ed.get_function(1).expect_err("index 1 is out of bounds");
    assert!(
        boundary.to_string().contains("out of bounds"),
        "unexpected error: {boundary}"
    );
    let overflow = ed
        .get_function(usize::MAX)
        .expect_err("huge indexes must fail on overflow");
    assert!(
        overflow.to_string().contains("overflow"),
        "unexpected error: {overflow}"
    );

    // Truncated directory: 12 declared bytes but only 4 present. Parsing must fail instead of
    // returning a view that panics on first use.
    let truncated = [0u8; 4];
    let truncation = ExceptionData::parse_with_opts(
        &truncated,
        dir,
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect_err("a directory that is not fully backed by the image must be rejected");
    assert!(
        truncation
            .to_string()
            .contains("not fully backed by the image"),
        "unexpected error: {truncation}"
    );

    // A size that is not a whole number of x64 entries is malformed.
    let messy = DataDirectory {
        virtual_address: 0,
        size: 10,
    };
    let messy_err = ExceptionData::parse_with_opts(
        &normal,
        messy,
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect_err("10 bytes cannot hold 12-byte entries");
    assert!(
        messy_err
            .to_string()
            .contains("invalid exception directory table size"),
        "unexpected error: {messy_err}"
    );

    // Empty directories are valid and every query is a safe no-op.
    let empty = DataDirectory {
        virtual_address: 0,
        size: 0,
    };
    let empty_ed = ExceptionData::parse_with_opts(
        &[],
        empty,
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect("an empty directory must parse");
    assert!(empty_ed.is_empty());
    assert_eq!(empty_ed.len(), 0);
    assert!(empty_ed.functions().next().is_none());
    assert!(
        empty_ed
            .find_function(0)
            .expect("lookup in an empty directory must succeed")
            .is_none()
    );
    assert!(empty_ed.get_function(0).is_err());

    // Bad extended unwind slot: one declared slot but ALLOC_LARGE needs more. The error is
    // reported once, then the fused iterator stays at None and size_hint follows Iterator rules.
    let bad_slot_info = goblin::pe::exception::UnwindInfo::parse(&[1, 4, 1, 0, 4, 1, 0, 0], 0)
        .expect("the unwind info header itself is well formed");
    let mut codes = bad_slot_info.unwind_codes();
    assert_eq!(codes.size_hint(), (0, Some(1)));
    let first = codes
        .next()
        .expect("an iterator item must be present")
        .expect_err("the extended slot read has to fail");
    assert!(
        first.to_string().contains("too big"),
        "unexpected error: {first}"
    );
    assert!(codes.next().is_none());
    assert!(codes.next().is_none(), "the error must not be repeated");
    assert_eq!(codes.size_hint(), (0, Some(0)));

    // Lazy iteration may yield valid operations before the single terminal error.
    let valid_then_bad =
        goblin::pe::exception::UnwindInfo::parse(&[1, 4, 2, 0, 1, 0x70, 4, 0x11], 0)
            .expect("two-slot unwind info must parse");
    let mut codes = valid_then_bad.unwind_codes();
    assert_eq!(codes.size_hint(), (0, Some(2)));
    let push = codes
        .next()
        .expect("first slot holds a complete push operation")
        .expect("the push operation is valid");
    assert_eq!(
        push.operation,
        UnwindOperation::PushNonVolatile(Register(7))
    );
    assert_eq!(codes.size_hint(), (0, Some(1)));
    codes
        .next()
        .expect("the broken ALLOC_LARGE still produces an item")
        .expect_err("ALLOC_LARGE info 1 needs two extension slots that do not exist");
    assert!(codes.next().is_none());
    assert_eq!(codes.size_hint(), (0, Some(0)));

    // Legal multi-slot layout: offsets and operations are unchanged by the boundary checks.
    let legal_bytes = x64_unwind_info();
    let legal = goblin::pe::exception::UnwindInfo::parse(&legal_bytes, 0)
        .expect("legal unwind info must parse");
    let mut codes = legal.unwind_codes();
    assert_eq!(codes.size_hint(), (1, Some(3)));
    let alloc = codes.next().expect("ALLOC_LARGE item").unwrap();
    assert_eq!(alloc.code_offset, 4);
    assert_eq!(alloc.operation, UnwindOperation::Alloc(56));
    // Two of the three slots are consumed; the hint must stay within the Iterator contract.
    assert_eq!(codes.size_hint(), (0, Some(1)));
    let push = codes.next().expect("push item").unwrap();
    assert_eq!(push.code_offset, 1);
    assert_eq!(
        push.operation,
        UnwindOperation::PushNonVolatile(Register(7))
    );
    assert!(codes.next().is_none());
    assert_eq!(codes.size_hint(), (0, Some(0)));

    // Natural exhaustion without an error also fuses at None with a zero-sized hint.
    let only_push = goblin::pe::exception::UnwindInfo::parse(&[1, 1, 1, 0, 1, 0x70, 0, 0], 0)
        .expect("single-slot unwind info must parse");
    let mut codes = only_push.unwind_codes();
    assert_eq!(codes.size_hint(), (0, Some(1)));
    assert!(codes.next().expect("one code").is_ok());
    assert!(codes.next().is_none());
    assert!(codes.next().is_none());
    assert_eq!(codes.size_hint(), (0, Some(0)));

    // ARM64 low level entry: 8-byte records, full bounds checks of its own.
    let arm64_bytes = [
        0x00u8, 0x10, 0, 0, 0x05, 0x10, 0, 0, // packed function at RVA 0x1000
        0x00, 0x20, 0, 0, 0x05, 0x20, 0, 0, // packed function at RVA 0x2000
    ];
    let arm64_dir = DataDirectory {
        virtual_address: 0,
        size: 16,
    };
    let arm64_ed = ExceptionData::parse_arm64_with_opts(
        &arm64_bytes,
        arm64_dir,
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect("two complete ARM64 entries must parse");
    assert_eq!(arm64_ed.len_arm64(), 2);
    let collected: Vec<Arm64RuntimeFunction> = arm64_ed
        .functions_arm64()
        .map(|result| result.expect("iteration stays in bounds"))
        .collect();
    assert_eq!(collected.len(), 2);
    assert_eq!(collected[0].begin_address, 0x1000);
    assert_eq!(
        arm64_ed
            .get_function_arm64(1)
            .expect("last valid index")
            .begin_address,
        0x2000
    );
    let arm64_boundary = arm64_ed
        .get_function_arm64(2)
        .expect_err("index 2 is out of bounds");
    assert!(
        arm64_boundary.to_string().contains("out of bounds"),
        "unexpected error: {arm64_boundary}"
    );

    let arm64_truncated = [0u8; 4];
    let arm64_truncation = ExceptionData::parse_arm64_with_opts(
        &arm64_truncated,
        DataDirectory {
            virtual_address: 0,
            size: 8,
        },
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect_err("a half-present ARM64 entry must be rejected");
    assert!(
        arm64_truncation
            .to_string()
            .contains("not fully backed by the image"),
        "unexpected error: {arm64_truncation}"
    );
    let arm64_messy = ExceptionData::parse_arm64_with_opts(
        &arm64_bytes,
        DataDirectory {
            virtual_address: 0,
            size: 10,
        },
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect_err("10 bytes cannot hold 8-byte ARM64 entries");
    assert!(
        arm64_messy
            .to_string()
            .contains("invalid exception directory table size"),
        "unexpected error: {arm64_messy}"
    );
    let arm64_empty = ExceptionData::parse_arm64_with_opts(
        &[],
        DataDirectory {
            virtual_address: 0,
            size: 0,
        },
        &sections,
        FILE_ALIGNMENT,
        &ParseOptions::default(),
    )
    .expect("empty ARM64 directory must parse");
    assert!(arm64_empty.is_empty());
    assert!(arm64_empty.functions_arm64().next().is_none());

    // ------------------------------------------------------------------
    // Full PE entry: strict vs permissive handling, indirect chains, cycles
    // ------------------------------------------------------------------

    let mut section_data = vec![0u8; 0x100];
    section_data[..0x48].copy_from_slice(&x64_pdata());
    section_data[0x80..0x80 + 12].copy_from_slice(&x64_unwind_info());

    let valid_image = build_pe(
        COFF_MACHINE_X86_64,
        DataDirectory {
            virtual_address: SECTION_VA,
            size: 72,
        },
        &section_data,
        SECTION_RAW as u32,
    );
    let pe = PE::parse_with_opts(&valid_image, &ParseOptions::default())
        .expect("the synthesized PE must parse in strict mode");
    assert_eq!(pe.sections.len(), 1);
    let ed = pe
        .exception_data
        .as_ref()
        .expect("exception data must be present");
    assert_eq!(ed.len(), 6);
    assert_eq!(
        ed.functions()
            .map(|result| result.expect("every entry is in bounds"))
            .count(),
        6
    );

    let entry0 = ed.get_function(0).expect("entry 0");
    let entry1 = ed.get_function(1).expect("entry 1");
    let entry2 = ed.get_function(2).expect("entry 2");
    let entry3 = ed.get_function(3).expect("entry 3");
    let entry4 = ed.get_function(4).expect("entry 4");
    let entry5 = ed.get_function(5).expect("entry 5");

    // Half-open range semantics through the full PE view as well.
    let found = ed
        .find_function(0x150)
        .expect("binary search must stay in bounds")
        .expect("RVA 0x150 is inside entry 0");
    assert_eq!(found.begin_address, 0x100);
    assert!(ed.find_function(0x200).unwrap().is_none());
    assert!(ed.find_function(0x50).unwrap().is_none());

    // Direct unwind info.
    let unwind = ed
        .get_unwind_info(entry0, &pe.sections)
        .expect("direct unwind info must resolve");
    assert_eq!(unwind.unwind_codes().flatten().count(), 2);

    // Legal single and multi-level indirect references resolve to the same unwind info.
    let via_one = ed
        .get_unwind_info(entry1, &pe.sections)
        .expect("one level of indirection must resolve");
    assert_eq!(via_one.unwind_codes().flatten().count(), 2);
    let via_two = ed
        .get_unwind_info(entry2, &pe.sections)
        .expect("two levels of indirection must resolve");
    assert_eq!(via_two.unwind_codes().flatten().count(), 2);

    // Separate queries may share records: the cycle detection is per resolution, not global.
    let again = ed
        .get_unwind_info(entry1, &pe.sections)
        .expect("reusing a record across queries is not a cycle");
    assert_eq!(again.unwind_codes().flatten().count(), 2);

    // Self loop and multi-record loop fail with the offending RVA instead of spinning.
    let self_loop = ed
        .get_unwind_info(entry3, &pe.sections)
        .expect_err("a self redirect must be reported");
    assert!(
        self_loop.to_string().contains("cycle") && self_loop.to_string().contains("1024"),
        "unexpected error: {self_loop}"
    );
    let multi_loop = ed
        .get_unwind_info(entry4, &pe.sections)
        .expect_err("a two-record cycle must be reported");
    assert!(
        multi_loop.to_string().contains("cycle") && multi_loop.to_string().contains("103c"),
        "unexpected error: {multi_loop}"
    );
    // Entry 5 is the other half of that cycle and must fail on its own as well, reporting the
    // RVA that closes the loop (entry 4 at 0x1030).
    let multi_loop_back = ed
        .get_unwind_info(entry5, &pe.sections)
        .expect_err("entering the cycle from entry 5 must fail too");
    assert!(
        multi_loop_back.to_string().contains("1030"),
        "unexpected error: {multi_loop_back}"
    );

    // Strict mode: a truncated image with an oversized exception directory fails the whole parse.
    let truncated_image = build_pe(
        COFF_MACHINE_X86_64,
        DataDirectory {
            virtual_address: SECTION_VA,
            size: 12,
        },
        &[0u8; 4],
        SECTION_RAW as u32,
    );
    assert_eq!(truncated_image.len(), SECTION_RAW + 4);
    PE::parse_with_opts(&truncated_image, &ParseOptions::default())
        .expect_err("strict mode must surface the truncated exception directory");

    // Permissive mode: the broken exception data is dropped, leaving no crash-prone `Some`,
    // while the rest of the PE remains readable.
    let permissive = ParseOptions::default().with_parse_mode(ParseMode::Permissive);
    let recovered = PE::parse_with_opts(&truncated_image, &permissive)
        .expect("permissive mode must keep parsing around the bad directory");
    assert!(
        recovered.exception_data.is_none(),
        "permissive mode must not retain a broken exception directory"
    );
    assert_eq!(recovered.sections.len(), 1);
    assert!(recovered.is_64);

    // ARM64 full PE entry: valid image exposes the ARM64 directory, truncation follows the same
    // strict/permissive policy.
    let arm64_data = [
        0x00u8, 0x10, 0, 0, 0x05, 0x10, 0, 0, 0x00, 0x20, 0, 0, 0x05, 0x20, 0, 0,
    ];
    let arm64_image = build_pe(
        COFF_MACHINE_ARM64,
        DataDirectory {
            virtual_address: SECTION_VA,
            size: 16,
        },
        &arm64_data,
        SECTION_RAW as u32,
    );
    let arm64_pe = PE::parse_with_opts(&arm64_image, &ParseOptions::default())
        .expect("the ARM64 PE must parse strictly");
    let arm64_ed = arm64_pe
        .exception_data
        .as_ref()
        .expect("ARM64 exception data must be present");
    assert_eq!(arm64_ed.len_arm64(), 2);
    assert_eq!(arm64_ed.functions_arm64().map(Result::unwrap).count(), 2);

    let arm64_truncated_image = build_pe(
        COFF_MACHINE_ARM64,
        DataDirectory {
            virtual_address: SECTION_VA,
            size: 8,
        },
        &[0u8; 4],
        SECTION_RAW as u32,
    );
    PE::parse_with_opts(&arm64_truncated_image, &ParseOptions::default())
        .expect_err("strict ARM64 parse must reject the truncated directory");
    let arm64_recovered = PE::parse_with_opts(&arm64_truncated_image, &permissive)
        .expect("permissive ARM64 parse must recover");
    assert!(arm64_recovered.exception_data.is_none());
    assert_eq!(arm64_recovered.sections.len(), 1);

    // A section table reference kept for readability: queries above run against this section.
    let section: &SectionTable = &pe.sections[0];
    assert_eq!(section.virtual_address, SECTION_VA);
}
