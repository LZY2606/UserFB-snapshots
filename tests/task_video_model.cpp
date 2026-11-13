// Smacker4 video-model regression tests.
//
// Each scenario builds a small, fully legal SMK file from an independent
// symbol-level model (tests/model.hpp), decodes it through the public
// libsmacker API (smk_open_memory / smk_enable_video / smk_first /
// smk_next / smk_get_video / smk_get_palette), and compares every index
// pixel and palette byte against the model's own expectation. The model
// never parses the bitstream it emits and never calls into smacker.c.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "registry.hpp"
#include "model.hpp"
#include "decoder.hpp"

using namespace taskmodel;

namespace {

const unsigned int W = 8;
const unsigned int H = 8;

void install_markers(Model & m)
{
	for (int t = 0; t < 4; t ++) {
		m.markers[t][0] = static_cast<unsigned short>(0xA000 + t * 0x10 + 1);
		m.markers[t][1] = static_cast<unsigned short>(0xA000 + t * 0x10 + 2);
		m.markers[t][2] = static_cast<unsigned short>(0xA000 + t * 0x10 + 3);
	}
}

Model base_model(Layout layout = Layout::BALANCED, int w = W, int h = H)
{
	Model m;
	m.w = w;
	m.h = h;
	m.version = '4';
	m.layout = layout;
	install_markers(m);
	return m;
}

// four solid blocks fill an 8x8 frame with palette indices 1..4
FrameModel solid_frame(int a = 1, int b = 2, int c = 3, int d = 4)
{
	FrameModel f;
	f.blocks.push_back(BlockModel::make_solid(a));
	f.blocks.push_back(BlockModel::make_solid(b));
	f.blocks.push_back(BlockModel::make_solid(c));
	f.blocks.push_back(BlockModel::make_solid(d));
	return f;
}

// palette whose first `set_count` entries are direct 6-bit sets and the
// remaining entries are preserved (skip); the large trailing skip is
// splittable, so payload alignment to a 4-byte region always succeeds.
std::vector<PalOp> set_then_skip(int set_count, unsigned char base,
                                 bool copy_old = false, int copy_src = 0,
                                 int copy_count = 0)
{
	std::vector<PalOp> ops;
	int i = 0;
	for (; i < set_count; i ++) {
		unsigned char r = static_cast<unsigned char>(base + i);
		unsigned char g = static_cast<unsigned char>((base * 2 + i) & 0x3F);
		unsigned char b = static_cast<unsigned char>((base * 3 + i) & 0x3F);
		ops.push_back(PalOp::set(r, g, b));
	}
	if (copy_old) {
		// forward, non-overlapping copy from the pre-update snapshot
		ops.push_back(PalOp::copy(copy_src, copy_count));
		i += copy_count;
	}
	ops.push_back(PalOp::skip(256 - i));
	return ops;
}

void expect_pixel(Registry & r, const BuiltFile & built, unsigned int seed,
                  unsigned long frame, int x, int y, unsigned char want)
{
	// every modelled frame is square, so width = sqrt(pixel count)
	const int npix = static_cast<int>(built.expected[frame].indices.size());
	int w = 0;
	while (w * w < npix) w ++;
	int blocks_per_row = w / 4;
	int block = (y / 4) * blocks_per_row + (x / 4);
	// locate through the same machinery as a content failure
	std::string msg = loc(seed, frame, block, x, y) + " explicit index expected=" +
	                  std::to_string(want) + " actual=" +
	                  std::to_string(built.expected[frame].indices[y * w + x]);
	r.check(built.expected[frame].indices[y * w + x] == want,
	        msg.c_str());
}

// ---- deterministic PRNG for the fixed-seed combination sequence ---------
struct Rng {
	uint32_t state;
	explicit Rng(uint32_t seed) : state(seed) {}
	uint32_t next() {
		state = state * 1664525u + 1013904223u;
		return state;
	}
	int below(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

std::vector<PalOp> random_palette(Rng & rng, bool allow_copy)
{
	std::vector<PalOp> ops;
	int i = 0;
	while (i < 16) {
		int choice = rng.below(allow_copy ? 3 : 2);
		if (choice == 0) {
			ops.push_back(PalOp::set(rng.below(64), rng.below(64), rng.below(64)));
			i ++;
		} else if (choice == 1) {
			int count = 1 + rng.below(3);
			if (i + count > 16) count = 16 - i;
			ops.push_back(PalOp::skip(count));
			i += count;
		} else {
			int count = 1 + rng.below(4);
			int src = i + 1 + rng.below(4); // always forward of the write head
			if (src + count > 256 || count > 64) { ops.push_back(PalOp::skip(1)); i ++; }
			else { ops.push_back(PalOp::copy(src, count)); i += count; }
		}
	}
	ops.push_back(PalOp::skip(256 - i)); // large, splittable tail
	return ops;
}

BlockModel random_block(Rng & rng, int position)
{
	static uint16_t lit_counter = 0x1100;
	auto fresh = []() { return Symbol::literal(lit_counter += 0x0101); };
	int kind = rng.below(6);
	switch (kind) {
	case 0: {
		Symbol color = (position % 3 == 0) ? Symbol::cached(0) : fresh();
		return BlockModel::mono(color, static_cast<unsigned int>(0x3C00u + position));
	}
	case 1: {
		Symbol vals[8];
		for (int k = 0; k < 8; k ++)
			vals[k] = (k == 3 && position % 2 == 0) ? Symbol::cached(1) : fresh();
		return BlockModel::make_full(vals);
	}
	case 2:
		return BlockModel::voidBlock();
	case 3:
		return BlockModel::make_solid(8 + rng.below(200));
	case 4:
		return BlockModel::make_dbl(fresh(), fresh());
	default: {
		Symbol vals[4] = { fresh(), fresh(), fresh(), fresh() };
		return BlockModel::make_half(vals);
	}
	}
}

} // namespace

void register_task_tests(Registry & r)
{
	const unsigned int seed = 0xC0FFEEu;

	// ======================== huffman group =============================
	r.add("huffman", "cache2_after_three_literals", [&]() {
		Model m = base_model();
		FrameModel f = solid_frame(9, 9, 9, 9);
		Symbol vals[8] = {
			Symbol::literal(0x0102), Symbol::literal(0x0304),
			Symbol::literal(0x0506), Symbol::cached(2), // resolves to 0x0102
			Symbol::literal(0x0708), Symbol::literal(0x090A),
			Symbol::literal(0x0B0C), Symbol::literal(0x0D0E)
		};
		f.blocks[0] = BlockModel::make_full(vals);
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// pair B of row 1: cache2 returned the first literal's bytes
		expect_pixel(r, built, seed, 0, 0, 1, 0x02);
		expect_pixel(r, built, seed, 0, 1, 1, 0x01);
	});

	r.add("huffman", "cache0_repeat_no_dequeue", [&]() {
		Model m = base_model();
		FrameModel f = solid_frame(9, 9, 9, 9);
		Symbol vals[8] = {
			Symbol::literal(0x1111), Symbol::literal(0x2222),
			Symbol::cached(0),               // repeats cache0, queue must not move
			Symbol::literal(0x3333),
			Symbol::cached(2),               // correct: oldest literal 0x1111
			Symbol::literal(0x4444), Symbol::literal(0x5555),
			Symbol::literal(0x6666)
		};
		f.blocks[0] = BlockModel::make_full(vals);
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// cache0 repeat at index 2 paints 0x2222; cache2 at index 4 paints 0x1111
		expect_pixel(r, built, seed, 0, 2, 1, 0x22);
		expect_pixel(r, built, seed, 0, 2, 2, 0x11);
		expect_pixel(r, built, seed, 0, 3, 2, 0x11);
	});

	r.add("huffman", "two_trees_interleaved", [&]() {
		Model m = base_model();
		FrameModel f;
		// block 1 seeds the MCLR cache with color 0x0A0B and the separate
		// MMAP cache with mask 0x000F
		BlockModel b1 = BlockModel::mono(Symbol::literal(0x0A0B), 0x000F);
		// block 2 references cache0 of *both* trees: independence means the
		// MMAP lookup still returns 0x000F, not the MCLR color 0x0A0B
		BlockModel b2 = BlockModel::mono(Symbol::cached(0), 0x000F);
		b2.map_sym = Symbol::cached(0);
		f.blocks.push_back(b1);
		f.blocks.push_back(b2);
		f.blocks.push_back(BlockModel::make_solid(6));
		f.blocks.push_back(BlockModel::make_solid(6));
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// mask 0x000F: low nibble picks s1 high byte 0x0A, rest s2 low 0x0B
		expect_pixel(r, built, seed, 0, 0, 0, 0x0A);  // block 0, bit0 set
		expect_pixel(r, built, seed, 0, 0, 1, 0x0B);  // block 0, bit4 clear -> (0,1)
		expect_pixel(r, built, seed, 0, 4, 0, 0x0A);  // block 1 same mask via MMAP cache0
		expect_pixel(r, built, seed, 0, 4, 1, 0x0B);
	});

	r.add("huffman", "zero_bit_constant_tree", [&]() {
		Model m = base_model(Layout::SKEWED);
		// identical MONO blocks: both MCLR and MMAP collapse to a single
		// leaf each (zero-bit constant trees); FULL is never consulted.
		FrameModel f;
		for (int k = 0; k < 4; k ++)
			f.blocks.push_back(BlockModel::mono(Symbol::literal(0x5A5B), 0x0003));
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		r.check(built.bytes.size() > 104, "constant-tree file produced");
		check_decode(r, built, seed, W, H);
		expect_pixel(r, built, seed, 0, 0, 0, 0x5A); // mask bit0 set -> s1 high byte
		expect_pixel(r, built, seed, 0, 1, 0, 0x5A); // bit1 set -> s1 high byte
		expect_pixel(r, built, seed, 0, 2, 0, 0x5B); // bit2 clear -> s2 low byte
	});

	// ========================= blocks group =============================
	r.add("blocks", "full_left_right_order", [&]() {
		Model m = base_model();
		FrameModel f = solid_frame(9, 9, 9, 9);
		Symbol vals[8];
		uint16_t cur = 0x1020;
		for (int k = 0; k < 8; k ++) { vals[k] = Symbol::literal(cur); cur += 0x0202; }
		f.blocks[0] = BlockModel::make_full(vals);
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// first pair: A=0x1020 fills cols 3(0x10),2(0x20); B=0x1222 cols 1,0
		expect_pixel(r, built, seed, 0, 0, 0, 0x22);
		expect_pixel(r, built, seed, 0, 1, 0, 0x12);
		expect_pixel(r, built, seed, 0, 2, 0, 0x20);
		expect_pixel(r, built, seed, 0, 3, 0, 0x10);
	});

	r.add("blocks", "double_half_asymmetric", [&]() {
		Model m = base_model(Layout::SKEWED);
		FrameModel f;
		f.blocks.push_back(BlockModel::make_dbl(Symbol::literal(0x1122),
		                                   Symbol::literal(0x3344)));
		Symbol hv[4] = {
			Symbol::literal(0x5566), Symbol::literal(0x7788),
			Symbol::literal(0x99AA), Symbol::literal(0xBBCC)
		};
		f.blocks.push_back(BlockModel::make_half(hv));
		f.blocks.push_back(BlockModel::make_solid(6));
		f.blocks.push_back(BlockModel::make_solid(7));
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// DOUBLE: duplicated columns and rows
		expect_pixel(r, built, seed, 0, 0, 0, 0x22);
		expect_pixel(r, built, seed, 0, 1, 0, 0x22);
		expect_pixel(r, built, seed, 0, 2, 0, 0x11);
		expect_pixel(r, built, seed, 0, 0, 1, 0x22); // row repeated
		expect_pixel(r, built, seed, 0, 0, 2, 0x44);
		expect_pixel(r, built, seed, 0, 3, 3, 0x33);
		// HALF: asymmetric pairs, rows duplicated; (3,1) is the high byte
		expect_pixel(r, built, seed, 0, 4, 0, 0x88);
		expect_pixel(r, built, seed, 0, 5, 0, 0x77);
		expect_pixel(r, built, seed, 0, 6, 0, 0x66);
		expect_pixel(r, built, seed, 0, 7, 0, 0x55);
		expect_pixel(r, built, seed, 0, 7, 1, 0x55); // repeated row, high byte
		expect_pixel(r, built, seed, 0, 4, 2, 0xCC);
		expect_pixel(r, built, seed, 0, 7, 3, 0x99);
	});

	r.add("blocks", "long_run_wraps_block_row", [&]() {
		Model m = base_model(Layout::BALANCED, 16, 16);
		FrameModel f1;
		f1.blocks.push_back(BlockModel::make_solid(0x07, 16)); // establish buffer
		FrameModel f2;
		f2.blocks.push_back(BlockModel::make_solid(0x11, 6));  // wraps row boundary
		f2.blocks.push_back(BlockModel::voidBlock(4));    // keeps frame 1
		f2.blocks.push_back(BlockModel::make_solid(0x22, 6));
		m.frames.push_back(f1);
		m.frames.push_back(f2);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, 16, 16);
		// block 4 starts the second block row after the wrap
		std::string a = loc(seed, 1, 4, 0, 4) + " run wrap";
		r.check(built.expected[1].indices[4 * 16 + 0] == 0x11, a.c_str());
		// void block 6 (row 1, column 2) retains the frame-1 index
		std::string b = loc(seed, 1, 6, 8, 4) + " void retention";
		r.check(built.expected[1].indices[4 * 16 + 8] == 0x07, b.c_str());
		// final run begins at block 10
		std::string c = loc(seed, 1, 10, 8, 8) + " post-wrap run";
		r.check(built.expected[1].indices[8 * 16 + 8] == 0x22, c.c_str());
		check_reopen_stable(r, built, seed);
	});

