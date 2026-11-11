//! Fault-matrix tests for truncated and corrupted CompressedVector packets.
//!
//! The E57 reader reconstructs the bit width of every point record layer by
//! layer: from the section offset, the packet headers and the prototype in
//! the XML section. If a data packet is truncated, the bytes right behind it
//! can accidentally look like another valid packet. A naive reader would then
//! keep parsing and silently cross the section boundary.
//!
//! This module contains:
//! - a structure-aware fixture builder that emits only the mandatory parts
//!   (file header, XML with prototype, CompressedVector section, data packets
//!   and one index packet) and nothing else;
//! - a table of single-point fault operators. Every operator declares the
//!   layer that is expected to reject it, and the matrix test checks exactly
//!   that layer (or documents that the current reader tolerates/is fooled by
//!   a particular fault);
//! - checks for bounded resource usage and for the reader never consuming
//!   more than the page of the damaged packet;
//! - a deliberately naive parser that treats short packets as EOF to document
//!   which record it would lose first;
//! - positive cases for chunked reading and a writer/reader round trip.

use e57::{
    E57Reader, E57Writer, RawValues, Record, RecordDataType, RecordName, RecordValue, Result,
};
use std::cell::Cell;
use std::io::{Cursor, Read, Seek, SeekFrom};
use std::rc::Rc;

// ---------------------------------------------------------------------------
// Physical/logical layout constants
// ---------------------------------------------------------------------------

const PAGE_SIZE: usize = 1024;
const PAGE_PAYLOAD: usize = PAGE_SIZE - 4;
const FILE_HEADER_SIZE: u64 = 48;
const SECTION_HEADER_SIZE: u64 = 32;
const DATA_PACKET_HEADER_SIZE: u64 = 6;
const INDEX_PACKET_LENGTH: u64 = 32;

// Fixture geometry: one point cloud with three records and three data packets.
const PROTOTYPE_LEN: usize = 3;
const POINTS_PER_PACKET: usize = 180;
const PACKET_COUNT: usize = 3;
const RECORD_COUNT: u64 = (POINTS_PER_PACKET * PACKET_COUNT) as u64;

// Bit widths of the three prototype records.
const X_BITS: usize = 32; // Single
const INTENSITY_BITS: usize = 3; // Integer 0..=7
const RED_BITS: usize = 8; // Integer 0..=255

// The packed intensity stream is continuous across packets, exactly like the
// real writer: a packet only flushes complete bytes and carries the residual
// bits into the next packet. 540 values * 3 bit = 1620 bit = 202.5 bytes.
const INTENSITY_TOTAL_BYTES: usize = 203;
const INTENSITY_PACKET_SIZES: [usize; PACKET_COUNT] = [67, 68, 68];

// Logical layout of the hand-built fixture.
const XML_LOGICAL_OFFSET: u64 = FILE_HEADER_SIZE;
const SECTION_LOGICAL_OFFSET: u64 = 1024;
const DATA_LOGICAL_OFFSET: u64 = SECTION_LOGICAL_OFFSET + SECTION_HEADER_SIZE;
const PACKET_LOGICAL_LENGTH: u64 = 980;

// Physical layout derived from the logical one.
const SECTION_PHYSICAL_OFFSET: u64 = 1028;
const DATA_PHYSICAL_OFFSET: u64 = 1060;

/// CRC32C (Castagnoli), matching the checksum implementation of the library.
/// Stored big endian inside E57 pages for historical reasons.
fn crc32c(data: &[u8]) -> u32 {
    let mut table = [0_u32; 256];
    for i in 0..256u32 {
        let mut value = i;
        for _ in 0..8 {
            if value % 2 == 0 {
                value /= 2;
            } else {
                value /= 2;
                value ^= 0x82F6_3B78;
            }
        }
        table[i as usize] = value;
    }
    !data.iter().fold(!0_u32, |sum, &byte| {
        table[((sum ^ u32::from(byte)) as u8) as usize] ^ (sum >> 8)
    })
}

/// Translates a logical (checksum-free stream) offset into a physical file offset.
fn logical_to_physical(offset: u64) -> u64 {
    offset + (offset / PAGE_PAYLOAD as u64) * 4
}

// ---------------------------------------------------------------------------
// Structure-aware fixture builder
// ---------------------------------------------------------------------------

/// A fully hand-built, minimal but valid E57 file plus the metadata required
/// to target single bytes of it with fault operators.
#[derive(Clone)]
struct Fixture {
    /// Physical file content (pages with CRC bytes, ready for E57Reader).
    physical: Vec<u8>,
    /// Logical content (page payloads concatenated, without CRC bytes).
    logical: Vec<u8>,
    /// Logical start offset of every data packet.
    packet_starts: Vec<u64>,
}

impl Fixture {
    /// Builds the minimal valid fixture.
    fn valid() -> Self {
        Self::with_record_count(RECORD_COUNT)
    }

