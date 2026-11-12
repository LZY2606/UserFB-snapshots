//! Tests for VBRI table-of-contents (TOC) driven seeking in the MP3 demuxer.
//!
//! All streams are generated locally: MPEG1/MPEG2 Layer III frames with per-group
//! bitrates, CRC-protected headers, a non-audio prefix, a short final TOC group, and
//! non-zero `main_data_begin` near group boundaries. No audio is downloaded or decoded.
//! The tests independently track the physical byte position and audio frame index of
//! every MPEG frame, and compare packet bytes and timestamps item-by-item against a
//! from-the-start scan.

use std::collections::HashMap;
use std::io::{self, Cursor, Read, Seek, SeekFrom};
use std::sync::{Arc, Mutex};

use symphonia_bundle_mp3::MpaReader;
use symphonia_core::formats::prelude::*;
use symphonia_core::io::{MediaSource, MediaSourceStream};

/// MPEG1 Layer III bitrates in kbps, indexed by the header's bitrate field.
const MPEG1_L3_KBPS: [u32; 15] =
    [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320];

/// MPEG2 Layer III bitrates in kbps, indexed by the header's bitrate field.
const MPEG2_L3_KBPS: [u32; 15] =
    [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160];

/// Offset of the VBRI tag within the frame: 4-byte header + 32 bytes.
const VBRI_TAG_OFFSET: usize = 4 + 32;
/// Length of the fixed VBRI header: id, version, delay, quality, num_bytes, num_frames,
/// and the four u16 TOC fields.
const VBRI_FIXED_LEN: usize = 26;

/// Total length in bytes (header + body) of a MPEG Layer III frame.
fn frame_len(mpeg1: bool, bitrate_idx: u8) -> usize {
    let kbps = if mpeg1 {
        MPEG1_L3_KBPS[bitrate_idx as usize]
    }
    else {
        MPEG2_L3_KBPS[bitrate_idx as usize]
    };

    let (factor, sample_rate) = if mpeg1 { (144usize, 44_100usize) } else { (72, 22_050) };

    factor * kbps as usize * 1000 / sample_rate
}

/// The 4-byte header word of a MPEG Layer III frame (44.1/22.05 kHz stereo, no padding).
fn header_word(mpeg1: bool, crc: bool, bitrate_idx: u8) -> u32 {
    let mut word = 0xffe0_0000u32;
    word |= if mpeg1 { 0b11 } else { 0b10 } << 19;
    word |= 0b01 << 17; // Layer III.
    if !crc {
        word |= 1 << 16; // Protection bit: set means no CRC.
    }
    word |= (bitrate_idx as u32) << 12;
    word
}

/// Ways to corrupt or omit the VBRI table-of-contents.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Corrupt {
    /// A valid VBRI tag and TOC.
    None,
    /// No VBRI tag at all: the first frame is a plain audio frame.
    NoVbri,
    ZeroEntryCount,
    ZeroScale,
    ZeroFramesPerEntry,
    /// Entry width outside 1..=4.
    BadWidth,
    /// The first frame ends before all declared entries are present.
    TruncatedEntries,
    /// The cumulative spans exceed the declared audio byte range.
    SpansExceedRange,
    /// Structurally valid, but one entry is shifted so a group boundary lands mid-frame.
    ShiftedEntry,
}

#[derive(Clone)]
struct StreamParams {
    mpeg1: bool,
    crc: bool,
    /// Non-audio junk bytes before the first frame.
    prefix: usize,
    frames_per_entry: u16,
    entry_width: u16,
    scale: u16,
    /// Bitrate index of each group.
    group_bitrates: Vec<u8>,
    /// Number of MPEG frames in each group. Group 0 includes the VBRI tag frame.
    group_frames: Vec<usize>,
    /// main_data_begin values for selected MPEG frame indices.
    mdb: HashMap<usize, u16>,
    corrupt: Corrupt,
}

struct Built {
    bytes: Vec<u8>,
    /// Physical byte offset of each MPEG frame, including the VBRI tag frame at index 0.
    frame_pos: Vec<u64>,
    /// Total length of each MPEG frame.
    frame_len: Vec<usize>,
    /// Audio samples per MPEG frame.
    spf: u64,
    params: StreamParams,
}