	r.add("blocks", "mono_mask_pixel_selection", [&]() {
		Model m = base_model();
		FrameModel f;
		f.blocks.push_back(BlockModel::mono(Symbol::literal(0xA1B2), 0x003C));
		f.blocks.push_back(BlockModel::make_solid(6));
		f.blocks.push_back(BlockModel::make_solid(6));
		f.blocks.push_back(BlockModel::make_solid(6));
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		expect_pixel(r, built, seed, 0, 0, 0, 0xB2); // bit0 clear -> s2 low byte
		expect_pixel(r, built, seed, 0, 2, 0, 0xA1); // bit2 set -> s1 high byte
		expect_pixel(r, built, seed, 0, 0, 1, 0xA1); // bit4 of 0x3C set -> s1 high
	});

	// ======================== palette group =============================
	r.add("palette", "void_block_with_palette_change", [&]() {
		Model m = base_model();
		m.frames.push_back(solid_frame(1, 2, 3, 4));
		FrameModel f2;
		f2.has_palette = true;
		f2.pal_ops = set_then_skip(8, 0x10);
		for (int k = 0; k < 4; k ++)
			f2.blocks.push_back(BlockModel::voidBlock());
		m.frames.push_back(f2);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// indices are untouched by VOID ...
		expect_pixel(r, built, seed, 1, 0, 0, 1); // block 0
		expect_pixel(r, built, seed, 1, 7, 4, 4); // block 3, second block row
		// ... but RGB follows the new palette
		r.check(built.expected[1].rgb[1 * 3 + 0] == PAL_MAP[0x11],
		        "void+palette: model expected new red channel");
		DecodeResult got = decode_file(built.bytes);
		r.check(got.palette[1][1 * 3 + 0] == PAL_MAP[0x11],
		        "void+palette: decoded red channel changed");
		r.check(got.indices[1][0] == 1, "void+palette: decoded index retained");
	});