    /// Builds the fixture with an overridden XML recordCount value.
    /// The packet contents are unchanged, so a count larger than the actual
    /// data simulates the corresponding metadata corruption.
    fn with_record_count(record_count: u64) -> Self {
        let mut logical = Vec::new();

        // File header placeholder, finalized after the XML length is known.
        logical.extend_from_slice(&[0_u8; FILE_HEADER_SIZE as usize]);

        // XML section with the matching prototype.
        let xml = build_xml(record_count, SECTION_PHYSICAL_OFFSET);
        assert_eq!(logical.len() as u64, XML_LOGICAL_OFFSET);
        assert!(
            xml.len() < (SECTION_LOGICAL_OFFSET - XML_LOGICAL_OFFSET) as usize,
            "XML section must fit into the first logical page"
        );
        logical.extend_from_slice(xml.as_bytes());
        logical.resize(SECTION_LOGICAL_OFFSET as usize, 0);

        // CompressedVector section header. Offsets inside it are physical.
        assert_eq!(logical.len() as u64, SECTION_LOGICAL_OFFSET);
        let data_length = PACKET_LOGICAL_LENGTH * PACKET_COUNT as u64;
        let section_length = SECTION_HEADER_SIZE + data_length + INDEX_PACKET_LENGTH;
        let index_logical_offset = DATA_LOGICAL_OFFSET + data_length;
        let index_physical_offset = logical_to_physical(index_logical_offset);
        write_section_header(
            &mut logical,
            section_length,
            DATA_PHYSICAL_OFFSET,
            index_physical_offset,
        );

        // Three data packets, each carrying POINTS_PER_PACKET points.
        // The intensity bytestream is one continuous bit stream across all
        // packets (residual bits are carried over), so it is built once and
        // sliced per packet.
        let intensity_stream = build_intensity_stream();
        assert_eq!(intensity_stream.len(), INTENSITY_TOTAL_BYTES);
        let mut packet_starts = Vec::new();
        let mut intensity_offset = 0usize;
        for packet in 0..PACKET_COUNT {
            packet_starts.push(logical.len() as u64);
            let intensity_size = INTENSITY_PACKET_SIZES[packet];
            let intensity_chunk =
                &intensity_stream[intensity_offset..intensity_offset + intensity_size];
            write_data_packet(&mut logical, packet, intensity_chunk);
            intensity_offset += intensity_size;
            assert_eq!(
                logical.len() as u64,
                packet_starts[packet] + PACKET_LOGICAL_LENGTH
            );
        }
        assert_eq!(intensity_offset, INTENSITY_TOTAL_BYTES);
        assert_eq!(logical.len() as u64, index_logical_offset);

        // One minimal, structurally valid index packet (the reader skips it).
        write_index_packet(&mut logical, data_length);

        // Finalize the file header.
        write_file_header(&mut logical, XML_LOGICAL_OFFSET, xml.len() as u64);

        let physical = paginate(&logical);
        Self {
            physical,
            logical,
            packet_starts,
        }
    }

    /// Logical start of the given data packet.
    fn packet_start(&self, packet: usize) -> u64 {
        self.packet_starts[packet]
    }

    /// Edits a logical byte of the physical file while keeping page CRCs valid.
    fn patch_logical(&mut self, logical_offset: u64, value: u8) {
        let physical_offset = logical_to_physical(logical_offset) as usize;
        self.physical[physical_offset] = value;
        self.fix_page_crc(logical_offset);
    }

    /// Edits a logical u16 value (little endian) and repairs the page CRC.
    fn patch_logical_u16(&mut self, logical_offset: u64, value: u16) {
        let bytes = value.to_le_bytes();
        self.patch_logical(logical_offset, bytes[0]);
        self.patch_logical(logical_offset + 1, bytes[1]);
    }

    /// Edits a logical u64 value (little endian) and repairs the page CRCs.
    fn patch_logical_u64(&mut self, logical_offset: u64, value: u64) {
        let bytes = value.to_le_bytes();
        for (i, byte) in bytes.iter().enumerate() {
            self.patch_logical(logical_offset + i as u64, *byte);
        }
    }

    /// Recomputes the CRC of the page that contains the given logical offset.
    fn fix_page_crc(&mut self, logical_offset: u64) {
        let page = logical_offset / PAGE_PAYLOAD as u64;
        let payload_start = (page * PAGE_SIZE as u64) as usize;
        let payload_end = payload_start + PAGE_PAYLOAD;
        let crc = crc32c(&self.physical[payload_start..payload_end]);
        self.physical[payload_end..payload_end + 4].copy_from_slice(&crc.to_be_bytes());
    }

    /// Flips a payload bit without repairing the CRC, to simulate bit rot.
    fn flip_payload_bit(&mut self, logical_offset: u64) {
        let physical_offset = logical_to_physical(logical_offset) as usize;
        self.physical[physical_offset] ^= 0x01;
    }

    /// Returns a copy truncated to the given physical length.
    fn truncated_to(&self, physical_length: usize) -> Vec<u8> {
        self.physical[..physical_length].to_vec()
    }
}

