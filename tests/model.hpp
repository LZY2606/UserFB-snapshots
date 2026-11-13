#pragma once
// Symbol-level video model for Smacker regression tests.
//
// A Model describes frames as 4x4 block symbols (literal 16-bit values or
// cache0/1/2 references), block types with runs, and per-frame palette
// operations. It is encoded into a fully legal SMK file with the writers
// in bitstream.hpp/huffman.hpp, and an independent simulator expands the
// same model into expected index pixels and 8-bit RGB palettes.
//
// The simulator deliberately re-expresses the Smacker block/palette
// semantics from the format description: it never parses the produced
// bitstream, never calls a libsmacker function, and is not derived from
// another copy of smacker.c.
#include <cstring>
#include <cstdint>
#include <map>
#include <vector>
#include <stdexcept>
#include <string>
#include <array>
#include "bitstream.hpp"
#include "huffman.hpp"

namespace taskmodel {

enum TreeId { TREE_MMAP = 0, TREE_MCLR = 1, TREE_FULL = 2, TREE_TYPE = 3 };

// Smacker 6-bit -> 8-bit colour expansion (format constant, identical to
// the table documented in the Smacker spec / libsmacker).
static const unsigned char PAL_MAP[64] = {
	0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
	0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
	0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
	0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
	0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
	0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
	0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
	0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
};

struct Symbol {
	enum Kind { LIT, CACHE } kind = LIT;
	unsigned int lit = 0; // valid when LIT
	int cache = 0;        // 0..2, valid when CACHE
	static Symbol literal(unsigned int v) { Symbol s; s.kind = LIT; s.lit = v; return s; }
	static Symbol cached(int slot) { Symbol s; s.kind = CACHE; s.cache = slot; return s; }
};

enum BlockKind { BK_MONO = 0, BK_FULL = 1, BK_VOID = 2, BK_SOLID = 3,
                 BK_DOUBLE = 4, BK_HALF = 5 };

struct BlockModel {
	BlockKind kind = BK_VOID;
	int run = 1;
	Symbol type_sym;                 // defaults to the literal type word
	bool type_sym_set = false;
	// MONO: one MCLR lookup packs s1 (high byte) / s2 (low byte),
	// one MMAP lookup supplies the selection mask
	Symbol clr;
	Symbol map_sym;
	unsigned int mask = 0;
	// SOLID
	int solid = 0;
	// FULL: 8 symbols, index 2*r is pair A (cols 3,2), 2*r+1 pair B (cols 1,0)
	Symbol full[8];
	// DOUBLE: index 0 paints rows 0-1, index 1 rows 2-3
	Symbol dbl[2];
	// HALF: indices 0,1 paint rows 0-1 (A,B), 2,3 paint rows 2-3
	Symbol half[4];