impl Built {
    fn num_mpeg_frames(&self) -> usize {
        self.frame_pos.len()
    }

    /// The number of audio frames (excludes the VBRI tag frame).
    fn num_audio_frames(&self) -> usize {
        if self.params.corrupt == Corrupt::NoVbri {
            self.num_mpeg_frames()
        }
        else {
            self.num_mpeg_frames() - 1
        }
    }

    /// The bytes of an audio frame. Audio frame 0 is the first frame after the VBRI frame.
    fn audio_frame_bytes(&self, idx: usize) -> &[u8] {
        let mpeg_idx = if self.params.corrupt == Corrupt::NoVbri { idx } else { idx + 1 };
        let pos = self.frame_pos[mpeg_idx] as usize;
        &self.bytes[pos..pos + self.frame_len[mpeg_idx]]
    }
}

fn build_stream(params: StreamParams) -> Built {
    let n_groups = params.group_frames.len();
    assert_eq!(n_groups, params.group_bitrates.len());

    let has_vbri = params.corrupt != Corrupt::NoVbri;

    // Byte span of each group of frames.
    let spans: Vec<usize> = (0..n_groups)
        .map(|g| frame_len(params.mpeg1, params.group_bitrates[g]) * params.group_frames[g])
        .collect();

    let total_frames: usize = params.group_frames.iter().sum();
    let total_bytes: usize = spans.iter().sum();

    // The TOC entries (span / scale) for each group.
    let mut entries: Vec<u64> = if has_vbri {
        spans
            .iter()
            .map(|&span| {
                assert_eq!(span % params.scale as usize, 0, "span must be a multiple of scale");
                (span / params.scale as usize) as u64
            })
            .collect()
    }
    else {
        Vec::new()
    };

    if params.corrupt == Corrupt::ShiftedEntry {
        // Move one byte of span from a group to the previous one. The table remains
        // structurally valid, but the shifted group's start lands mid-frame.
        let k = n_groups / 3;
        entries[k] += 1;
        entries[k + 1] -= 1;
    }

    let mut built = Built {
        bytes: Vec::new(),
        frame_pos: Vec::new(),
        frame_len: Vec::new(),
        spf: if params.mpeg1 { 1152 } else { 576 },
        params,
    };

    // Non-audio prefix junk (decimal digits, never looks like a sync word).
    for i in 0..built.params.prefix {
        built.bytes.push(0x30 + (i % 10) as u8);
    }

    let mut mpeg_idx = 0usize;

    for g in 0..n_groups {
        let br = built.params.group_bitrates[g];
        let flen = frame_len(built.params.mpeg1, br);

        for _ in 0..built.params.group_frames[g] {
            built.frame_pos.push(built.bytes.len() as u64);
            built.frame_len.push(flen);

            let mut frame = vec![0u8; flen];
            let word = header_word(built.params.mpeg1, built.params.crc, br);
            frame[..4].copy_from_slice(&word.to_be_bytes());

            let hdr_size = if built.params.crc { 6 } else { 4 };

            if built.params.crc {
                frame[4] = 0xbe;
                frame[5] = 0xef;
            }

            if let Some(&mdb) = built.params.mdb.get(&mpeg_idx) {
                if mdb > 0 {
                    if built.params.mpeg1 {
                        // 9-bit main_data_begin.
                        frame[hdr_size] = (mdb >> 1) as u8;
                        frame[hdr_size + 1] = ((mdb & 1) << 7) as u8;
                    }
                    else {
                        // 8-bit main_data_begin.
                        frame[hdr_size] = mdb as u8;
                    }
                }
            }

            if mpeg_idx == 0 && has_vbri {
                write_vbri_tag(&mut frame, &built.params, &entries, total_frames, total_bytes);
            }

            built.bytes.extend_from_slice(&frame);
            mpeg_idx += 1;
        }
    }

    built
}