// ---------------------------------------------------------------------------
// Fixture construction helpers
// ---------------------------------------------------------------------------

fn build_xml(record_count: u64, section_physical_offset: u64) -> String {
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
         <e57Root type=\"Structure\" xmlns=\"http://www.astm.org/COMMIT/E57/2010-e57-v1.0\">\n\
         <formatName type=\"String\"><![CDATA[ASTM E57 3D Imaging Data File]]></formatName>\n\
         <guid type=\"String\"><![CDATA[fault-matrix-fixture]]></guid>\n\
         <versionMajor type=\"Integer\">1</versionMajor>\n\
         <versionMinor type=\"Integer\">0</versionMinor>\n\
         <data3D type=\"Vector\" allowHeterogeneousChildren=\"1\">\n\
         <vectorChild type=\"Structure\">\n\
         <points type=\"CompressedVector\" fileOffset=\"{section_physical_offset}\" recordCount=\"{record_count}\">\n\
         <prototype type=\"Structure\">\n\
         <cartesianX type=\"Float\" precision=\"single\" minimum=\"0\" maximum=\"1000\">0</cartesianX>\n\
         <intensity type=\"Integer\" minimum=\"0\" maximum=\"7\">0</intensity>\n\
         <colorRed type=\"Integer\" minimum=\"0\" maximum=\"255\">0</colorRed>\n\
         </prototype>\n\
         </points>\n\
         </vectorChild>\n\
         </data3D>\n\
         <images2D type=\"Vector\" allowHeterogeneousChildren=\"1\">\n\
         </images2D>\n\
         </e57Root>\n"
    )
}

fn write_file_header(logical: &mut [u8], xml_offset: u64, xml_length: u64) {
    let physical_length = paginate(logical).len() as u64;
    let mut header = [0_u8; FILE_HEADER_SIZE as usize];
    header[0..8].copy_from_slice(b"ASTM-E57");
    header[8..12].copy_from_slice(&1u32.to_le_bytes());
    header[12..16].copy_from_slice(&0u32.to_le_bytes());
    header[16..24].copy_from_slice(&physical_length.to_le_bytes());
    header[24..32].copy_from_slice(&logical_to_physical(xml_offset).to_le_bytes());
    header[32..40].copy_from_slice(&xml_length.to_le_bytes());
    header[40..48].copy_from_slice(&(PAGE_SIZE as u64).to_le_bytes());
    logical[..FILE_HEADER_SIZE as usize].copy_from_slice(&header);
}

fn write_section_header(
    logical: &mut Vec<u8>,
    section_length: u64,
    data_offset: u64,
    index_offset: u64,
) {
    let mut header = [0_u8; SECTION_HEADER_SIZE as usize];
    header[0] = 1; // CompressedVector section ID
    header[8..16].copy_from_slice(&section_length.to_le_bytes());
    header[16..24].copy_from_slice(&data_offset.to_le_bytes());
    header[24..32].copy_from_slice(&index_offset.to_le_bytes());
    logical.extend_from_slice(&header);
}

/// Packs every intensity value of the whole point cloud into one continuous
/// 3-bit LSB-first bit stream, matching the cross-packet behavior of the writer.
fn build_intensity_stream() -> Vec<u8> {
    let total_points = POINTS_PER_PACKET * PACKET_COUNT;
    let mut stream = vec![0_u8; (total_points * INTENSITY_BITS).div_ceil(8)];
    for global_index in 0..total_points {
        let value = (global_index % 8) as u8;
        let base_bit = global_index * INTENSITY_BITS;
        for b in 0..INTENSITY_BITS {
            if value & (1 << b) != 0 {
                let bit = base_bit + b;
                stream[bit / 8] |= 1 << (bit % 8);
            }
        }
    }
    stream
}

fn write_data_packet(logical: &mut Vec<u8>, packet: usize, intensity_chunk: &[u8]) {
    let packet_start = logical.len();
    let mut x_stream = Vec::with_capacity(POINTS_PER_PACKET * X_BITS / 8);
    let mut red_stream = Vec::with_capacity(POINTS_PER_PACKET * RED_BITS / 8);

    for index in 0..POINTS_PER_PACKET {
        let global_index = packet * POINTS_PER_PACKET + index;

        // X: 32 bit, byte aligned.
        x_stream.extend_from_slice(&(global_index as f32).to_le_bytes());

        // Red: 8 bit per value.
        red_stream.push((global_index % 256) as u8);
    }

    let bytestreams: [&[u8]; PROTOTYPE_LEN] = [&x_stream, intensity_chunk, &red_stream];
    let payload_size: usize = bytestreams.iter().map(|stream| stream.len()).sum();
    let unpadded_length = DATA_PACKET_HEADER_SIZE as usize + PROTOTYPE_LEN * 2 + payload_size;
    assert!(unpadded_length <= PACKET_LOGICAL_LENGTH as usize);
    assert!(PACKET_LOGICAL_LENGTH.is_multiple_of(4));

    // Packet header: ID, flags, length-1 (includes trailing alignment), bytestream count.
    logical.push(1);
    logical.push(0);
    logical.extend_from_slice(&((PACKET_LOGICAL_LENGTH - 1) as u16).to_le_bytes());
    logical.extend_from_slice(&(PROTOTYPE_LEN as u16).to_le_bytes());

    // u16 size of every bytestream, then the payloads themselves.
    for stream in &bytestreams {
        logical.extend_from_slice(&(stream.len() as u16).to_le_bytes());
    }
    for stream in &bytestreams {
        logical.extend_from_slice(stream);
    }

    // Trailing zero padding up to the declared 4-byte aligned packet length.
    let packet_end = packet_start + PACKET_LOGICAL_LENGTH as usize;
    logical.extend(std::iter::repeat_n(0_u8, packet_end - logical.len()));
    assert_eq!(logical.len(), packet_end);
}