	static BlockModel mono(Symbol color, unsigned int mask, int run = 1) {
		BlockModel m; m.kind = BK_MONO; m.run = run; m.clr = color;
		m.mask = mask; m.map_sym = Symbol::literal(mask);
		return m;
	}
	static BlockModel make_full(const Symbol vals[8], int run = 1) {
		BlockModel m; m.kind = BK_FULL; m.run = run;
		for (int i = 0; i < 8; i ++) m.full[i] = vals[i];
		return m;
	}
	static BlockModel voidBlock(int run = 1) {
		BlockModel m; m.kind = BK_VOID; m.run = run; return m;
	}
	static BlockModel make_solid(int index, int run = 1) {
		BlockModel m; m.kind = BK_SOLID; m.run = run; m.solid = index; return m;
	}
	static BlockModel make_dbl(Symbol top, Symbol bottom, int run = 1) {
		BlockModel m; m.kind = BK_DOUBLE; m.run = run; m.dbl[0] = top; m.dbl[1] = bottom;
		return m;
	}
	static BlockModel make_half(const Symbol vals[4], int run = 1) {
		BlockModel m; m.kind = BK_HALF; m.run = run;
		for (int i = 0; i < 4; i ++) m.half[i] = vals[i];
		return m;
	}
};

enum PalOpKind { PAL_SET, PAL_SKIP, PAL_COPY };
struct PalOp {
	PalOpKind kind;
	int count = 1;          // SKIP/COPY
	int src = 0;            // COPY
	unsigned char rgb[3];   // SET, six-bit components
	static PalOp set(unsigned char r, unsigned char g, unsigned char b) {
		PalOp o; o.kind = PAL_SET; o.rgb[0] = r; o.rgb[1] = g; o.rgb[2] = b; return o;
	}
	static PalOp skip(int count) { PalOp o; o.kind = PAL_SKIP; o.count = count; return o; }
	static PalOp copy(int src, int count) { PalOp o; o.kind = PAL_COPY; o.count = count; o.src = src; return o; }
};

struct FrameModel {
	bool has_palette = false;
	std::vector<PalOp> pal_ops;
	std::vector<BlockModel> blocks;
};

struct ExpectedFrame {
	std::vector<unsigned char> indices;
	std::vector<unsigned char> rgb;       // 256*3, palette after this frame
	bool palette_changed;
};

struct Model {
	int w = 8, h = 8;
	unsigned char version = '4';
	Layout layout = Layout::BALANCED;
	// initial escape-marker (runtime cache seed) values per tree
	unsigned short markers[4][3];
	std::vector<FrameModel> frames;
};

// runtime per-tree cache simulator, mirroring smk_huff16_lookup's queue
struct CacheSim {
	unsigned short v[3];
	CacheSim() { reset(); }
	void reset() { std::memset(v, 0, sizeof(v)); }
	unsigned short resolve(unsigned short value) {
		if (v[0] != value) {
			v[2] = v[1];
			v[1] = v[0];
			v[0] = value;
		}
		return value;
	}
};

// inverse of the decoder's sizetable (run lengths 1..59 are directly mapped)
static int size_table_index(int run)
{
	static const int sizetable[64] = {
		1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
		17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
		33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
		49,50,51,52,53,54,55,56,57,58,59,128,256,512,1024,2048
	};
	for (int i = 0; i < 60; i ++)
		if (sizetable[i] == run)
			return i;
	throw std::runtime_error("run length not representable in test model");
}

struct BuiltFile {
	Bytes bytes;
	std::vector<ExpectedFrame> expected;
};

namespace detail {

struct TreePlan {
	std::set<unsigned int> leaves;       // literal values + marker values
	Huff16Table table;
};

// 16-bit value physically stored in the bitstream for one symbol: literals
// use their own value, cache references use that tree's escape marker.
static unsigned int leaf_value(const Model & m, int tree, const Symbol & s)
{
	if (s.kind == Symbol::LIT)
		return s.lit;
	return m.markers[tree][s.cache];
}

static void collect_tree_leaves(const Model & m, TreePlan plans[4])
{
	for (const FrameModel & f : m.frames) {
		for (const BlockModel & b : f.blocks) {
			unsigned int word;
			switch (b.kind) {
			case BK_MONO:
				plans[TREE_MCLR].leaves.insert(leaf_value(m, TREE_MCLR, b.clr));
				plans[TREE_MMAP].leaves.insert(leaf_value(m, TREE_MMAP, b.map_sym));
				word = 0u | (size_table_index(b.run) << 2);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			case BK_FULL:
				for (int i = 0; i < 8; i ++)
					plans[TREE_FULL].leaves.insert(leaf_value(m, TREE_FULL, b.full[i]));
				word = 1u | (size_table_index(b.run) << 2);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			case BK_VOID:
				word = 2u | (size_table_index(b.run) << 2);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			case BK_SOLID:
				word = 3u | (size_table_index(b.run) << 2) |
				       ((static_cast<unsigned int>(b.solid) & 0xFFu) << 8);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			case BK_DOUBLE:
				for (int i = 0; i < 2; i ++)
					plans[TREE_FULL].leaves.insert(leaf_value(m, TREE_FULL, b.dbl[i]));
				word = 1u | (size_table_index(b.run) << 2);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			case BK_HALF:
				for (int i = 0; i < 4; i ++)
					plans[TREE_FULL].leaves.insert(leaf_value(m, TREE_FULL, b.half[i]));
				word = 1u | (size_table_index(b.run) << 2);
				plans[TREE_TYPE].leaves.insert(leaf_value(m, TREE_TYPE,
				    b.type_sym_set ? b.type_sym : Symbol::literal(word)));
				break;
			}
		}
	}
	for (int t = 0; t < 4; t ++)
		for (int k = 0; k < 3; k ++)
			plans[t].leaves.insert(m.markers[t][k]);
}

static void emit_code(BitWriter & w, const std::map<unsigned int, Code> & enc, unsigned int value)
{
	auto it = enc.find(value);
	if (it == enc.end())
		throw std::runtime_error("value missing from huff16 tree");
	for (int b = it->second.len - 1; b >= 0; b --)
		w.bit((it->second.bits >> b) & 1u);
}

// encode palette ops into the chunk's palette region; returns payload bytes
// (everything after the leading count byte)
static Bytes encode_palette(const std::vector<PalOp> & ops)
{
	std::vector<PalOp> tuned = ops;

	// validate entry coverage and the copy ranges the library accepts
	int entries = 0;
	for (const PalOp & o : tuned) {
		switch (o.kind) {
		case PAL_SET: entries += 1; break;
		case PAL_SKIP:
			if (o.count < 1 || entries + o.count > 256)
				throw std::runtime_error("illegal palette skip");
			entries += o.count; break;
		case PAL_COPY:
			if (o.count < 1 || o.count > 64 || entries + o.count > 256 ||
			    o.src + o.count > 256 || (o.src < entries && o.src + o.count > entries))
				throw std::runtime_error("illegal palette copy range");
			entries += o.count; break;
		}
	}
	if (entries != 256)
		throw std::runtime_error("palette ops must fill all 256 entries");

	// the 0x80 skip field is 7 bits: break any skip larger than 128
	std::vector<PalOp> split;
	for (const PalOp & o : tuned) {
		if (o.kind != PAL_SKIP || o.count <= 128) {
			split.push_back(o);
			continue;
		}
		int left = o.count;
		while (left > 128) {
			split.push_back(PalOp::skip(128));
			left -= 128;
		}
		split.push_back(PalOp::skip(left));
	}
	tuned.swap(split);

	auto payload_len = [&tuned]() {
		int n = 0;
		for (const PalOp & o : tuned)
			n += (o.kind == PAL_SET ? 3 : o.kind == PAL_SKIP ? 1 : 2);
		return n;
	};
	// total palette region = 1 count byte + payload, must be a multiple of
	// 4; splitting one skip into two adds exactly one payload byte.
	int guard = 0;
	while ((1 + payload_len()) % 4 != 0) {
		bool did = false;
		for (size_t i = 0; i < tuned.size(); i ++) {
			if (tuned[i].kind == PAL_SKIP && tuned[i].count >= 2) {
				int c = tuned[i].count;
				tuned.erase(tuned.begin() + static_cast<ptrdiff_t>(i));
				tuned.insert(tuned.begin() + static_cast<ptrdiff_t>(i),
				             PalOp::skip(c - 1));
				tuned.insert(tuned.begin() + static_cast<ptrdiff_t>(i),
				             PalOp::skip(1));
				did = true;
				break;
			}
		}
		if (!did || ++guard > 8)
			throw std::runtime_error("cannot align palette region to 4 bytes");
	}

	Bytes out;
	for (const PalOp & o : tuned) {
		switch (o.kind) {
		case PAL_SET:
			for (int c = 0; c < 3; c ++) {
				if (o.rgb[c] > 0x3F)
					throw std::runtime_error("palette component exceeds 6 bits");
				out.push_back(o.rgb[c]);
			}
			break;
		case PAL_SKIP:
			out.push_back(static_cast<unsigned char>(0x80 | (o.count - 1)));
			break;
		case PAL_COPY:
			out.push_back(static_cast<unsigned char>(0x40 | (o.count - 1)));
			out.push_back(static_cast<unsigned char>(o.src & 0xFFu));
			break;
		}
	}
	return out;
}

// independent expected-image + palette expansion
struct Simulator {
	const Model & m;
	std::vector<unsigned char> frame;
	unsigned char palette[256][3];
	CacheSim cache[4];
	std::vector<ExpectedFrame> out;