fn write_vbri_tag(
    frame: &mut [u8],
    params: &StreamParams,
    entries: &[u64],
    total_frames: usize,
    total_bytes: usize,
) {
    let mut tag = Vec::new();
    tag.extend_from_slice(b"VBRI");
    tag.extend_from_slice(&1u16.to_be_bytes()); // version
    tag.extend_from_slice(&0u16.to_be_bytes()); // delay
    tag.extend_from_slice(&0u16.to_be_bytes()); // quality

    let num_bytes = if params.corrupt == Corrupt::SpansExceedRange {
        // Declare an audio byte range smaller than the cumulative spans.
        (total_bytes - 1) as u32
    }
    else {
        total_bytes as u32
    };
    tag.extend_from_slice(&num_bytes.to_be_bytes());
    tag.extend_from_slice(&(total_frames as u32).to_be_bytes());

    let entry_count =
        if params.corrupt == Corrupt::ZeroEntryCount { 0 } else { entries.len() as u16 };
    let scale = if params.corrupt == Corrupt::ZeroScale { 0 } else { params.scale };
    let width = if params.corrupt == Corrupt::BadWidth { 5 } else { params.entry_width };
    let fpe =
        if params.corrupt == Corrupt::ZeroFramesPerEntry { 0 } else { params.frames_per_entry };

    tag.extend_from_slice(&entry_count.to_be_bytes());
    tag.extend_from_slice(&scale.to_be_bytes());
    tag.extend_from_slice(&width.to_be_bytes());
    tag.extend_from_slice(&fpe.to_be_bytes());

    // Encode the entries with the real entry width.
    let entry_width = params.entry_width as usize;
    let mut entry_bytes = Vec::new();

    for &entry in entries {
        assert!(entry < (1u64 << (8 * entry_width)), "entry does not fit width");
        entry_bytes.extend_from_slice(&entry.to_be_bytes()[8 - entry_width..]);
    }

    let room = frame.len() - VBRI_TAG_OFFSET - VBRI_FIXED_LEN;

    let write_len = if params.corrupt == Corrupt::TruncatedEntries {
        // The declared entry count requires more entries than fit in the frame. Only the
        // whole entries that fit are present.
        assert!(entry_bytes.len() > room, "truncated case must not fit in first frame");
        room / entry_width * entry_width
    }
    else {
        assert!(entry_bytes.len() <= room, "vbri tag does not fit in first frame");
        entry_bytes.len()
    };

    tag.extend_from_slice(&entry_bytes[..write_len]);

    frame[VBRI_TAG_OFFSET..VBRI_TAG_OFFSET + tag.len()].copy_from_slice(&tag);
}

/// A `MediaSource` that counts real read and seek operations.
#[derive(Clone, Copy, Default)]
struct Counters {
    reads: u64,
    bytes: u64,
    seeks: u64,
}

struct CountingSource {
    inner: Cursor<Vec<u8>>,
    seekable: bool,
    len: Option<u64>,
    stats: Arc<Mutex<Counters>>,
}

impl Read for CountingSource {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        let mut stats = self.stats.lock().unwrap();
        stats.reads += 1;
        stats.bytes += n as u64;
        Ok(n)
    }
}

impl Seek for CountingSource {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        self.stats.lock().unwrap().seeks += 1;
        self.inner.seek(pos)
    }
}

impl MediaSource for CountingSource {
    fn is_seekable(&self) -> bool {
        self.seekable
    }

    fn byte_len(&self) -> Option<u64> {
        self.len
    }
}

fn open_bytes(bytes: Vec<u8>, seekable: bool, with_len: bool) -> (MpaReader<'static>, Arc<Mutex<Counters>>) {
    let total = bytes.len() as u64;
    let stats = Arc::new(Mutex::new(Counters::default()));

    let src = CountingSource {
        inner: Cursor::new(bytes),
        seekable,
        len: if with_len { Some(total) } else { None },
        stats: stats.clone(),
    };

    let mss = MediaSourceStream::new(Box::new(src), Default::default());
    let reader = MpaReader::try_new(mss, FormatOptions::default()).expect("open");

    (reader, stats)
}

fn open_reader(built: &Built, seekable: bool, with_len: bool) -> (MpaReader<'static>, Arc<Mutex<Counters>>) {
    open_bytes(built.bytes.clone(), seekable, with_len)
}

fn snapshot(stats: &Arc<Mutex<Counters>>) -> Counters {
    *stats.lock().unwrap()
}