	r.add("palette", "set_low_entry_then_copy_old_snapshot", [&]() {
		Model m = base_model(Layout::SKEWED);
		FrameModel f1;
		f1.has_palette = true;
		f1.pal_ops = set_then_skip(8, 0x08); // old entry 0 = (8,16,24)
		f1.blocks.push_back(BlockModel::make_solid(4));
		f1.blocks.push_back(BlockModel::make_solid(4));
		f1.blocks.push_back(BlockModel::make_solid(4));
		f1.blocks.push_back(BlockModel::make_solid(4));
		FrameModel f2;
		f2.has_palette = true;
		// overwrite low entry 0, then copy old entry 0,1 into entries 4,5
		f2.pal_ops = set_then_skip(4, 0x20, true, 0, 2);
		f2.blocks.push_back(BlockModel::voidBlock(4));
		m.frames.push_back(f1);
		m.frames.push_back(f2);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		DecodeResult got = decode_file(built.bytes);
		const auto * old_pal = got.palette[0].data(); // palette before frame 2
		const auto * new_pal = got.palette[1].data(); // palette after frame 2
		// concrete 6-bit expansion of the frame-1 direct-set entry 0: (8,16,24)
		r.check(old_pal[0] == PAL_MAP[0x08] && old_pal[1] == PAL_MAP[0x10] &&
		        old_pal[2] == PAL_MAP[0x18],
		        "palette snapshot: old entry0 holds its direct 6-bit color");
		// frame 2 overwrote entry 0 with a different color ...
		r.check(new_pal[0] != old_pal[0],
		        "palette snapshot: low entry0 was overwritten");
		// ... yet the copy into entries 4/5 reads the pre-update snapshot
		r.check(std::memcmp(new_pal + 4 * 3, old_pal + 0 * 3, 3) == 0,
		        "palette snapshot: entry4 copied OLD entry0, not live new entry0");
		r.check(std::memcmp(new_pal + 5 * 3, old_pal + 1 * 3, 3) == 0,
		        "palette snapshot: entry5 copied OLD entry1");
		// explicitly not a read-back of the new palette being written
		r.check(std::memcmp(new_pal + 4 * 3, new_pal + 0 * 3, 3) != 0,
		        "palette snapshot: copy source is not the live new palette");
	});

