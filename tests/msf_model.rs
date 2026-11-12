// Structured fuzz regressions for the bottom MSF 7.00 layer.
//
// Every sample here is synthesized entirely in memory from an independent
// logical model (stream list + raw stream bytes). The production parser is
// never used to compute expected values: all expectations come from the bytes
// the model itself owns. Nothing in these tests executes file contents or
// touches a symbol server.
//
// Physical page tails are filled with TAIL_FILL, and generated payload bytes are
// guaranteed never to equal TAIL_FILL, so a parser that fails to truncate the
// last page leaks a distinguishable byte.

use std::collections::VecDeque;
use std::fmt;
use std::io::{Cursor, ErrorKind};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use pdb::{Error, PDB, Source, SourceSlice, SourceView, StreamIndex};

const MAGIC: &[u8] = b"Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00";

/// Value used for every unowned byte (page tails, unused pages).
const TAIL_FILL: u8 = 0xA5;

/// Fixed generator seed so samples are reproducible across machines.
const RNG_SEED: u64 = 0x0D5E_4D53_4637_0001;

/// Hard sample bound required by the test design.
const MAX_SAMPLE: usize = 1024 * 1024;

/// Page sizes exercised by the generated domain. The MSF header accepts
/// powers of two in `0x100..=128 * 0x10000`; 512 and 1024 keep every sample
/// comfortably below the 1 MiB cap while forcing multi-page directories.
const GENERATED_PAGE_SIZES: [u32; 2] = [512, 1024];

// Markers used by tools/check-msf-mutants.mjs. Every failing assertion that
// guards real behavior carries one of these, so the mutant harness can prove
// the failure points at bytes, request slices, or error results rather than at
// a compilation problem or an unrelated panic.
const MARKER_BYTE: &str = "msf-byte-mismatch";
const MARKER_SLICE: &str = "msf-slice-mismatch";
const MARKER_UNEXPECTED_OK: &str = "msf-unexpected-ok";
const MARKER_UNEXPECTED_ERR: &str = "msf-unexpected-err";
const MARKER_VIEW: &str = "msf-view-mismatch";

// ---------------------------------------------------------------------------
// Deterministic generator (no external rng dependency)
// ---------------------------------------------------------------------------

struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        Rng(seed | 1)
    }

    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    fn below(&mut self, bound: u64) -> usize {
        (self.next_u64() % bound) as usize
    }

    /// Payload byte that can never collide with the page-tail fill, which makes
    /// a missing last-page truncation observable byte-for-byte.
    fn payload_byte(&mut self) -> u8 {
        let b = (self.next_u64() & 0xff) as u8;
        if b == TAIL_FILL {
            TAIL_FILL.wrapping_add(1)
        } else {
            b
        }
    }
}

// ---------------------------------------------------------------------------
// Independent logical model
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
enum Logical {
    /// Stream number exists in the directory but is marked 0xffffffff: the
    /// stream does not exist and owns no page numbers.
    Absent,
    /// Stream exists with length zero; it owns no page numbers but is distinct
    /// from an absent stream.
    Empty,
    /// Stream with real content bytes.
    Data(Vec<u8>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Layout {
    /// Pages are handed out in ascending physical order.
    Forward,
    /// Each contiguous allocation is placed in descending order.
    Reverse,
    /// Low and high physical addresses are alternated.
    Interleaved,
}

impl Layout {
    fn name(self) -> &'static str {
        match self {
            Layout::Forward => "forward",
            Layout::Reverse => "reverse",
            Layout::Interleaved => "interleaved",
        }
    }

    fn all() -> [Layout; 3] {
        [Layout::Forward, Layout::Reverse, Layout::Interleaved]
    }
}

/// Reorder an owned pool of distinct pages according to the requested layout.
fn order_pool(pool: &[u32], layout: Layout) -> Vec<u32> {
    match layout {
        Layout::Forward => pool.to_vec(),
        Layout::Reverse => pool.iter().rev().copied().collect(),
        Layout::Interleaved => {
            let mut deque: VecDeque<u32> = pool.iter().copied().collect();
            let mut out = Vec::with_capacity(pool.len());
            let mut take_high = true;
            while let Some(page) = if take_high {
                deque.pop_back()
            } else {
                deque.pop_front()
            } {
                out.push(page);
                take_high = !take_high;
            }
            out
        }
    }
}

/// A fully serialized MSF image together with every fact the model knows about
/// it. None of these fields are populated by the production parser.
#[derive(Clone, Debug)]
struct Model {
    page_size: u32,
    layout: Layout,
    streams: Vec<Logical>,
    pages_used: u32,
    image: Vec<u8>,
    locator_pages: Vec<u32>,
    directory_pages: Vec<u32>,
    /// Per logical stream: `None` for absent streams, otherwise the ordered
    /// physical pages holding its payload (empty vec for zero-length streams).
    payload_pages: Vec<Option<Vec<u32>>>,
    /// The independently built directory byte sequence.
    directory_bytes: Vec<u8>,
}

impl Model {
    fn describe(&self) -> String {
        let mut msg = format!(
            "layout={} page_size={} pages_used={} locator_pages={:?} directory_pages={:?}",
            self.layout.name(),
            self.page_size,
            self.pages_used,
            self.locator_pages,
            self.directory_pages,
        );
        for (i, stream) in self.streams.iter().enumerate() {
            let pages = self.payload_pages[i]
                .as_ref()
                .map(|p| format!("{:?}", p))
                .unwrap_or_else(|| "-".to_string());
            match stream {
                Logical::Absent => {
                    msg.push_str(&format!("\n  stream[{}] absent pages={}", i, pages))
                }
                Logical::Empty => {
                    msg.push_str(&format!("\n  stream[{}] empty pages={}", i, pages))
                }
                Logical::Data(data) => msg.push_str(&format!(
                    "\n  stream[{}] data len={} pages={}",
                    i,
                    data.len(),
                    pages
                )),
            }
        }
        msg
    }