/// Read the full packet sequence from the current position.
fn read_all(reader: &mut MpaReader<'_>) -> Vec<(u64, Vec<u8>)> {
    let mut out = Vec::new();

    while let Some(pkt) = reader.next_packet().expect("packet") {
        out.push((pkt.pts.get() as u64, pkt.data.to_vec()));
    }

    out
}

fn seek_ts(reader: &mut MpaReader<'_>, mode: SeekMode, ts: u64) -> SeekedTo {
    reader
        .seek(mode, SeekTo::Timestamp { ts: Timestamp::from(ts as i64), track_id: 0 })
        .expect("seek")
}

/// Compute the audio frame index a seek to `required_ts` must land on, replicating the
/// from-the-start scan: find the frame containing the timestamp, then walk back over the
/// reference (reservoir) frames exactly like the demuxer does.
fn expected_actual_idx(built: &Built, required_ts: u64) -> usize {
    let target = (required_ts / built.spf) as usize;
    let target_mpeg = target + 1;

    let mdb = built.params.mdb.get(&target_mpeg).copied().unwrap_or(0) as u64;

    if mdb == 0 {
        return target;
    }

    let pos = built.frame_pos[target_mpeg];
    let max_ref = (target + 1).min(4);

    let mut n = 0;
    let mut ref_idx = target;

    while n < max_ref {
        ref_idx = target - n;

        if pos - built.frame_pos[ref_idx + 1] >= mdb {
            break;
        }

        n += 1;
    }

    ref_idx
}

/// Assert that the packets read after a seek are the exact suffix of the ground-truth
/// sequence, byte for byte and timestamp for timestamp.
fn assert_packets_from(
    reader: &mut MpaReader<'_>,
    ground: &[(u64, Vec<u8>)],
    start: usize,
    count: usize,
) {
    for (i, expected) in ground.iter().enumerate().skip(start).take(count) {
        let pkt = reader.next_packet().expect("packet").expect("not end of stream");
        assert_eq!(pkt.pts.get() as u64, expected.0, "pts of audio frame {i}");
        assert_eq!(&pkt.data[..], &expected.1[..], "bytes of audio frame {i}");
    }
}

/// Assert that the ground-truth sequence matches the independently tracked frame table.
fn assert_ground_matches_table(built: &Built, ground: &[(u64, Vec<u8>)]) {
    assert_eq!(ground.len(), built.num_audio_frames(), "audio frame count");

    for (i, (ts, data)) in ground.iter().enumerate() {
        assert_eq!(*ts, i as u64 * built.spf, "ts of audio frame {i}");
        assert_eq!(&data[..], built.audio_frame_bytes(i), "bytes of audio frame {i}");
    }
}

/// The table structure is parsed correctly: durations, the packet sequence (with the VBRI
/// tag frame skipped exactly once), and indexed landings for all entry widths and both
/// MPEG versions.
#[test]
fn vbri_layout() {
    // Case A: MPEG1, CRC, 2-byte entries, groups of 8, short final group, main_data_begin
    // near a group boundary.
    let built = build_stream(StreamParams {
        mpeg1: true,
        crc: true,
        prefix: 37,
        frames_per_entry: 8,
        entry_width: 2,
        scale: 1,
        group_bitrates: vec![9, 5, 12, 7, 14, 3, 10, 6, 8, 11, 4, 13, 9],
        group_frames: vec![8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 5],
        mdb: HashMap::from([(32usize, 400u16), (33, 128)]),
        corrupt: Corrupt::None,
    });
    check_layout(&built, &[80, 31, 32], true);

    // Case B: MPEG2, CRC, 1-byte entries, one frame per group.
    let built = build_stream(StreamParams {
        mpeg1: false,
        crc: true,
        prefix: 3,
        frames_per_entry: 1,
        entry_width: 1,
        scale: 1,
        group_bitrates: vec![8, 1, 2, 3, 1, 2, 3],
        group_frames: vec![1, 1, 1, 1, 1, 1, 1],
        mdb: HashMap::from([(4usize, 60u16)]),
        corrupt: Corrupt::None,
    });
    check_layout(&built, &[5, 3], false);

    // Case C: MPEG1, no CRC, 3-byte entries.
    let built = build_stream(StreamParams {
        mpeg1: true,
        crc: false,
        prefix: 0,
        frames_per_entry: 4,
        entry_width: 3,
        scale: 1,
        group_bitrates: vec![6, 9, 4, 13, 8],
        group_frames: vec![4, 4, 4, 4, 3],
        mdb: HashMap::new(),
        corrupt: Corrupt::None,
    });
    check_layout(&built, &[16, 5], false);

    // Case D: MPEG2, CRC, 4-byte entries, main_data_begin at a group boundary.
    let built = build_stream(StreamParams {
        mpeg1: false,
        crc: true,
        prefix: 20,
        frames_per_entry: 4,
        entry_width: 4,
        scale: 1,
        group_bitrates: vec![8, 4, 10, 2, 12],
        group_frames: vec![4, 4, 4, 4, 3],
        mdb: HashMap::from([(8usize, 100u16)]),
        corrupt: Corrupt::None,
    });
    check_layout(&built, &[17, 7], false);
}