	r.add("palette", "direct_six_bit_and_skip_preserve", [&]() {
		Model m = base_model();
		FrameModel f;
		f.has_palette = true;
		std::vector<PalOp> ops;
		ops.push_back(PalOp::set(63, 0, 0));       // entry 0: full red
		ops.push_back(PalOp::skip(1));             // entry 1: stays black
		ops.push_back(PalOp::set(0, 63, 0));       // entry 2: full green
		ops.push_back(PalOp::skip(253));
		f.pal_ops = ops;
		f.blocks.push_back(BlockModel::make_solid(0));
		f.blocks.push_back(BlockModel::make_solid(1));
		f.blocks.push_back(BlockModel::make_solid(2));
		f.blocks.push_back(BlockModel::make_solid(0));
		m.frames.push_back(f);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		DecodeResult got = decode_file(built.bytes);
		r.check(got.palette[0][0] == 0xFF && got.palette[0][1] == 0 &&
		        got.palette[0][2] == 0, "6-bit direct red expands to 0xFF");
		r.check(got.palette[0][1 * 3 + 0] == 0 &&
		        got.palette[0][1 * 3 + 1] == 0, "skip preserves black entry");
		r.check(got.palette[0][2 * 3 + 1] == 0xFF, "6-bit direct green expands");
	});