	explicit Simulator(const Model & model) : m(model), frame(model.w * model.h, 0) {
		std::memset(palette, 0, sizeof(palette));
	}

	unsigned short lookup(int tree, const Symbol & s) {
		unsigned short value;
		if (s.kind == Symbol::LIT)
			value = static_cast<unsigned short>(s.lit);
		else
			value = cache[tree].v[s.cache];
		return cache[tree].resolve(value);
	}

	void apply_palette(const std::vector<PalOp> & ops) {
		unsigned char snap[256][3];
		std::memcpy(snap, palette, sizeof(snap)); // snapshot before any update
		int i = 0;
		for (const PalOp & o : ops) {
			switch (o.kind) {
			case PAL_SET:
				for (int c = 0; c < 3; c ++)
					palette[i][c] = PAL_MAP[o.rgb[c]];
				i ++;
				break;
			case PAL_SKIP:
				for (int q = 0; q < o.count; q ++)
					std::memcpy(palette[i + q], snap[i + q], 3);
				i += o.count;
				break;
			case PAL_COPY:
				for (int q = 0; q < o.count; q ++)
					std::memcpy(palette[i + q], snap[o.src + q], 3);
				i += o.count;
				break;
			}
		}
	}

	void paint(int row, int col, const BlockModel & b, BlockKind resolved) {
		const int skip = row * m.w + col;
		switch (resolved) {
		case BK_MONO: {
			unsigned short clr = lookup(TREE_MCLR, b.clr);
			// the selection mask is whatever the MMAP lookup resolved to
			unsigned short mapv = lookup(TREE_MMAP, b.map_sym);
			unsigned char s1 = static_cast<unsigned char>((clr >> 8) & 0xFFu);
			unsigned char s2 = static_cast<unsigned char>(clr & 0xFFu);
			for (int r = 0; r < 4; r ++)
				for (int c = 0; c < 4; c ++) {
					unsigned char pick = (mapv >> (r * 4 + c)) & 1u ? s1 : s2;
					frame[skip + r * m.w + c] = pick;
				}
			break;
		}
		case BK_FULL: {
			unsigned short v[8];
			for (int i = 0; i < 8; i ++) v[i] = lookup(TREE_FULL, b.full[i]);
			for (int r = 0; r < 4; r ++) {
				unsigned short a = v[2 * r];     // encoded first: cols 3,2
				unsigned short bp = v[2 * r + 1]; // encoded second: cols 1,0
				int p = skip + r * m.w;
				frame[p + 0] = static_cast<unsigned char>(bp & 0xFFu);
				frame[p + 1] = static_cast<unsigned char>((bp >> 8) & 0xFFu);
				frame[p + 2] = static_cast<unsigned char>(a & 0xFFu);
				frame[p + 3] = static_cast<unsigned char>((a >> 8) & 0xFFu);
			}
			break;
		}
		case BK_VOID:
			break; // previous indices remain in place
		case BK_SOLID: {
			for (int r = 0; r < 4; r ++)
				for (int c = 0; c < 4; c ++)
					frame[skip + r * m.w + c] = static_cast<unsigned char>(b.solid);
			break;
		}
		case BK_DOUBLE: {
			unsigned short d0 = lookup(TREE_FULL, b.dbl[0]);
			unsigned short d1 = lookup(TREE_FULL, b.dbl[1]);
			for (int r = 0; r < 2; r ++) {
				int p = skip + r * m.w;
				frame[p + 0] = frame[p + 1] = static_cast<unsigned char>(d0 & 0xFFu);
				frame[p + 2] = frame[p + 3] = static_cast<unsigned char>((d0 >> 8) & 0xFFu);
			}
			for (int r = 2; r < 4; r ++) {
				int p = skip + r * m.w;
				frame[p + 0] = frame[p + 1] = static_cast<unsigned char>(d1 & 0xFFu);
				frame[p + 2] = frame[p + 3] = static_cast<unsigned char>((d1 >> 8) & 0xFFu);
			}
			break;
		}
		case BK_HALF: {
			unsigned short v[4];
			for (int i = 0; i < 4; i ++) v[i] = lookup(TREE_FULL, b.half[i]);
			for (int r = 0; r < 2; r ++) {
				int p = skip + r * m.w;
				frame[p + 0] = static_cast<unsigned char>((v[1] & 0xFFu));
				frame[p + 1] = static_cast<unsigned char>((v[1] >> 8) & 0xFFu);
				frame[p + 2] = static_cast<unsigned char>((v[0] & 0xFFu));
				frame[p + 3] = static_cast<unsigned char>((v[0] >> 8) & 0xFFu);
			}
			for (int r = 2; r < 4; r ++) {
				int p = skip + r * m.w;
				frame[p + 0] = static_cast<unsigned char>((v[3] & 0xFFu));
				frame[p + 1] = static_cast<unsigned char>((v[3] >> 8) & 0xFFu);
				frame[p + 2] = static_cast<unsigned char>((v[2] & 0xFFu));
				frame[p + 3] = static_cast<unsigned char>((v[2] >> 8) & 0xFFu);
			}
			break;
		}
		}
	}