fn check_layout(built: &Built, targets: &[usize], check_io: bool) {
    let total = built.bytes.len() as u64;

    let (mut reader, _) = open_reader(built, true, true);

    // The duration comes from the basic VBRI header and counts all MPEG frames.
    assert_eq!(
        reader.tracks()[0].num_frames,
        Some(built.num_mpeg_frames() as u64 * built.spf),
        "duration from vbri header"
    );

    // The packet sequence is exactly the audio frames in order: the VBRI tag frame is
    // skipped exactly once and timestamps match a from-the-start scan.
    let ground = read_all(&mut reader);
    assert_ground_matches_table(built, &ground);
    drop(reader);

    for &target_idx in targets {
        let ts = target_idx as u64 * built.spf;
        let expected_idx = expected_actual_idx(built, ts);

        let (mut reader, stats) = open_reader(built, true, true);
        let before = snapshot(&stats);
        let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
        let after = snapshot(&stats);

        assert_eq!(seeked.required_ts.get() as u64, ts);
        assert_eq!(seeked.actual_ts.get() as u64, expected_idx as u64 * built.spf);
        assert_packets_from(&mut reader, &ground, expected_idx, 4);

        if check_io {
            // The seek must not have rescanned the stream.
            let read_delta = after.bytes - before.bytes;
            assert!(read_delta * 3 < total, "seek re-read {read_delta} of {total} bytes");
        }
    }
}

/// On a long, valid indexed stream, repeated backward seeks near the end must land exactly
/// where a from-the-start scan would land while re-reading only a small fraction of the
/// compressed data.
#[test]
fn indexed_seek() {
    // 65 groups: 64 groups of 64 frames plus a short final group. 4097 MPEG frames total.
    let mut group_frames = vec![64usize; 64];
    group_frames.push(1);

    let bitrates = [9u8, 5, 12, 7, 14, 3, 10, 6];
    let group_bitrates: Vec<u8> = (0..65).map(|g| bitrates[g % bitrates.len()]).collect();

    // Non-zero main_data_begin on and around group boundaries.
    let mut mdb = HashMap::new();
    for g in 1..65usize {
        mdb.insert(g * 64, 300);
        mdb.insert(g * 64 + 1, 137);
    }

    let built = build_stream(StreamParams {
        mpeg1: true,
        crc: true,
        prefix: 37,
        frames_per_entry: 64,
        entry_width: 3,
        scale: 1,
        group_bitrates,
        group_frames,
        mdb,
        corrupt: Corrupt::None,
    });

    assert!(built.num_mpeg_frames() >= 4096);

    let total = built.bytes.len() as u64;
    let n_audio = built.num_audio_frames();

    // Opening the file must not read the whole stream up-front.
    let (mut reader, stats) = open_reader(&built, true, true);
    let open_bytes = snapshot(&stats).bytes;
    assert!(open_bytes * 3 < total, "open read {open_bytes} of {total} bytes");

    // Ground truth: the full packet sequence read from the start.
    let ground = read_all(&mut reader);
    assert_ground_matches_table(&built, &ground);
    drop(reader);

    // Targets near the end of the stream, seeked to in descending order, including exact
    // group boundaries (which carry non-zero main_data_begin).
    let targets: Vec<u64> =
        vec![4090u64, 4031, 3900, 3777, 3200, 3000, 0].into_iter().map(|i| i * built.spf).collect();

    for mode in [SeekMode::Accurate, SeekMode::Coarse] {
        let (mut reader, stats) = open_reader(&built, true, true);

        for &ts in &targets {
            let before = snapshot(&stats);
            let seeked = seek_ts(&mut reader, mode, ts);
            let after = snapshot(&stats);

            let expected_idx = expected_actual_idx(&built, ts);

            assert_eq!(seeked.required_ts.get() as u64, ts);
            assert_eq!(seeked.actual_ts.get() as u64, expected_idx as u64 * built.spf, "mode {mode:?} ts {ts}");
            assert!(seeked.actual_ts.get() <= seeked.required_ts.get());

            assert_packets_from(&mut reader, &ground, expected_idx, 3);

            let read_delta = after.bytes - before.bytes;
            assert!(
                read_delta * 3 < total,
                "indexed seek re-read {read_delta} of {total} bytes (mode {mode:?}, ts {ts})"
            );
            assert!(after.seeks > before.seeks, "seek must reposition the source");
        }
    }

    // A seekable source that does not report its byte length must still use the index:
    // the table itself provides the byte range.
    let (mut reader, stats) = open_reader(&built, true, false);

    for &ts in &targets[..4] {
        let before = snapshot(&stats);
        let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
        let after = snapshot(&stats);

        let expected_idx = expected_actual_idx(&built, ts);

        assert_eq!(seeked.actual_ts.get() as u64, expected_idx as u64 * built.spf);
        assert_packets_from(&mut reader, &ground, expected_idx, 3);

        let read_delta = after.bytes - before.bytes;
        assert!(read_delta * 3 < total, "seek without byte_len re-read {read_delta} of {total}");
    }

    let _ = n_audio;
}