    /// Byte offset of u32 directory entry `entry_index` inside the image. The
    /// directory spans `directory_pages` in order and may cross page boundary.
    fn directory_entry_offset(&self, entry_index: usize) -> usize {
        let inner = entry_index
            .checked_mul(4)
            .expect("directory entry offset overflow");
        assert!(
            inner < self.directory_bytes.len(),
            "directory entry {} is outside the {}-byte directory",
            entry_index,
            self.directory_bytes.len()
        );
        let page_index = inner / self.page_size as usize;
        let page_offset = inner % self.page_size as usize;
        self.directory_pages[page_index] as usize * self.page_size as usize + page_offset
    }

    /// Directory entry index of stream `stream`'s `page_in_stream`-th payload
    /// page number.
    fn payload_entry_index(&self, stream: usize, page_in_stream: usize) -> usize {
        let before: usize = (1 + self.streams.len()) // stream count + size array
            + self.payload_pages[..stream]
                .iter()
                .map(|p| p.as_ref().map_or(0, Vec::len))
                .sum::<usize>();
        before + page_in_stream
    }

    fn expected_payload(&self, stream: usize) -> Option<&[u8]> {
        match &self.streams[stream] {
            Logical::Absent => None,
            Logical::Empty => Some(&[][..]),
            Logical::Data(data) => Some(data),
        }
    }
}

fn put_u32(buf: &mut [u8], offset: usize, value: u32) {
    buf[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn pages_for(bytes: usize, page_size: u32) -> usize {
    (bytes + page_size as usize - 1) / page_size as usize
}

/// Serialize a model whose physical page assignments are already fully chosen.
/// This is the single writer used by both generated and hand-built samples.
fn assemble(
    page_size: u32,
    layout: Layout,
    streams: Vec<Logical>,
    locator_pages: Vec<u32>,
    directory_pages: Vec<u32>,
    payload_pages: Vec<Option<Vec<u32>>>,
) -> Model {
    let ps = page_size as usize;

    // Independently build the directory byte stream:
    //   stream_count, one size per stream, then payload page numbers in order.
    let mut directory = Vec::new();
    directory.extend_from_slice(&(streams.len() as u32).to_le_bytes());
    for stream in &streams {
        let size = match stream {
            Logical::Absent => u32::MAX,
            Logical::Empty => 0,
            Logical::Data(data) => data.len() as u32,
        };
        directory.extend_from_slice(&size.to_le_bytes());
    }
    for pages in payload_pages.iter().flatten() {
        for page in pages {
            directory.extend_from_slice(&page.to_le_bytes());
        }
    }

    let payload_page_count: usize = payload_pages
        .iter()
        .flatten()
        .map(Vec::len)
        .sum();
    assert_eq!(
        directory.len(),
        4 * (1 + streams.len() + payload_page_count),
        "directory layout must match the MSF 7.00 stream table shape"
    );
    assert_eq!(
        directory_pages.len(),
        pages_for(directory.len(), page_size),
        "directory must use exactly enough pages for its bytes"
    );
    assert_eq!(
        locator_pages.len(),
        pages_for(directory_pages.len() * 4, page_size),
        "page-number table must use exactly enough pages for its u32s"
    );

    // Every referenced page must be distinct and positive; page 0 is the header.
    let mut referenced = vec![0u32];
    referenced.extend(locator_pages.iter().copied());
    referenced.extend(directory_pages.iter().copied());
    for pages in payload_pages.iter().flatten() {
        referenced.extend(pages.iter().copied());
    }
    let mut sorted = referenced.clone();
    sorted.sort_unstable();
    sorted.dedup();
    assert_eq!(sorted.len(), referenced.len(), "physical pages must not alias");
    let total_pages = *sorted.last().unwrap() as usize + 1;
    let pages_used = (total_pages - 1) as u32;

    let mut image = vec![TAIL_FILL; total_pages * ps];

    // Header (page 0).
    image[..MAGIC.len()].copy_from_slice(MAGIC);
    put_u32(&mut image, 32, page_size);
    put_u32(&mut image, 36, 1); // free page map
    put_u32(&mut image, 40, pages_used);
    put_u32(&mut image, 44, directory.len() as u32);
    put_u32(&mut image, 48, 0); // reserved
    for (i, page) in locator_pages.iter().enumerate() {
        put_u32(&mut image, 52 + 4 * i, *page);
    }

    // Page-number table: directory page numbers, in directory order.
    let mut locator = Vec::with_capacity(directory_pages.len() * 4);
    for page in &directory_pages {
        locator.extend_from_slice(&page.to_le_bytes());
    }
    for (i, chunk) in locator.chunks(ps).enumerate() {
        let start = locator_pages[i] as usize * ps;
        image[start..start + chunk.len()].copy_from_slice(chunk);
    }

    // Directory itself, written across directory pages in order so a directory
    // genuinely spanning multiple pages is exercised end-to-end.
    for (i, chunk) in directory.chunks(ps).enumerate() {
        let start = directory_pages[i] as usize * ps;
        image[start..start + chunk.len()].copy_from_slice(chunk);
    }

    // Payload pages; unused tail bytes stay at TAIL_FILL.
    for (stream, maybe_pages) in streams.iter().zip(payload_pages.iter()) {
        if let (Logical::Data(data), Some(pages)) = (stream, maybe_pages) {
            let mut written = 0;
            for page in pages {
                let start = *page as usize * ps;
                let take = std::cmp::min(ps, data.len() - written);
                image[start..start + take].copy_from_slice(&data[written..written + take]);
                // The remainder of this physical page must be the fill value.
                for tail in &mut image[start + take..start + ps] {
                    assert_eq!(*tail, TAIL_FILL);
                }
                written += take;
            }
            assert_eq!(written, data.len());
        }
    }

    // The library always reads a 4096-byte header view; pad short images while
    // keeping pages_used honest.
    if image.len() < 4096 {
        image.resize(4096, TAIL_FILL);
    }
    assert!(image.len() <= MAX_SAMPLE, "sample exceeds the 1 MiB domain bound");

    Model {
        page_size,
        layout,
        streams,
        pages_used,
        image,
        locator_pages,
        directory_pages,
        payload_pages,
        directory_bytes: directory,
    }
}

/// Choose physical pages for a model automatically, following `layout`.
fn build_model(page_size: u32, layout: Layout, streams: Vec<Logical>) -> Model {
    let ps = page_size;

    let mut payload_pages: Vec<Option<Vec<u32>>> = streams
        .iter()
        .map(|stream| match stream {
            Logical::Absent => None,
            Logical::Empty => Some(Vec::new()),
            Logical::Data(data) => Some(Vec::with_capacity(pages_for(data.len(), ps))),
        })
        .collect();

    let payload_page_count: usize = streams
        .iter()
        .map(|stream| match stream {
            Logical::Data(data) => pages_for(data.len(), ps),
            _ => 0,
        })
        .sum();

    // Placeholder directory size to size the metadata pages.
    let directory_size = 4 * (1 + streams.len() + payload_page_count);
    let directory_page_count = pages_for(directory_size, ps);
    let locator_page_count = pages_for(directory_page_count * 4, ps);

    let total_pages = 1 + locator_page_count + directory_page_count + payload_page_count;
    let free: Vec<u32> = (1..total_pages as u32).collect();

    // Not just the page order inside one allocation: the physical *region*
    // occupied by each role (page-number table, directory, payload) moves too.
    // These are the low-to-high role assignments per layout.
    enum Role {
        Locator,
        Directory,
        Payload,
    }
    let role_order: [Role; 3] = match layout {
        Layout::Forward => [Role::Locator, Role::Directory, Role::Payload],
        Layout::Reverse => [Role::Payload, Role::Directory, Role::Locator],
        Layout::Interleaved => [Role::Directory, Role::Payload, Role::Locator],
    };
    let role_size = |role: &Role| match role {
        Role::Locator => locator_page_count,
        Role::Directory => directory_page_count,
        Role::Payload => payload_page_count,
    };

    let mut locator_region: &[u32] = &[];
    let mut directory_region: &[u32] = &[];
    let mut payload_region: &[u32] = &[];
    let mut cursor = 0;
    for role in &role_order {
        let count = role_size(role);
        let region = &free[cursor..cursor + count];
        cursor += count;
        match role {
            Role::Locator => locator_region = region,
            Role::Directory => directory_region = region,
            Role::Payload => payload_region = region,
        }
    }
    assert_eq!(cursor, free.len());

    let locator_pages = order_pool(locator_region, layout);
    let directory_pages = order_pool(directory_region, layout);

    let mut pool_cursor = 0;
    for (stream, slot) in streams.iter().zip(payload_pages.iter_mut()) {
        if let Some(pages) = slot {
            let count = match stream {
                Logical::Data(data) => pages_for(data.len(), ps),
                _ => 0,
            };
            let group = &payload_region[pool_cursor..pool_cursor + count];
            pool_cursor += count;
            *pages = order_pool(group, layout);
        }
    }
    assert_eq!(pool_cursor, payload_region.len());

    assemble(
        page_size,
        layout,
        streams,
        locator_pages,
        directory_pages,
        payload_pages,
    )
}

// ---------------------------------------------------------------------------
// Generated and hand-built sample domains
// ---------------------------------------------------------------------------

/// Fixed-seed generated domain: absent/empty/content streams mixed together,
/// with every content stream crossing at least one page boundary and ending
/// short of a full final page.
fn seeded_streams(page_size: u32) -> Vec<Logical> {
    let mut rng = Rng::new(RNG_SEED);
    let ps = page_size as u64;

    // Chosen so the directory spans >= 2 pages at both 512 and 1024 byte pages
    // while the whole image stays well below 1 MiB.
    let content_page_counts = [45usize, 75, 35, 95, 10];

    let mut streams = Vec::new();
    let mut content = 0;
    for kind in [
        "absent",
        "empty",
        "data",
        "data",
        "absent",
        "data",
        "empty",
        "data",
        "data",
    ] {
        match kind {
            "absent" => streams.push(Logical::Absent),
            "empty" => streams.push(Logical::Empty),
            _ => {
                let pages = content_page_counts[content];
                content += 1;
                // 1..page_size-1 bytes short of a whole number of pages, so the
                // stream both crosses a boundary and has a truncated last page.
                let missing = 1 + rng.below(ps - 1);
                let len = pages * page_size as usize - missing;
                let data: Vec<u8> = (0..len).map(|_| rng.payload_byte()).collect();
                streams.push(Logical::Data(data));
            }
        }
    }
    streams
}

/// Small, fully hand-specified counterexample: an absent stream, an empty
/// stream, a two-page non-contiguous payload, and a tiny sibling payload.
/// Physical gaps and reversed page order are chosen literally.
fn hand_model() -> Model {
    let page_size = 512u32;
    let ps = page_size as usize;

    let big: Vec<u8> = (0..515)
        .map(|i| {
            let b = ((i * 37 + 11) % 251) as u8;
            if b == TAIL_FILL {
                TAIL_FILL.wrapping_add(1)
            } else {
                b
            }
        })
        .collect();
    let small: Vec<u8> = vec![0x12, 0x34, 0x56];
    assert!(big.iter().all(|&b| b != TAIL_FILL));
    assert!(small.iter().all(|&b| b != TAIL_FILL));

    let streams = vec![
        Logical::Absent,
        Logical::Empty,
        Logical::Data(big),
        Logical::Data(small),
    ];

    // Pages 3 and 5 are deliberately left unused to create real gaps.
    let locator_pages = vec![1];
    let directory_pages = vec![2];
    let payload_pages = vec![
        None,
        Some(Vec::new()),
        Some(vec![6, 4]), // non-contiguous, backwards on disk
        Some(vec![7]),
    ];

    let model = assemble(
        page_size,
        Layout::Forward,
        streams,
        locator_pages,
        directory_pages,
        payload_pages,
    );
    assert_eq!(model.image.len(), 8 * ps);

    // Independently spell out the directory bytes we expect on disk.
    let mut expected_directory = Vec::new();
    for value in [4u32, u32::MAX, 0, 515, 3, 6, 4, 7] {
        expected_directory.extend_from_slice(&value.to_le_bytes());
    }
    let on_disk: Vec<u8> = (0..expected_directory.len())
        .map(|o| model.image[2 * ps + o])
        .collect();
    assert_eq!(
        on_disk, expected_directory,
        "hand-built directory must match the literal expected sequence"
    );
    model
}

// ---------------------------------------------------------------------------
// Assertion helpers: all compare against model-owned bytes
// ---------------------------------------------------------------------------

fn first_diff(left: &[u8], right: &[u8]) -> Option<usize> {
    left.iter().zip(right).position(|(a, b)| a != b)
}

fn assert_bytes_equal(model: &Model, context: &str, got: &[u8], expected: &[u8]) {
    if got.len() != expected.len() {
        panic!(
            "{}: {} length differs\nmodel:\n{}\ngot len={} expected len={}",
            MARKER_BYTE,
            context,
            model.describe(),
            got.len(),
            expected.len()
        );
    }
    if let Some(offset) = first_diff(got, expected) {
        panic!(
            "{}: {} byte {} differs\nmodel:\n{}\ngot={:#04x} expected={:#04x}",
            MARKER_BYTE,
            context,
            offset,
            model.describe(),
            got[offset],
            expected[offset]
        );
    }
    // Explicitly prove no page-tail fill leaked into the stream.
    if let Some(offset) = got.iter().position(|&b| b == TAIL_FILL) {
        panic!(
            "{}: {} borrowed page-tail fill at byte {}\nmodel:\n{}",
            MARKER_BYTE,
            context,
            offset,
            model.describe()
        );
    }
}

fn open_cursor(model: &Model) -> PDB<'static, Cursor<Vec<u8>>> {
    PDB::open(Cursor::new(model.image.clone())).unwrap_or_else(|e| {
        panic!(
            "{}: open failed for a valid model: {:?}\nmodel:\n{}",
            MARKER_UNEXPECTED_ERR,
            e,
            model.describe()
        )
    })
}

fn expect_absent(model: &Model, pdb: &mut PDB<'_, Cursor<Vec<u8>>>, index: usize) {
    match pdb.raw_stream(StreamIndex(index as u16)) {
        Err(Error::StreamNotFound(n)) => assert_eq!(n, index as u32),
        Err(e) => panic!(
            "{}: stream {} expected StreamNotFound, got {:?}\nmodel:\n{}",
            MARKER_UNEXPECTED_ERR,
            index,
            e,
            model.describe()
        ),
        Ok(_) => panic!(
            "{}: absent stream {} returned content\nmodel:\n{}",
            MARKER_UNEXPECTED_OK,
            index,
            model.describe()
        ),
    }
}

fn expect_stream(model: &Model, pdb: &mut PDB<'_, Cursor<Vec<u8>>>, index: usize) {
    let expected = model.expected_payload(index).expect("use expect_absent");
    let stream = match pdb.raw_stream(StreamIndex(index as u16)) {
        Ok(Some(stream)) => stream,
        Ok(None) => panic!(
            "{}: present stream {} returned None\nmodel:\n{}",
            MARKER_UNEXPECTED_ERR,
            index,
            model.describe()
        ),
        Err(e) => panic!(
            "{}: present stream {} failed: {:?}\nmodel:\n{}",
            MARKER_UNEXPECTED_ERR,
            index,
            e,
            model.describe()
        ),
    };
    assert_bytes_equal(
        model,
        &format!("stream[{}]", index),
        stream.as_slice(),
        expected,
    );
}

fn assert_no_fabricated_content<T>(context: &str, result: &pdb::Result<T>) {
    if result.is_ok() {
        panic!(
            "{}: {} must fail instead of returning fabricated content",
            MARKER_UNEXPECTED_OK,
            context
        );
    }
}

// ---------------------------------------------------------------------------
// Test 1: model layout
// ---------------------------------------------------------------------------

#[test]
fn model_layout() {
    let mut generated = Vec::new();

    for &page_size in &GENERATED_PAGE_SIZES {
        for &layout in &Layout::all() {
            let model = build_model(page_size, layout, seeded_streams(page_size));

            // Structural invariants of the generated domain.
            assert!(
                model.directory_pages.len() >= 2,
                "directory must really span at least two pages\nmodel:\n{}",
                model.describe()
            );
            assert!(model.image.len() >= 4096);
            assert!(model.image.len() <= MAX_SAMPLE);
            assert!(model.streams.iter().any(|s| matches!(s, Logical::Absent)));
            assert!(model.streams.iter().any(|s| matches!(s, Logical::Empty)));
            assert!(model.streams.iter().any(|s| matches!(s, Logical::Data(d) if d.len() > page_size as usize
                && d.len() % page_size as usize != 0)));

            let mut pdb = open_cursor(&model);

            for (index, stream) in model.streams.iter().enumerate() {
                match stream {
                    Logical::Absent => expect_absent(&model, &mut pdb, index),
                    Logical::Empty | Logical::Data(_) => expect_stream(&model, &mut pdb, index),
                }
            }

            // An index beyond the directory is a StreamNotFound, not content.
            let beyond = model.streams.len() as u16 + 20;
            match pdb.raw_stream(StreamIndex(beyond)) {
                Err(Error::StreamNotFound(n)) => assert_eq!(n, beyond as u32),
                other => panic!(
                    "{}: out-of-range index gave {:?}\nmodel:\n{}",
                    MARKER_UNEXPECTED_ERR,
                    other,
                    model.describe()
                ),
            }

            // The null StreamIndex follows the existing API: Ok(None), which is
            // different from an absent numbered stream's Err(StreamNotFound).
            match pdb.raw_stream(StreamIndex::none()) {
                Ok(None) => {}
                other => panic!(
                    "{}: null index gave {:?}\nmodel:\n{}",
                    MARKER_UNEXPECTED_ERR,
                    other,
                    model.describe()
                ),
            }

            generated.push(model);
        }
    }

    // Layouts must really relocate physical pages, including the page-number
    // table (locator), the directory, and every payload page list.
    for (i, &page_size) in GENERATED_PAGE_SIZES.iter().enumerate() {
        let offset = i * Layout::all().len();
        let forward = &generated[offset];
        let reverse = &generated[offset + 1];
        let interleaved = &generated[offset + 2];
        assert_eq!(forward.page_size, page_size);
        assert_eq!(
            forward.locator_pages,
            vec![1],
            "forward layout keeps the page-number table adjacent to the header"
        );
        assert_ne!(forward.locator_pages, reverse.locator_pages);
        assert_ne!(forward.locator_pages, interleaved.locator_pages);
        assert_ne!(forward.directory_pages, reverse.directory_pages);
        assert_ne!(forward.directory_pages, interleaved.directory_pages);
        assert_ne!(reverse.directory_pages, interleaved.directory_pages);
        assert_ne!(forward.payload_pages, reverse.payload_pages);
        assert_ne!(forward.payload_pages, interleaved.payload_pages);
    }

    // Hand-built counterexample, including the non-contiguous two-page stream.
    let hand = hand_model();
    let mut pdb = open_cursor(&hand);
    expect_absent(&hand, &mut pdb, 0);
    expect_stream(&hand, &mut pdb, 1);
    expect_stream(&hand, &mut pdb, 2);
    expect_stream(&hand, &mut pdb, 3);

    // The physical tail of the partially used final page must still be fill in
    // the image itself, proving the stream cannot borrow those bytes.
    let ps = hand.page_size as usize;
    let last_page = hand.payload_pages[3].as_ref().unwrap()[0] as usize * ps;
    assert!(hand.image[last_page + 3..last_page + ps]
        .iter()
        .all(|&b| b == TAIL_FILL));
}

// ---------------------------------------------------------------------------
// Test 2: relocation law
// ---------------------------------------------------------------------------

#[test]
fn relocation_law() {
    for &page_size in &GENERATED_PAGE_SIZES {
        let logical = seeded_streams(page_size);
        let models: Vec<Model> = Layout::all()
            .iter()
            .map(|&layout| build_model(page_size, layout, logical.clone()))
            .collect();

        // Relocation must change physical assignments while keeping the same
        // logical stream list and raw bytes.
        assert_ne!(models[0].payload_pages, models[1].payload_pages);
        assert_ne!(models[0].payload_pages, models[2].payload_pages);
        for pair in models.windows(2) {
            assert_eq!(pair[0].streams, pair[1].streams);
        }

        let data_indices: Vec<usize> = logical
            .iter()
            .enumerate()
            .filter(|(_, s)| matches!(s, Logical::Data(_)))
            .map(|(i, _)| i)
            .collect();

        for model in &models {
            let mut pdb = open_cursor(model);

            // Forward access order.
            for index in 0..model.streams.len() {
                match &model.streams[index] {
                    Logical::Absent => expect_absent(model, &mut pdb, index),
                    _ => expect_stream(model, &mut pdb, index),
                }
            }

            // Reverse access order.
            for index in (0..model.streams.len()).rev() {
                if let Some(expected) = model.expected_payload(index) {
                    let stream = pdb
                        .raw_stream(StreamIndex(index as u16))
                        .expect("present stream reads")
                        .expect("present stream is Some");
                    assert_bytes_equal(
                        model,
                        &format!("reverse stream[{}]", index),
                        stream.as_slice(),
                        expected,
                    );
                } else {
                    expect_absent(model, &mut pdb, index);
                }
            }

            // Repeated and interleaved access order.
            let pattern = [
                data_indices[0],
                data_indices[1],
                data_indices[0],
                *data_indices.last().unwrap(),
                data_indices[2],
                data_indices[1],
                data_indices[0],
            ];
            for index in pattern {
                expect_stream(model, &mut pdb, index);
            }

            // Hold one returned Stream while touching other streams; its bytes
            // must remain stable across subsequent lazy accesses.
            let retained_index = data_indices[0];
            let retained = pdb
                .raw_stream(StreamIndex(retained_index as u16))
                .expect("retained stream reads")
                .expect("retained stream is Some");
            let retained_expected = model.expected_payload(retained_index).unwrap().to_vec();
            assert_bytes_equal(
                model,
                "retained stream before churn",
                retained.as_slice(),
                &retained_expected,
            );
            for &other in &data_indices[1..] {
                expect_stream(model, &mut pdb, other);
            }
            expect_absent(model, &mut pdb, 0);
            expect_stream(model, &mut pdb, 1);
            assert_bytes_equal(
                model,
                "retained stream after churn",
                retained.as_slice(),
                &retained_expected,
            );

            // A fresh read must agree byte-for-byte with the retained view.
            let refetched = pdb
                .raw_stream(StreamIndex(retained_index as u16))
                .expect("refetched stream reads")
                .expect("refetched stream is Some");
            assert_eq!(
                retained.as_slice(),
                refetched.as_slice(),
                "{}: retained and refetched views diverged\nmodel:\n{}",
                MARKER_VIEW,
                model.describe()
            );
        }
    }
}

// ---------------------------------------------------------------------------
// Test 3: bounded corruption and lazy error behavior
// ---------------------------------------------------------------------------

struct Corruption {
    name: &'static str,
    image: Vec<u8>,
    /// Streams that must still read correctly after the corruption.
    siblings: Vec<usize>,
    /// Stream whose access must fail; None when every access must fail.
    victim: Option<usize>,
}

fn corrupt_zero_page_reference(model: &Model) -> Corruption {
    let victim = model
        .payload_pages
        .iter()
        .position(|p| p.as_ref().map_or(false, |pages| pages.len() >= 2))
        .expect("model has a multi-page stream");
    let mut image = model.image.clone();
    let offset = model.directory_entry_offset(model.payload_entry_index(victim, 0));
    let before = [
        image[offset],
        image[offset + 1],
        image[offset + 2],
        image[offset + 3],
    ];
    assert_eq!(
        u32::from_le_bytes(before),
        model.payload_pages[victim].as_ref().unwrap()[0]
    );
    put_u32(&mut image, offset, 0);
    Corruption {
        name: "payload page number replaced by zero",
        image,
        siblings: model
            .streams
            .iter()
            .enumerate()
            .filter(|(i, s)| *i != victim && matches!(s, Logical::Data(_)))
            .map(|(i, _)| i)
            .collect(),
        victim: Some(victim),
    }
}

fn corrupt_out_of_range_reference(model: &Model) -> Corruption {
    let victim = model
        .payload_pages
        .iter()
        .position(|p| p.as_ref().map_or(false, |pages| !pages.is_empty()))
        .expect("model has a payload stream");
    let mut image = model.image.clone();
    let offset = model.directory_entry_offset(model.payload_entry_index(victim, 0));
    let bad = model.pages_used + 1;
    put_u32(&mut image, offset, bad);
    Corruption {
        name: "payload page number beyond pages_used",
        image,
        siblings: model
            .streams
            .iter()
            .enumerate()
            .filter(|(i, s)| *i != victim && matches!(s, Logical::Data(_)))
            .map(|(i, _)| i)
            .collect(),
        victim: Some(victim),
    }
}

fn corrupt_truncated_directory(model: &Model) -> Corruption {
    // Cut the final directory page in half; payload pages past that point are
    // removed as well, so every lazy stream access must fail rather than
    // concatenate partial directory data.
    let last_directory_page = *model.directory_pages.last().unwrap();
    let cut = last_directory_page as usize * model.page_size as usize
        + model.page_size as usize / 2;
    let mut image = model.image.clone();
    assert!(cut < image.len());
    image.truncate(cut);
    Corruption {
        name: "directory truncated on its final page",
        image,
        siblings: Vec::new(),
        victim: None,
    }
}

fn corrupt_truncated_locator() -> Corruption {
    // 4096-byte pages: the header occupies exactly page 0, the page-number
    // table starts at page 1. Truncate inside page 1 so header parsing succeeds
    // but the lazy locator read hits real EOF.
    let page_size = 4096u32;
    let big: Vec<u8> = (0..5000).map(|i| (i % 250) as u8).collect();
    let small: Vec<u8> = vec![1, 2, 3, 4, 5];
    let streams = vec![
        Logical::Absent,
        Logical::Empty,
        Logical::Data(big),
        Logical::Data(small),
    ];
    let model = build_model(page_size, Layout::Forward, streams);
    assert_eq!(model.locator_pages, vec![1]);

    let mut image = model.image.clone();
    let cut = page_size as usize + 128;
    assert!(cut < image.len());
    image.truncate(cut);
    Corruption {
        name: "page-number table truncated after the header",
        image,
        siblings: Vec::new(),
        victim: None,
    }
}

fn corrupt_truncated_payload(model: &Model) -> Corruption {
    // Forward layout puts the final stream's last payload page at the highest
    // physical page, so a suffix truncation cuts real payload bytes.
    let forward = build_model(model.page_size, Layout::Forward, model.streams.clone());
    let victim = forward
        .payload_pages
        .iter()
        .rposition(|p| p.as_ref().map_or(false, |pages| !pages.is_empty()))
        .expect("model has a payload stream");
    let pages = forward.payload_pages[victim].as_ref().unwrap();
    let last_page = *pages.last().unwrap();
    let data_len = match &forward.streams[victim] {
        Logical::Data(data) => data.len(),
        _ => unreachable!(),
    };
    let tail_len = data_len - (pages.len() - 1) * forward.page_size as usize;
    assert!(tail_len > 1);

    // Keep fewer bytes than the real final fragment needs: the cut reaches
    // into payload, not merely into the TAIL_FILL region.
    let cut = last_page as usize * forward.page_size as usize + tail_len / 2;
    let mut image = forward.image.clone();
    assert!(cut < image.len());
    image.truncate(cut);
    Corruption {
        name: "payload final page truncated inside stream bytes",
        image,
        siblings: forward
            .streams
            .iter()
            .enumerate()
            .filter(|(i, s)| *i != victim && matches!(s, Logical::Data(_)))
            .map(|(i, _)| i)
            .collect(),
        victim: Some(victim),
    }
}

#[test]
fn lazy_errors() {
    let base = build_model(512, Layout::Interleaved, seeded_streams(512));
    let mut corruptions = Vec::new();
    corruptions.push(corrupt_zero_page_reference(&base));
    corruptions.push(corrupt_out_of_range_reference(&base));
    corruptions.push(corrupt_truncated_directory(&base));
    corruptions.push(corrupt_truncated_locator());
    corruptions.push(corrupt_truncated_payload(&base));

    for corruption in corruptions {
        let name = corruption.name;
        let open_result = PDB::open(Cursor::new(corruption.image.clone()));
        let mut pdb = match open_result {
            Ok(pdb) => pdb,
            Err(e) => {
                // Open-time rejection is acceptable for truncation cases.
                assert!(
                    matches!(e, Error::IoError(_) | Error::UnrecognizedFileFormat),
                    "{}: {} rejected with unexpected error {:?}",
                    MARKER_UNEXPECTED_ERR,
                    name,
                    e
                );
                continue;
            }
        };

        match corruption.victim {
            Some(victim) => {
                let result = pdb.raw_stream(StreamIndex(victim as u16));
                assert_no_fabricated_content(name, &result);
                let err = result.expect_err("victim must error");
                match name {
                    "payload page number replaced by zero" => assert!(
                        matches!(err, Error::PageReferenceOutOfRange(0)),
                        "{}: {} gave {:?}",
                        MARKER_UNEXPECTED_ERR,
                        name,
                        err
                    ),
                    "payload page number beyond pages_used" => assert!(
                        matches!(err, Error::PageReferenceOutOfRange(n) if n > base.pages_used),
                        "{}: {} gave {:?}",
                        MARKER_UNEXPECTED_ERR,
                        name,
                        err
                    ),
                    _ => assert!(
                        matches!(err, Error::IoError(_) | Error::PageReferenceOutOfRange(_)),
                        "{}: {} gave {:?}",
                        MARKER_UNEXPECTED_ERR,
                        name,
                        err
                    ),
                }

                // The directory remains intact for reference-level corruption,
                // so legal sibling streams keep reading exact model bytes.
                for sibling in &corruption.siblings {
                    let expected = base.expected_payload(*sibling);
                    let stream = pdb
                        .raw_stream(StreamIndex(*sibling as u16))
                        .unwrap_or_else(|e| {
                            panic!(
                                "{}: sibling {} unreadable after \"{}\": {:?}",
                                MARKER_UNEXPECTED_ERR,
                                sibling,
                                name,
                                e
                            )
                        })
                        .expect("sibling stream exists");
                    assert_bytes_equal(
                        &base,
                        &format!("sibling stream[{}] after {}", sibling, name),
                        stream.as_slice(),
                        expected.unwrap(),
                    );
                }
            }
            None => {
                // Directory-level damage: no numbered stream may resolve to
                // content; every access has to surface the I/O/parse failure.
                for index in 0..base.streams.len() {
                    let result = pdb.raw_stream(StreamIndex(index as u16));
                    assert_no_fabricated_content(
                        &format!("{} stream[{}]", name, index),
                        &result,
                    );
                    match result {
                        Err(Error::IoError(e))
                            if matches!(e.kind(), ErrorKind::UnexpectedEof) => {}
                        Err(Error::IoError(_)) => {}
                        Err(Error::PageReferenceOutOfRange(_)) => {}
                        Err(Error::UnexpectedEof) => {}
                        Err(e) => panic!(
                            "{}: {} stream[{}] gave {:?}",
                            MARKER_UNEXPECTED_ERR,
                            name,
                            index,
                            e
                        ),
                        Ok(_) => unreachable!(),
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Test 4: recording Source and view lifetime
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct RecordedRequest {
    slices: Vec<SourceSlice>,
    total: usize,
    returned: Vec<u8>,
}

/// Spec-compliant in-memory source. It never returns a wrong-length view: any
/// request outside the backing buffer becomes a real `io::Error`, so every
/// error the tests observe originates in file/I/O behavior.
#[derive(Debug)]
struct RecordingSource {
    backing: Vec<u8>,
    log: Arc<Mutex<Vec<RecordedRequest>>>,
    views_served: Arc<AtomicUsize>,
    io_errors: Arc<AtomicUsize>,
}

struct RecordingView {
    bytes: Vec<u8>,
}

impl fmt::Debug for RecordingView {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "RecordingView({} bytes)", self.bytes.len())
    }
}

impl SourceView<'_> for RecordingView {
    fn as_slice(&self) -> &[u8] {
        &self.bytes
    }
}

impl RecordingSource {
    fn new(backing: Vec<u8>) -> (Self, Arc<Mutex<Vec<RecordedRequest>>>, Arc<AtomicUsize>, Arc<AtomicUsize>) {
        let log = Arc::new(Mutex::new(Vec::new()));
        let views_served = Arc::new(AtomicUsize::new(0));
        let io_errors = Arc::new(AtomicUsize::new(0));
        let source = RecordingSource {
            backing,
            log: log.clone(),
            views_served: views_served.clone(),
            io_errors: io_errors.clone(),
        };
        (source, log, views_served, io_errors)
    }
}

impl<'s> Source<'s> for RecordingSource {
    fn view(
        &mut self,
        slices: &[SourceSlice],
    ) -> std::io::Result<Box<dyn SourceView<'s>>> {
        let total: usize = slices.iter().map(|s| s.size).sum();
        let mut returned = Vec::with_capacity(total);
        for slice in slices {
            let start = slice.offset as usize;
            let end = start.checked_add(slice.size).ok_or_else(|| {
                self.io_errors.fetch_add(1, Ordering::SeqCst);
                std::io::Error::new(ErrorKind::InvalidInput, "slice offset overflow")
            })?;
            if end > self.backing.len() {
                self.io_errors.fetch_add(1, Ordering::SeqCst);
                return Err(std::io::Error::new(
                    ErrorKind::UnexpectedEof,
                    format!(
                        "requested {} backing bytes, have {}",
                        end,
                        self.backing.len()
                    ),
                ));
            }
            returned.extend_from_slice(&self.backing[start..end]);
        }
        // Source contract: the view length must equal the requested length.
        assert_eq!(returned.len(), total);
        self.log.lock().unwrap().push(RecordedRequest {
            slices: slices.to_vec(),
            total,
            returned: returned.clone(),
        });
        self.views_served.fetch_add(1, Ordering::SeqCst);
        Ok(Box::new(RecordingView { bytes: returned }))
    }
}

/// Independently predict the SourceSlices for an ordered list of physical
/// pages, applying the same two rules the MSF layer documents: consecutive page
/// numbers coalesce into one slice, then the list is truncated to the stream
/// length. This mirrors `PageList::push`/`truncate` without calling them, so
/// the expectation stays independent of the production code.
fn predict_slices(pages: &[u32], length: usize, page_size: usize) -> Vec<SourceSlice> {
    let mut coalesced: Vec<SourceSlice> = Vec::new();
    let mut last_page: Option<u32> = None;
    for &page in pages {
        let contiguous = last_page.map(|previous| previous + 1 == page).unwrap_or(false);
        if contiguous {
            coalesced.last_mut().unwrap().size += page_size;
        } else {
            coalesced.push(SourceSlice {
                offset: u64::from(page) * page_size as u64,
                size: page_size,
            });
        }
        last_page = Some(page);
    }

    let mut remaining = length;
    let mut truncated = Vec::new();
    for mut slice in coalesced {
        if remaining == 0 {
            break;
        }
        if slice.size > remaining {
            slice.size = remaining;
        }
        remaining -= slice.size;
        truncated.push(slice);
    }
    assert_eq!(remaining, 0, "predicted slices must cover the whole stream");
    truncated
}

#[test]
fn source_views() {
    let model = build_model(512, Layout::Interleaved, seeded_streams(512));
    let (source, log, views_served, io_errors) = RecordingSource::new(model.image.clone());
    let mut pdb = PDB::open(source).unwrap_or_else(|e| {
        panic!(
            "{}: open over recording source failed: {:?}",
            MARKER_UNEXPECTED_ERR, e
        )
    });

    let data_indices: Vec<usize> = model
        .streams
        .iter()
        .enumerate()
        .filter(|(_, s)| matches!(s, Logical::Data(_)))
        .map(|(i, _)| i)
        .collect();

    // Hold views across many subsequent requests.
    let retained_a = pdb
        .raw_stream(StreamIndex(data_indices[0] as u16))
        .expect("stream a")
        .expect("stream a exists");
    let expected_a = model.expected_payload(data_indices[0]).unwrap().to_vec();
    let retained_b = pdb
        .raw_stream(StreamIndex(data_indices[1] as u16))
        .expect("stream b")
        .expect("stream b exists");
    let expected_b = model.expected_payload(data_indices[1]).unwrap().to_vec();

    for &index in data_indices.iter().chain(data_indices.iter()).rev() {
        let stream = pdb
            .raw_stream(StreamIndex(index as u16))
            .expect("churn stream")
            .expect("churn stream exists");
        assert_bytes_equal(
            &model,
            &format!("churn stream[{}]", index),
            stream.as_slice(),
            model.expected_payload(index).unwrap(),
        );
    }

    let requests_after_churn = log.lock().unwrap().len();
    assert!(requests_after_churn >= data_indices.len() * 2 + 2);

    // Retained views survive all later source requests.
    assert_bytes_equal(&model, "retained view a", retained_a.as_slice(), &expected_a);
    assert_bytes_equal(&model, "retained view b", retained_b.as_slice(), &expected_b);

    let ps = model.page_size as usize;
    let recorded = log.lock().unwrap().clone();

    // The header request is always page 0 for 4096 bytes.
    assert!(recorded.iter().any(|r| r.slices == vec![SourceSlice { offset: 0, size: 4096 }]));

    // At least one request is genuinely fragmented into multiple slices.
    assert!(recorded.iter().any(|r| r.slices.len() >= 2));

    // Every request: fragment concatenation order and the source's own
    // invariant (returned length == requested length, bytes in request order).
    for request in &recorded {
        let mut reconstructed = Vec::with_capacity(request.total);
        for slice in &request.slices {
            let start = slice.offset as usize;
            reconstructed.extend_from_slice(&model.image[start..start + slice.size]);
        }
        assert_eq!(reconstructed.len(), request.total);
        if reconstructed != request.returned {
            panic!(
                "{}: fragments were not concatenated in request order\n slices={:?}",
                MARKER_SLICE,
                request.slices
            );
        }
        for slice in &request.slices {
            assert_eq!(
                slice.offset % ps as u64,
                0,
                "{}: slice offset not page aligned: {:?}",
                MARKER_SLICE,
                slice
            );
        }
        for slice in &request.slices[..request.slices.len() - 1] {
            assert_eq!(
                slice.size % ps,
                0,
                "{}: non-final fragment has partial size: {:?}",
                MARKER_SLICE,
                slice
            );
        }
    }

    // For each data stream, find its payload request and compare it against the
    // independently predicted fragments, including the truncated tail.
    for &index in &data_indices {
        let expected = model.expected_payload(index).unwrap();
        let want_pages = model.payload_pages[index].as_ref().unwrap();
        let want_slices = predict_slices(want_pages, expected.len(), ps);

        let request = recorded
            .iter()
            .find(|r| r.slices == want_slices.as_slice() && r.total == expected.len())
            .unwrap_or_else(|| {
                panic!(
                    "{}: no source request matched stream[{}] pages {:?}\n model:\n{}",
                    MARKER_SLICE,
                    index,
                    want_pages,
                    model.describe()
                )
            });

        // Equality of the slice vectors pins both physical page order and the
        // coalescing decisions.
        assert_eq!(
            request.slices, want_slices,
            "{}: fragment layout changed for stream[{}]",
            MARKER_SLICE, index
        );
        let tail = expected.len() % ps;
        assert!(tail != 0, "generated streams end mid-page by construction");
        // The final fragment carries the truncated tail. When the last pages are
        // physically contiguous it also retains the full pages of that run, so
        // pin its exact size via the predicted slices and its remainder here.
        assert_eq!(
            request.slices.last().unwrap().size,
            want_slices.last().unwrap().size
        );
        assert_eq!(request.slices.last().unwrap().size % ps, tail);
        assert_eq!(request.total, expected.len());
        assert_eq!(request.returned, expected);
    }

    assert_eq!(io_errors.load(Ordering::SeqCst), 0);
    assert!(views_served.load(Ordering::SeqCst) >= recorded.len());

    // A truncated backing store surfaces a real I/O error through the same
    // source; the source itself still never returns a wrong-length view. Use a
    // forward-layout model so the final stream's last payload page really is
    // the image tail, making the cut land inside live stream bytes.
    let tail_model = build_model(512, Layout::Forward, seeded_streams(512));
    let tail_index = tail_model
        .streams
        .iter()
        .rposition(|s| matches!(s, Logical::Data(_)))
        .unwrap();
    let tail_pages = tail_model.payload_pages[tail_index].as_ref().unwrap();
    assert_eq!(
        *tail_pages.last().unwrap(),
        tail_model.pages_used,
        "forward layout must place the final payload page at the image tail"
    );
    let tail_len = match &tail_model.streams[tail_index] {
        Logical::Data(data) => data.len() - (tail_pages.len() - 1) * ps,
        _ => unreachable!(),
    };
    assert!(tail_len > 1);
    // Cut inside the victim's live final fragment, not merely inside the
    // TAIL_FILL region that follows it.
    let cut = *tail_pages.last().unwrap() as usize * ps + tail_len / 2;
    assert!(cut < tail_model.image.len());
    let (cut_source, cut_log, _cut_views, cut_io_errors) =
        RecordingSource::new(tail_model.image[..cut].to_vec());
    let mut cut_pdb = PDB::open(cut_source).expect("header still readable");
    let err = cut_pdb
        .raw_stream(StreamIndex(tail_index as u16))
        .expect_err("truncated backing must fail");
    match err {
        Error::IoError(io) => assert_eq!(io.kind(), ErrorKind::UnexpectedEof),
        other => panic!(
            "{}: expected IoError, got {:?}",
            MARKER_UNEXPECTED_ERR, other
        ),
    }
    assert!(cut_io_errors.load(Ordering::SeqCst) >= 1);
    for request in cut_log.lock().unwrap().iter() {
        assert_eq!(
            request.returned.len(),
            request.total,
            "{}: source violated its own length contract",
            MARKER_VIEW
        );
    }
}