fn write_index_packet(logical: &mut Vec<u8>, data_length: u64) {
    let start = logical.len();
    let mut packet = [0_u8; INDEX_PACKET_LENGTH as usize];
    // Header: ID 0, reserved byte, packet length-1, one entry, level 0.
    packet[1..3].copy_from_slice(&((INDEX_PACKET_LENGTH - 1) as u16).to_le_bytes());
    packet[3..5].copy_from_slice(&1u16.to_le_bytes());
    packet[5] = 0;
    // Single level-0 entry: first record index and size of the covered chunk.
    packet[16..24].copy_from_slice(&0u64.to_le_bytes());
    packet[24..32].copy_from_slice(&data_length.to_le_bytes());
    logical.extend_from_slice(&packet);
    assert_eq!(logical.len() - start, INDEX_PACKET_LENGTH as usize);
}

/// Splits the logical stream into 1020-byte pages and appends a big endian
/// CRC32C to every page. The last page is zero padded, exactly like the writer.
fn paginate(logical: &[u8]) -> Vec<u8> {
    let page_count = logical.len().div_ceil(PAGE_PAYLOAD);
    let mut physical = Vec::with_capacity(page_count * PAGE_SIZE);
    for payload in logical.chunks(PAGE_PAYLOAD) {
        let start = physical.len();
        physical.extend_from_slice(payload);
        physical.extend(std::iter::repeat_n(0_u8, PAGE_PAYLOAD - payload.len()));
        let crc = crc32c(&physical[start..start + PAGE_PAYLOAD]);
        physical.extend_from_slice(&crc.to_be_bytes());
    }
    physical
}

// ---------------------------------------------------------------------------
// Fault operator catalog
// ---------------------------------------------------------------------------

/// The parser layer at which a fault is expected to be rejected.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Layer {
    /// PagedReader page checksum validation.
    PageCrc,
    /// CompressedVectorSectionHeader parsing.
    Section,
    /// PacketHeader parsing (ID, alignment, bytestream count).
    PacketHeader,
    /// QueueReader semantic checks or exhausted value queues.
    Queue,
    /// IO layer: truncated physical file / exhausted paged reader.
    Io,
}

/// What the reader is expected to do with a damaged fixture.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Expectation {
    /// Must fail at the given layer; the error message must contain the fragment.
    Reject { layer: Layer, message: &'static str },
    /// Currently tolerated: all declared records must still come back correctly.
    Tolerated,
    /// Currently not detected: reading succeeds but the returned values are wrong.
    SilentCorruption,
}

/// One entry of the fault matrix: a named single-point fault operator together
/// with the layer at which it is expected to be rejected.
struct FaultCase {
    name: &'static str,
    expectation: Expectation,
    /// When set, every point delivered before the failure must belong to a
    /// packet with an index below this one. In other words: the reader may
    /// consume up to the damaged packet but must never reach the packets
    /// behind it. `Some(2)` for a fault inside packet 1 therefore means that
    /// no record with a global index of 360 or higher may be returned.
    first_packet_that_must_not_be_reached: Option<usize>,
    apply: fn(&mut Fixture),
}

impl FaultCase {
    /// Constructor for faults that are allowed to consume up to the damaged
    /// packet but must never read past it.
    const fn bounded(
        name: &'static str,
        expectation: Expectation,
        first_unreachable_packet: usize,
        apply: fn(&mut Fixture),
    ) -> Self {
        Self {
            name,
            expectation,
            first_packet_that_must_not_be_reached: Some(first_unreachable_packet),
            apply,
        }
    }
}