/// When the target frame has a non-zero main_data_begin, the up-to-four-frame reservoir
/// preroll must be preserved, even when the target lands exactly on a TOC group boundary.
#[test]
fn reservoir_preroll() {
    let mut mdb = HashMap::new();
    // Group boundary frame: reservoir reaches 2 frames back (208-byte frames).
    // Note: main_data_begin is 9 bits for MPEG1, so 511 is the maximum.
    mdb.insert(16usize, 400u16);
    // Frame just after a group boundary: minimal reservoir.
    mdb.insert(17, 1);
    // Group boundary after a high-bitrate group (731-byte frames): 1 frame back.
    mdb.insert(24, 500);
    // Mid-group frame in a low-bitrate group (156-byte frames) with a reservoir larger
    // than the reference ring: the preroll is capped by the ring, like a full scan.
    mdb.insert(44, 511);

    let params = StreamParams {
        mpeg1: true,
        crc: true,
        prefix: 11,
        frames_per_entry: 8,
        entry_width: 2,
        scale: 1,
        group_bitrates: vec![9, 5, 12, 7, 14, 3, 10, 6, 8, 11, 4],
        group_frames: vec![8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 3],
        mdb,
        corrupt: Corrupt::None,
    };

    let built = build_stream(params.clone());

    let (mut reader, _) = open_reader(&built, true, true);
    let ground = read_all(&mut reader);
    assert_ground_matches_table(&built, &ground);
    drop(reader);

    // Reference: the same stream with the TOC disabled (zero entry count). The byte layout
    // is identical, only the index is gone, so seeks perform the classic scan from the
    // start.
    let mut ref_params = params;
    ref_params.corrupt = Corrupt::ZeroEntryCount;
    let ref_built = build_stream(ref_params);

    let (mut ref_reader, _) = open_reader(&ref_built, true, true);
    let ref_ground = read_all(&mut ref_reader);
    assert_eq!(ground, ref_ground, "reference stream must have identical audio frames");
    drop(ref_reader);

    // Audio frame indices whose MPEG frames carry main_data_begin, plus a control frame.
    for target_idx in [15usize, 16, 23, 43, 50] {
        let ts = target_idx as u64 * built.spf;
        let expected_idx = expected_actual_idx(&built, ts);

        for mode in [SeekMode::Accurate, SeekMode::Coarse] {
            let (mut reader, _) = open_reader(&built, true, true);
            let seeked = seek_ts(&mut reader, mode, ts);

            assert_eq!(seeked.required_ts.get() as u64, ts);
            assert_eq!(
                seeked.actual_ts.get() as u64,
                expected_idx as u64 * built.spf,
                "mode {mode:?} target {target_idx}"
            );

            // The reference frames themselves must be delivered, in order.
            assert_packets_from(&mut reader, &ground, expected_idx, 5);
        }

        // The indexed result must match the classic scan on the no-TOC reference stream.
        let (mut ref_reader, _) = open_reader(&ref_built, true, true);
        let ref_seeked = seek_ts(&mut ref_reader, SeekMode::Accurate, ts);
        assert_eq!(ref_seeked.actual_ts.get() as u64, expected_idx as u64 * built.spf);
    }

    // For all main_data_begin targets the preroll must actually move the position back.
    for target_idx in [15usize, 16, 23, 43] {
        assert!(expected_actual_idx(&built, target_idx as u64 * built.spf) < target_idx);
    }
}