	void run() {
		const int total_blocks = (m.w / 4) * (m.h / 4);
		for (size_t fi = 0; fi < m.frames.size(); fi ++) {
			const FrameModel & f = m.frames[fi];
			bool changed = false;
			if (f.has_palette) {
				apply_palette(f.pal_ops);
				changed = true;
			}
			for (int t = 0; t < 4; t ++)
				cache[t].reset();

			int row = 0, col = 0, produced = 0;
			for (const BlockModel & b : f.blocks) {
				unsigned int base_type;
				switch (b.kind) {
				case BK_MONO: base_type = 0; break;
				case BK_FULL: case BK_DOUBLE: case BK_HALF: base_type = 1; break;
				case BK_VOID: base_type = 2; break;
				default: base_type = 3; break;
				}
				unsigned int word = base_type |
				    (static_cast<unsigned int>(size_table_index(b.run)) << 2);
				if (b.kind == BK_SOLID)
					word |= (static_cast<unsigned int>(b.solid) & 0xFFu) << 8;
				Symbol ts = b.type_sym_set ? b.type_sym : Symbol::literal(word);
				// the TYPE tree is consulted once per run-block
				unsigned short got = lookup(TREE_TYPE, ts);
				BlockKind resolved = b.kind;
				if (m.version == '4' && (got & 0x3u) == 1) {
					// extra selector bits are decided by the model block kind
					if (b.kind == BK_DOUBLE) resolved = BK_DOUBLE;
					else if (b.kind == BK_HALF) resolved = BK_HALF;
					else resolved = BK_FULL;
				}
				for (int rep = 0; rep < b.run; rep ++) {
					paint(row, col, b, resolved);
					produced ++;
					col += 4;
					if (col >= m.w) { col = 0; row += 4; }
				}
			}
			if (produced != total_blocks)
				throw std::runtime_error("frame block count does not fill image: frame " +
				                         std::to_string(fi));
			ExpectedFrame ef;
			ef.indices = frame;
			ef.rgb.assign(256 * 3, 0);
			for (int i = 0; i < 256; i ++)
				for (int c = 0; c < 3; c ++)
					ef.rgb[i * 3 + c] = palette[i][c];
			ef.palette_changed = changed;
			out.push_back(std::move(ef));
		}
	}
};

} // namespace detail

// Build the complete SMK file and expected decode results from a model.
static BuiltFile build_smk(const Model & m)
{
	if (m.w % 4 || m.h % 4 || m.w <= 0 || m.h <= 0)
		throw std::runtime_error("dimensions must be positive multiples of 4");
	if (m.version != '4' && m.version != '2')
		throw std::runtime_error("unsupported SMK version");
	for (int t = 0; t < 4; t ++) {
		for (int k = 0; k < 3; k ++)
			for (int q = k + 1; q < 3; q ++)
				if (m.markers[t][k] == m.markers[t][q])
					throw std::runtime_error("markers must be distinct per tree");
	}

	detail::TreePlan plans[4];
	detail::collect_tree_leaves(m, plans);

	// tree chunk
	BitWriter tw;
	unsigned long alloc[4];
	std::map<unsigned int, Code> encode[4];
	for (int t = 0; t < 4; t ++) {
		Huff16Spec spec;
		spec.leaves.assign(plans[t].leaves.begin(), plans[t].leaves.end());
		for (int k = 0; k < 3; k ++)
			spec.cache_marker[k] = m.markers[t][k];
		spec.layout = m.layout;
		plans[t].table = build_huff16(tw, spec);
		alloc[t] = plans[t].table.alloc_size;
		encode[t] = plans[t].table.encode;
	}
	tw.align_to_byte();
	const Bytes tree_bytes = tw.out;

	// frame chunks
	std::vector<Bytes> chunks;
	std::vector<unsigned char> frame_types;
	for (const FrameModel & f : m.frames) {
		Bytes chunk;
		if (f.has_palette) {
			Bytes payload = detail::encode_palette(f.pal_ops);
			unsigned long region = 1 + payload.size();
			if (region % 4)
				throw std::runtime_error("palette region not multiple of 4");
			chunk.push_back(static_cast<unsigned char>(region / 4));
			chunk.insert(chunk.end(), payload.begin(), payload.end());
		}
		BitWriter vw;
		for (const BlockModel & b : f.blocks) {
			unsigned int base_type;
			switch (b.kind) {
			case BK_MONO: base_type = 0; break;
			case BK_FULL: case BK_DOUBLE: case BK_HALF: base_type = 1; break;
			case BK_VOID: base_type = 2; break;
			default: base_type = 3; break;
			}
			unsigned int word = base_type |
			    (static_cast<unsigned int>(size_table_index(b.run)) << 2);
			if (b.kind == BK_SOLID)
				word |= (static_cast<unsigned int>(b.solid) & 0xFFu) << 8;
			Symbol ts = b.type_sym_set ? b.type_sym : Symbol::literal(word);
			// TYPE lookup and any v4 selector bits happen once per run-block
			detail::emit_code(vw, encode[TREE_TYPE], detail::leaf_value(m, TREE_TYPE, ts));
			if (m.version == '4' && b.kind != BK_MONO && b.kind != BK_VOID &&
			    b.kind != BK_SOLID) {
				if (b.kind == BK_DOUBLE) { vw.bit(1); }
				else if (b.kind == BK_HALF) { vw.bit(0); vw.bit(1); }
				else { vw.bit(0); vw.bit(0); }
			}
			for (int rep = 0; rep < b.run; rep ++) {
				switch (b.kind) {
				case BK_MONO:
					detail::emit_code(vw, encode[TREE_MCLR], detail::leaf_value(m, TREE_MCLR, b.clr));
					detail::emit_code(vw, encode[TREE_MMAP], detail::leaf_value(m, TREE_MMAP, b.map_sym));
					break;
				case BK_FULL:
					for (int i = 0; i < 8; i ++)
						detail::emit_code(vw, encode[TREE_FULL],
						    detail::leaf_value(m, TREE_FULL, b.full[i]));
					break;
				case BK_VOID: break;
				case BK_SOLID: break;
				case BK_DOUBLE:
					for (int i = 0; i < 2; i ++)
						detail::emit_code(vw, encode[TREE_FULL],
						    detail::leaf_value(m, TREE_FULL, b.dbl[i]));
					break;
				case BK_HALF:
					for (int i = 0; i < 4; i ++)
						detail::emit_code(vw, encode[TREE_FULL],
						    detail::leaf_value(m, TREE_FULL, b.half[i]));
					break;
				}
			}
		}
		vw.align_to_byte();
		chunk.insert(chunk.end(), vw.out.begin(), vw.out.end());
		if (chunk.size() % 4) {
			// chunk sizes are masked to 4-byte boundaries; pad the tail
			while (chunk.size() % 4)
				chunk.push_back(0);
		}
		chunks.push_back(chunk);
		frame_types.push_back(f.has_palette ? 0x01 : 0x00);
	}

	// assemble the 128-byte header
	Bytes hdr;
	hdr.push_back('S'); hdr.push_back('M'); hdr.push_back('K'); hdr.push_back(m.version);
	put_u32(hdr, m.w);
	put_u32(hdr, m.h);
	put_u32(hdr, m.frames.size());
	put_u32(hdr, 100);                 // milliseconds per frame
	put_u32(hdr, 0);                   // video flags: no ring, no y-scale
	for (int i = 0; i < 7; i ++) put_u32(hdr, 0); // audio max buffers
	put_u32(hdr, tree_bytes.size());
	for (int t = 0; t < 4; t ++) put_u32(hdr, alloc[t]);
	for (int i = 0; i < 7; i ++) put_u32(hdr, 0); // audio rate info
	put_u32(hdr, 0);                   // dummy
	for (size_t i = 0; i < chunks.size(); i ++) {
		unsigned long sz = chunks[i].size();
		if (i == 0) sz |= 0x01;       // first frame is a keyframe
		put_u32(hdr, sz);
	}
	for (unsigned char ft : frame_types)
		hdr.push_back(ft);
	// header layout: 104 fixed bytes, then 4 size bytes + 1 type byte per frame
	const size_t expected_hdr = 104 + 5 * chunks.size();
	if (hdr.size() != expected_hdr)
		throw std::runtime_error("header size mismatch: " + std::to_string(hdr.size()));

	BuiltFile bf;
	bf.bytes.reserve(hdr.size() + tree_bytes.size());
	bf.bytes.insert(bf.bytes.end(), hdr.begin(), hdr.end());
	bf.bytes.insert(bf.bytes.end(), tree_bytes.begin(), tree_bytes.end());
	for (const Bytes & c : chunks)
		bf.bytes.insert(bf.bytes.end(), c.begin(), c.end());

	detail::Simulator sim(m);
	sim.run();
	bf.expected = std::move(sim.out);
	return bf;
}

} // namespace taskmodel