const FAULT_MATRIX: &[FaultCase] = &[
    // --- Packet type and framing ------------------------------------------------
    FaultCase::bounded(
        "unknown packet type byte",
        Expectation::Reject {
            layer: Layer::PacketHeader,
            message: "packet",
        },
        2,
        |fixture| fixture.patch_logical(fixture.packet_start(1), 0xFF),
    ),
    FaultCase::bounded(
        "data packet length not aligned to four bytes",
        Expectation::Reject {
            layer: Layer::PacketHeader,
            message: "packet",
        },
        2,
        |fixture| {
            let length_field = fixture.packet_start(1) + 2;
            // length-1: 979 -> 978, so the declared length 979 is not divisible by four.
            fixture.patch_logical_u16(length_field, PACKET_LOGICAL_LENGTH as u16 - 2);
        },
    ),
    FaultCase::bounded(
        "zero bytestream count",
        Expectation::Reject {
            layer: Layer::PacketHeader,
            message: "byte stream count",
        },
        2,
        |fixture| {
            let count_field = fixture.packet_start(1) + 4;
            fixture.patch_logical_u16(count_field, 0);
        },
    ),
    FaultCase {
        name: "compression restart flag toggled",
        // The flag is parsed but never evaluated by this reader.
        expectation: Expectation::Tolerated,
        first_packet_that_must_not_be_reached: None,
        apply: |fixture| fixture.patch_logical(fixture.packet_start(1) + 1, 1),
    },
    FaultCase {
        name: "aligned but wrong packet length",
        // The declared length is only alignment-checked; framing comes from
        // the bytestream sizes, so an aligned wrong length is ignored today.
        expectation: Expectation::Tolerated,
        first_packet_that_must_not_be_reached: None,
        apply: |fixture| {
            let length_field = fixture.packet_start(1) + 2;
            fixture.patch_logical_u16(length_field, PACKET_LOGICAL_LENGTH as u16 - 5);
        },
    },
    // --- Prototype/queue semantics ----------------------------------------------
    FaultCase::bounded(
        "bytestream count does not match prototype size",
        Expectation::Reject {
            layer: Layer::Queue,
            message: "prototype",
        },
        2,
        |fixture| {
            let count_field = fixture.packet_start(1) + 4;
            fixture.patch_logical_u16(count_field, PROTOTYPE_LEN as u16 - 1);
        },
    ),
    FaultCase::bounded(
        "truncated bitpack tail in the last packet",
        // Dropping the final intensity byte shortens the continuous bit stream
        // by four bits. The reader does not frame packets from their declared
        // length, so the missing byte shifts every following read by one byte:
        // the next refill walks into the index packet with misaligned framing
        // and is stopped there by its reserved-byte validation instead of
        // silently reading across the section boundary.
        Expectation::Reject {
            layer: Layer::PacketHeader,
            message: "index packet",
        },
        3,
        |fixture| {
            let intensity_size_field = fixture.packet_start(2) + DATA_PACKET_HEADER_SIZE + 2; // X size comes first
            fixture.patch_logical_u16(intensity_size_field, (INTENSITY_PACKET_SIZES[2] - 1) as u16);
        },
    ),
    // --- Resource bounds --------------------------------------------------------
    FaultCase::bounded(
        "bytestream size grown beyond the end of the file",
        // The u16 size field caps any allocation at 65535 bytes; the tight
        // fixture ends first, so the over-long read must fail at the IO layer
        // instead of growing memory without bounds.
        Expectation::Reject {
            layer: Layer::Io,
            message: "read",
        },
        2,
        |fixture| {
            let size_field = fixture.packet_start(1) + DATA_PACKET_HEADER_SIZE;
            fixture.patch_logical_u16(size_field, u16::MAX);
        },
    ),
    FaultCase {
        name: "bytestream size grown into the following stream",
        // Detection gap: if the enlarged size still fits into the file, the
        // reader borrows a byte from the red bytestream and returns wrong data.
        expectation: Expectation::SilentCorruption,
        first_packet_that_must_not_be_reached: None,
        apply: |fixture| {
            let intensity_size_field = fixture.packet_start(2) + DATA_PACKET_HEADER_SIZE + 2;
            fixture.patch_logical_u16(intensity_size_field, (INTENSITY_PACKET_SIZES[2] + 1) as u16);
        },
    },
    // --- Section metadata -------------------------------------------------------
    FaultCase::bounded(
        "compressed vector section ID damaged",
        Expectation::Reject {
            layer: Layer::Section,
            message: "section",
        },
        0,
        |fixture| fixture.patch_logical(SECTION_LOGICAL_OFFSET, 7),
    ),
    FaultCase::bounded(
        "section data offset behind end of file",
        Expectation::Reject {
            layer: Layer::Io,
            message: "read",
        },
        0,
        |fixture| {
            let data_offset_field = SECTION_LOGICAL_OFFSET + 16;
            fixture.patch_logical_u64(data_offset_field, u64::MAX / 2);
        },
    ),
    FaultCase::bounded(
        "section data offset pointing at the XML section",
        // The XML start is not a valid packet: parsing must stop at the
        // packet type byte instead of wandering across the section boundary.
        Expectation::Reject {
            layer: Layer::PacketHeader,
            message: "packet",
        },
        0,
        |fixture| {
            let data_offset_field = SECTION_LOGICAL_OFFSET + 16;
            fixture.patch_logical_u64(data_offset_field, XML_LOGICAL_OFFSET);
        },
    ),
    // --- Page checksum ----------------------------------------------------------
    FaultCase::bounded(
        "bit rot inside a data packet with stale page checksum",
        Expectation::Reject {
            layer: Layer::PageCrc,
            message: "invalid checksum",
        },
        2,
        |fixture| {
            let damaged_logical = fixture.packet_start(1) + 10;
            fixture.flip_payload_bit(damaged_logical);
        },
    ),
];