/// Common parameters for the fallback tests: a valid stream whose TOC is then corrupted.
fn make_fallback_params(corrupt: Corrupt) -> StreamParams {
    StreamParams {
        mpeg1: true,
        crc: true,
        prefix: 13,
        frames_per_entry: 8,
        entry_width: 2,
        scale: 1,
        group_bitrates: vec![9, 5, 12, 7, 14, 3],
        group_frames: vec![8, 8, 8, 8, 8, 7],
        mdb: HashMap::new(),
        corrupt,
    }
}

/// Corrupted, missing, or unusable tables must disable the index and keep the original
/// behavior. The basic VBRI header (frame count, tag-frame skip) remains in effect.
#[test]
fn fallback() {
    for corrupt in [
        Corrupt::ZeroEntryCount,
        Corrupt::ZeroScale,
        Corrupt::ZeroFramesPerEntry,
        Corrupt::BadWidth,
        Corrupt::SpansExceedRange,
    ] {
        let built = build_stream(make_fallback_params(corrupt));
        check_fallback_stream(&built, format!("{corrupt:?}"));
    }

    // Truncated entries: the first frame ends before all declared entries are present.
    let built = build_stream(StreamParams {
        mpeg1: true,
        crc: true,
        prefix: 5,
        frames_per_entry: 1,
        entry_width: 4,
        scale: 1,
        group_bitrates: [vec![5u8], vec![9u8; 59]].concat(),
        group_frames: vec![1; 60],
        mdb: HashMap::new(),
        corrupt: Corrupt::TruncatedEntries,
    });
    check_fallback_stream(&built, "TruncatedEntries".to_string());

    // Structurally valid table, but one entry is shifted by a byte: the anchor of the
    // shifted group lands mid-frame and the seek must fall back to a reliable rescan
    // instead of publishing a wrong timestamp.
    let built = build_stream(StreamParams {
        frames_per_entry: 8,
        corrupt: Corrupt::ShiftedEntry,
        ..make_fallback_params(Corrupt::None)
    });

    let total = built.bytes.len() as u64;

    let (mut reader, _) = open_reader(&built, true, true);
    let ground = read_all(&mut reader);
    assert_ground_matches_table(&built, &ground);
    drop(reader);

    // Group 3 (MPEG frames 24..32) is the shifted one. Audio frame 27 is 4 frames into
    // the group, so its anchor is the shifted group start.
    let ts = 27 * built.spf;

    let (mut reader, stats) = open_reader(&built, true, true);
    let before = snapshot(&stats);
    let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
    let after = snapshot(&stats);

    assert_eq!(seeked.actual_ts.get() as u64, ts);
    assert_packets_from(&mut reader, &ground, 27, 3);

    // The result must have been produced by the reliable rescan, not the bad index.
    let read_delta = after.bytes - before.bytes;
    assert!(read_delta * 3 > total, "shifted entry must trigger the fallback rescan");

    // No VBRI tag at all: the original duration estimate and seek behavior apply.
    let built = build_stream(StreamParams {
        group_bitrates: vec![9, 9, 9, 9, 9, 9],
        corrupt: Corrupt::NoVbri,
        ..make_fallback_params(Corrupt::None)
    });

    let (mut reader, _) = open_reader(&built, true, true);
    let ground = read_all(&mut reader);
    assert_ground_matches_table(&built, &ground);
    drop(reader);

    let (mut reader, _) = open_reader(&built, true, true);
    let ts = 40 * built.spf;
    let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
    assert_eq!(seeked.actual_ts.get() as u64, ts);
    assert_packets_from(&mut reader, &ground, 40, 3);

    // A non-seekable source keeps forward-only behavior even when a valid index exists.
    let built = build_stream(make_fallback_params(Corrupt::None));

    let (mut reader, _) = open_reader(&built, false, false);
    let first = read_all(&mut reader);
    assert_eq!(first.len(), built.num_audio_frames());

    let (mut reader, _) = open_reader(&built, false, false);
    // Read a few packets so that a backward seek is requested from the past.
    for _ in 0..10 {
        reader.next_packet().expect("packet").expect("not end of stream");
    }

    // Backward seek must fail with a forward-only error.
    let backward = reader.seek(
        SeekMode::Accurate,
        SeekTo::Timestamp { ts: Timestamp::from(2 * built.spf as i64), track_id: 0 },
    );
    assert!(backward.is_err(), "backward seek on a non-seekable source must fail");

    // Forward seek still works by scanning forward.
    let ts = 20 * built.spf;
    let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
    assert_eq!(seeked.actual_ts.get() as u64, ts);
    assert_packets_from(&mut reader, &first, 20, 3);

    // A truncated file keeps working for the data that is present.
    let built = build_stream(make_fallback_params(Corrupt::None));

    let mut bytes = built.bytes.clone();
    bytes.truncate(bytes.len() - 100);

    let (mut reader, _) = open_bytes(bytes, true, true);
    let mut count = 0;
    while reader.next_packet().expect("packet").is_some() {
        count += 1;
    }
    assert!(count > 0 && count < built.num_audio_frames(), "truncated stream reads");

    // Seeking past the last complete frame runs into the truncated tail and must fail
    // with an out-of-range error, not a panic or a bad timestamp.
    let ts = built.num_audio_frames() as u64 * built.spf;
    let missing = reader.seek(
        SeekMode::Accurate,
        SeekTo::Timestamp { ts: Timestamp::from(ts as i64), track_id: 0 },
    );
    assert!(missing.is_err(), "seek into truncated tail must fail");

    // Seeking to an early frame still works.
    let ts0 = 4 * built.spf;
    let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts0);
    assert_eq!(seeked.actual_ts.get() as u64, ts0);
}