	// ====================== crossframe group ============================
	r.add("crossframe", "first_ref_next_frame_is_zero_cache", [&]() {
		Model m = base_model();
		FrameModel f1 = solid_frame(9, 9, 9, 9);
		Symbol warm[8];
		for (int k = 0; k < 8; k ++) warm[k] = Symbol::literal(0x4000 + k * 0x0101);
		f1.blocks[0] = BlockModel::make_full(warm); // caches end non-zero
		FrameModel f2 = solid_frame(9, 9, 9, 9);
		Symbol cold[8];
		for (int k = 0; k < 8; k ++) cold[k] = Symbol::literal(0x7000 + k * 0x0101);
		cold[0] = Symbol::cached(0); // frame start reset the queue -> value 0
		f2.blocks[0] = BlockModel::make_full(cold);
		m.frames.push_back(f1);
		m.frames.push_back(f2);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		// first lookup of frame 2 is pair A of row 0: zeros at cols 2,3
		expect_pixel(r, built, seed, 1, 2, 0, 0x00);
		expect_pixel(r, built, seed, 1, 3, 0, 0x00);
	});

	r.add("crossframe", "void_indices_track_rgb_across_frames", [&]() {
		Model m = base_model(Layout::SKEWED);
		m.frames.push_back(solid_frame(5, 5, 5, 5));
		FrameModel f2;
		f2.has_palette = true;
		f2.pal_ops = set_then_skip(8, 0x18);
		f2.blocks.push_back(BlockModel::voidBlock(4));
		FrameModel f3;
		f3.has_palette = true;
		f3.pal_ops = set_then_skip(8, 0x30);
		f3.blocks.push_back(BlockModel::voidBlock(4));
		m.frames.push_back(f2);
		m.frames.push_back(f3);
		BuiltFile built = build_smk(m);
		check_decode(r, built, seed, W, H);
		DecodeResult got = decode_file(built.bytes);
		for (unsigned long fi = 0; fi < 3; fi ++)
			for (size_t k = 0; k < got.indices[fi].size(); k ++)
				r.check(got.indices[fi][k] == 5,
				        ("crossframe void index drift frame " +
				         std::to_string(fi)).c_str());
		r.check(got.palette[0][5 * 3 + 0] != got.palette[2][5 * 3 + 0],
		        "crossframe RGB changed while indices stayed");
		check_reopen_stable(r, built, seed);
	});

	r.add("crossframe", "fixed_seed_mixed_sequence", [&]() {
		Model m = base_model(Layout::SKEWED);
		Rng rng(0x12345678u);
		const int nframes = 5;
		for (int fi = 0; fi < nframes; fi ++) {
			FrameModel f;
			if (fi == 1 || fi == 3) {
				f.has_palette = true;
				f.pal_ops = random_palette(rng, true);
			}
			for (int b = 0; b < 4; b ++)
				f.blocks.push_back(random_block(rng, fi * 4 + b));
			m.frames.push_back(f);
		}
		BuiltFile built = build_smk(m);
		Bytes snapshot = built.bytes; // input must remain immutable
		check_decode(r, built, seed, W, H);
		check_reopen_stable(r, built, seed);
		r.check(built.bytes == snapshot, "input bytes mutated by decode");
		DecodeResult got = decode_file(built.bytes);
		r.check(got.indices.size() == static_cast<size_t>(nframes),
		        "seeded sequence frame count");
	});
}