#[test]
fn fault_matrix() {
    for case in FAULT_MATRIX {
        let mut fixture = Fixture::valid();
        (case.apply)(&mut fixture);
        match case.expectation {
            Expectation::Reject { layer, message } => {
                // Rejecting cases must surface the error at the declared layer
                // and must not deliver any record from a packet behind the
                // damaged one.
                let (points, err) = read_until_error(fixture.physical);
                let err = err.unwrap_or_else(|| panic!("case \"{}\" must fail", case.name));
                let rendered = err.to_string().to_lowercase();
                assert!(
                    rendered.contains(message),
                    "case \"{}\": expected rejection at {:?} (message containing \"{}\"), got: {err}",
                    case.name,
                    layer,
                    message
                );
                if let Some(first_unreachable) = case.first_packet_that_must_not_be_reached {
                    let max_allowed = first_unreachable * POINTS_PER_PACKET;
                    assert!(
                        points.len() <= max_allowed,
                        "case \"{}\": reader crossed the damaged packet boundary, \
                         delivered {} records but at most {max_allowed} belong to earlier packets",
                        case.name,
                        points.len()
                    );
                }
            }
            Expectation::Tolerated => {
                let points = read_all_points(fixture.physical).unwrap_or_else(|err| {
                    panic!("case \"{}\" must be tolerated: {err}", case.name)
                });
                assert_eq!(
                    points.len(),
                    RECORD_COUNT as usize,
                    "case \"{}\"",
                    case.name
                );
                assert_point_matches(&points, 539);
            }
            Expectation::SilentCorruption => {
                let points = read_all_points(fixture.physical).unwrap_or_else(|_| {
                    panic!("case \"{}\" demonstrates undetected corruption", case.name)
                });
                assert_eq!(points.len(), RECORD_COUNT as usize);
                let last_red = match &points[539][2] {
                    RecordValue::Integer(value) => *value,
                    other => panic!("wrong red record: {other:?}"),
                };
                assert_ne!(
                    last_red,
                    (539 % 256) as i64,
                    "case \"{}\": expected shifted, wrong red values",
                    case.name
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cursor wrapper that records the highest physical byte the reader touched
// ---------------------------------------------------------------------------

struct HighWaterCursor {
    inner: Cursor<Vec<u8>>,
    high_water: Rc<Cell<u64>>,
}

impl HighWaterCursor {
    fn new(data: Vec<u8>, high_water: Rc<Cell<u64>>) -> Self {
        Self {
            inner: Cursor::new(data),
            high_water,
        }
    }
}

impl Read for HighWaterCursor {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let pos = self.inner.position();
        let read = self.inner.read(buf)?;
        let end = pos + read as u64;
        self.high_water.set(self.high_water.get().max(end));
        Ok(read)
    }
}

impl Seek for HighWaterCursor {
    fn seek(&mut self, pos: SeekFrom) -> std::io::Result<u64> {
        self.inner.seek(pos)
    }
}

// ---------------------------------------------------------------------------
// Shared reading helpers
// ---------------------------------------------------------------------------

fn read_all_points(data: Vec<u8>) -> Result<Vec<RawValues>> {
    let mut reader = E57Reader::new(Cursor::new(data))?;
    let pc = reader.pointclouds().remove(0);
    reader.pointcloud_raw(&pc)?.collect()
}

/// Iterates until the first error (or the end) and returns both the points
/// delivered before the failure and the failure itself. Used to prove that a
/// damaged packet never leaks records from the packets behind it.
fn read_until_error(data: Vec<u8>) -> (Vec<RawValues>, Option<e57::Error>) {
    let mut reader = match E57Reader::new(Cursor::new(data)) {
        Ok(reader) => reader,
        Err(err) => return (Vec::new(), Some(err)),
    };
    let pc = reader.pointclouds().remove(0);
    let mut iter = match reader.pointcloud_raw(&pc) {
        Ok(iter) => iter,
        Err(err) => return (Vec::new(), Some(err)),
    };
    let mut points = Vec::new();
    for item in iter.by_ref() {
        match item {
            Ok(point) => points.push(point),
            Err(err) => return (points, Some(err)),
        }
    }
    (points, None)
}

/// Reads in small chunks to cross packet boundaries while iterating.
fn read_all_points_chunked(data: Vec<u8>) -> Result<Vec<RawValues>> {
    let mut reader = E57Reader::new(Cursor::new(data))?;
    let pc = reader.pointclouds().remove(0);
    let mut iter = reader.pointcloud_raw(&pc)?;
    let mut points = Vec::new();
    let mut batch = Vec::new();
    loop {
        batch.clear();
        for _ in 0..37 {
            match iter.next() {
                Some(item) => batch.push(item?),
                None => break,
            }
        }
        if batch.is_empty() {
            break;
        }
        points.append(&mut batch);
    }
    Ok(points)
}

fn intensity_at(global_index: usize) -> i64 {
    (global_index % 8) as i64
}

fn assert_point_matches(points: &[RawValues], global_index: usize) {
    let point = &points[global_index];
    assert_eq!(point.len(), PROTOTYPE_LEN);
    match &point[0] {
        RecordValue::Single(value) => assert_eq!(*value, global_index as f32),
        other => panic!("wrong X record: {other:?}"),
    }
    match &point[1] {
        RecordValue::Integer(value) => assert_eq!(*value, intensity_at(global_index)),
        other => panic!("wrong intensity record: {other:?}"),
    }
    match &point[2] {
        RecordValue::Integer(value) => assert_eq!(*value, (global_index % 256) as i64),
        other => panic!("wrong red record: {other:?}"),
    }
}

// ---------------------------------------------------------------------------
// Positive cases
// ---------------------------------------------------------------------------

#[test]
fn valid_fixture_chunked_reading() {
    let fixture = Fixture::valid();
    let points = read_all_points_chunked(fixture.physical).unwrap();
    assert_eq!(points.len(), RECORD_COUNT as usize);
    for index in [0, 1, 36, 179, 180, 359, 360, 539] {
        assert_point_matches(&points, index);
    }
}

#[test]
fn writer_reader_round_trip() {
    // Same record geometry as the hand-built fixture, but produced by the
    // public writer API and consumed through the public reader API. The
    // writer requires a complete Cartesian triple, so Y and Z are added.
    let prototype = vec![
        Record::CARTESIAN_X_F32,
        Record::CARTESIAN_Y_F32,
        Record::CARTESIAN_Z_F32,
        Record {
            name: RecordName::Intensity,
            data_type: RecordDataType::Integer { min: 0, max: 7 },
        },
    ];

    let mut buffer = Cursor::new(Vec::new());
    {
        let mut writer = E57Writer::new(&mut buffer, "round-trip-guid").unwrap();
        let mut pc = writer.add_pointcloud("pc-guid", prototype).unwrap();
        for global_index in 0..RECORD_COUNT as usize {
            pc.add_point(vec![
                RecordValue::Single(global_index as f32),
                RecordValue::Single((global_index + 1000) as f32),
                RecordValue::Single((global_index + 2000) as f32),
                RecordValue::Integer(intensity_at(global_index)),
            ])
            .unwrap();
        }
        pc.finalize().unwrap();
        writer.finalize().unwrap();
    }

    let points = read_all_points(buffer.into_inner()).unwrap();
    assert_eq!(points.len(), RECORD_COUNT as usize);
    for global_index in [0, 179, 180, 359, 360, 539] {
        let point = &points[global_index];
        assert_eq!(point.len(), 4);
        assert_eq!(point[0], RecordValue::Single(global_index as f32));
        assert_eq!(point[1], RecordValue::Single((global_index + 1000) as f32));
        assert_eq!(point[2], RecordValue::Single((global_index + 2000) as f32));
        assert_eq!(point[3], RecordValue::Integer(intensity_at(global_index)));
    }
}

#[test]
fn oversized_bytestream_size_is_resource_bounded() {
    // A corrupted bytestream size is the only packet field that directly
    // drives an allocation (ByteStreamReadBuffer::resize). Because it is a
    // u16, the largest single growth the parser can request is u16::MAX
    // bytes. The tight fixture ends long before that many bytes are
    // available, so the read must fail at the IO layer instead of growing
    // memory without a limit.
    let mut fixture = Fixture::valid();
    let size_field = fixture.packet_start(1) + DATA_PACKET_HEADER_SIZE;
    fixture.patch_logical_u16(size_field, u16::MAX);

    let mut reader = E57Reader::new(Cursor::new(fixture.physical)).unwrap();
    let pc = reader.pointclouds().remove(0);
    let mut iter = reader.pointcloud_raw(&pc).unwrap();

    // The 178 complete intensity values of packet 0 are still delivered.
    let mut delivered = 0usize;
    let err = loop {
        match iter.next() {
            Some(Ok(_)) => delivered += 1,
            Some(Err(err)) => break err,
            None => panic!("point cloud ended before the damaged packet was reached"),
        }
    };
    assert_eq!(delivered, 178);
    assert!(
        err.to_string().to_lowercase().contains("read"),
        "expected a bounded IO failure instead of an unbounded allocation, got: {err}"
    );
}

// ---------------------------------------------------------------------------
// Metadata fault: inflated record count
// ---------------------------------------------------------------------------

#[test]
fn inflated_record_count_runs_into_the_index_packet_and_padding() {
    // Promise more records than the packets contain. After all real points
    // are consumed, the reader must not invent data: it skips the index
    // packet and finally rejects the zero padding behind it as a bad packet.
    let fixture = Fixture::with_record_count(RECORD_COUNT + 60);

    let mut reader = E57Reader::new(Cursor::new(fixture.physical)).unwrap();
    let pc = reader.pointclouds().remove(0);
    assert_eq!(pc.records, RECORD_COUNT + 60);
    let mut iter = reader.pointcloud_raw(&pc).unwrap();
    for _ in 0..RECORD_COUNT {
        iter.next().unwrap().unwrap();
    }
    let err = iter.next().unwrap().unwrap_err();
    assert!(
        err.to_string().to_lowercase().contains("packet"),
        "expected a packet level rejection behind the section, got: {err}"
    );
}

// ---------------------------------------------------------------------------
// Read boundary: the reader stops inside the page of the damaged packet
// ---------------------------------------------------------------------------

#[test]
fn checksum_failure_stops_in_damaged_page() {
    let mut fixture = Fixture::valid();
    // Packet 1 starts at logical 2036; offset +10 lands on page 2.
    let damaged_logical = fixture.packet_start(1) + 10;
    fixture.flip_payload_bit(damaged_logical);

    let high_water = Rc::new(Cell::new(0_u64));
    let cursor = HighWaterCursor::new(fixture.physical, Rc::clone(&high_water));
    let mut reader = E57Reader::new(cursor).unwrap();
    let pc = reader.pointclouds().remove(0);
    let mut iter = reader.pointcloud_raw(&pc).unwrap();

    // The intensity bit stream is continuous across packets: the 67 intensity
    // bytes of packet 0 only hold 178 complete 3-bit values. Populating the
    // 179th point forces the reader to open packet 1 on the corrupted page,
    // which has to fail immediately with a checksum error.
    let mut delivered = 0usize;
    let err = loop {
        match iter.next() {
            Some(Ok(_)) => delivered += 1,
            Some(Err(err)) => break err,
            None => panic!("point cloud ended before the damaged packet was reached"),
        }
    };
    assert_eq!(delivered, 178);
    assert!(
        err.to_string().to_lowercase().contains("invalid checksum"),
        "expected CRC rejection, got: {err}"
    );
    drop(reader);

    // The reader may reach page 2 (physical 2048..3072), the page of the
    // damaged packet, but must never touch page 3 where packet 2 begins.
    assert!(
        high_water.get() > 2 * PAGE_SIZE as u64,
        "the damaged page itself was never read: {}",
        high_water.get()
    );
    assert!(
        high_water.get() <= 3 * PAGE_SIZE as u64,
        "reader crossed the damaged packet's page boundary: {}",
        high_water.get()
    );
}

// ---------------------------------------------------------------------------
// Control case: naive reader treating short packets as EOF
// ---------------------------------------------------------------------------

#[test]
fn truncated_packet_is_an_error_not_eof_and_naive_reader_loses_record_360() {
    let fixture = Fixture::valid();
    // Cut at a physical page boundary inside packet 2. Three full pages
    // (3072 physical / 3060 logical bytes) remain.
    let cut_logical = 3 * PAGE_PAYLOAD as u64;
    let cut_physical = logical_to_physical(cut_logical) as usize;
    assert_eq!(cut_physical, 3 * PAGE_SIZE);

    // The strict library reader must surface an error instead of stopping.
    let result = read_all_points(fixture.truncated_to(cut_physical));
    let err = result.expect_err("truncation must not look like EOF");
    assert!(
        err.to_string().to_lowercase().contains("read"),
        "expected an IO layer rejection, got: {err}"
    );

    // The deliberately naive parser below demonstrates the silent-loss
    // behavior and pins the index of the first record it drops.
    let naive_points =
        naive_read_treating_short_packets_as_eof(&fixture.logical[..cut_logical as usize]);
    assert_eq!(naive_points, 2 * POINTS_PER_PACKET);
    assert_eq!(naive_points, 360);
}

/// Deliberately naive reference parser: short packets are treated as EOF.
/// It only understands the minimal fixture layout and serves as a control to
/// show which record a lenient reader would lose first.
fn naive_read_treating_short_packets_as_eof(logical: &[u8]) -> usize {
    let mut pos = DATA_LOGICAL_OFFSET as usize;
    let mut points = 0usize;
    for _ in 0..PACKET_COUNT {
        if pos + DATA_PACKET_HEADER_SIZE as usize > logical.len() || logical[pos] != 1 {
            break; // Naive behavior: call it EOF.
        }
        let bytestream_count = u16::from_le_bytes([logical[pos + 4], logical[pos + 5]]) as usize;
        pos += DATA_PACKET_HEADER_SIZE as usize;
        if pos + bytestream_count * 2 > logical.len() {
            break;
        }
        let mut sizes = Vec::with_capacity(bytestream_count);
        for _ in 0..bytestream_count {
            sizes.push(u16::from_le_bytes([logical[pos], logical[pos + 1]]) as usize);
            pos += 2;
        }
        let total_size: usize = sizes.iter().sum();
        if pos + total_size > logical.len() {
            break; // Short packet: the naive reader stops here.
        }
        pos += total_size;
        points += sizes[0] / (X_BITS / 8);
        pos = (pos + 3) & !3; // Alignment, mirroring the real reader.
    }
    points
}