/// For a stream whose index is disabled, the original behavior must be preserved: the
/// duration still comes from the basic VBRI header, the packet sequence is intact, and
/// seeks work without the index (a late accurate seek visibly rescans the stream).
fn check_fallback_stream(built: &Built, name: String) {
    let total = built.bytes.len() as u64;
    let n_audio = built.num_audio_frames();

    let (mut reader, _) = open_reader(built, true, true);

    // The basic VBRI header remains valid: duration and tag-frame skip are preserved.
    assert_eq!(
        reader.tracks()[0].num_frames,
        Some(built.num_mpeg_frames() as u64 * built.spf),
        "{name}: duration from vbri header"
    );

    let ground = read_all(&mut reader);
    assert_ground_matches_table(built, &ground);
    drop(reader);

    // A late accurate seek cannot use an index: it must rescan, and still land correctly.
    let target_idx = n_audio - 3;
    let ts = target_idx as u64 * built.spf;

    let (mut reader, stats) = open_reader(built, true, true);
    let before = snapshot(&stats);
    let seeked = seek_ts(&mut reader, SeekMode::Accurate, ts);
    let after = snapshot(&stats);

    assert_eq!(seeked.required_ts.get() as u64, ts, "{name}");
    assert_eq!(seeked.actual_ts.get() as u64, ts, "{name}");
    assert_packets_from(&mut reader, &ground, target_idx, 3);

    let read_delta = after.bytes - before.bytes;
    assert!(read_delta * 3 > total, "{name}: index should be disabled");

    // Coarse seeks still work and land correctly.
    let (mut reader, _) = open_reader(&built, true, true);
    let seeked = seek_ts(&mut reader, SeekMode::Coarse, ts);
    assert_eq!(seeked.actual_ts.get() as u64, ts, "{name}: coarse");
    assert_packets_from(&mut reader, &ground, target_idx, 3);
}
