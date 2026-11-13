#pragma once
// Independent Smacker Huffman tree encoder.
//
// This implements the on-disk tree shapes consumed by libsmacker, but is
// written from the format description only: it builds canonical 8-bit
// prefix codes and explicit 16-bit tree shapes, then serialises them with
// the LSB-first BitWriter. Nothing in here calls into smacker.c.
#include <algorithm>
#include <map>
#include <vector>
#include <set>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include "bitstream.hpp"

namespace taskmodel {

// Layout policies for the 8-bit symbol trees: at least two distinct
// (symbol table, code length, tree shape) arrangements are exercised.
enum class Layout {
	// balanced: depths differ by at most one, canonical order
	BALANCED,
	// skewed: a comb tree, depths 1..m-1,m-1 (still a complete prefix code)
	SKEWED
};

struct Code {
	unsigned int bits;
	int len;
};

// prefix-code trie used only to serialise the preorder node/leaf stream
struct TrieNode {
	int symbol = -1;                 // >= 0 means leaf
	TrieNode * child[2] = {nullptr, nullptr};
	~TrieNode() { delete child[0]; delete child[1]; }
};

// assign canonical codes given per-symbol depths; symbols of equal depth
// are ordered by numeric value, codes increase in lock-step
static std::map<int, Code> canonical_codes(const std::map<int, int> & depth)
{
	std::map<int, Code> out;
	std::map<int, std::vector<int> > by_len;
	for (auto & kv : depth)
		by_len[kv.second].push_back(kv.first);

	unsigned int code = 0;
	int prev_len = -1;
	for (auto & kv : by_len) {
		int len = kv.first;
		if (prev_len < 0)
			code = 0;
		else
			code <<= (len - prev_len);
		for (int sym : kv.second) {
			out[sym] = {code, len};
			code ++;
		}
		prev_len = len;
	}
	return out;
}

// depth assignment for an explicit binary-tree shape policy
static std::map<int, int> huff8_depths(const std::vector<int> & symbols, Layout layout)
{
	std::map<int, int> depth;
	const size_t m = symbols.size();
	if (m == 1) {
		depth[symbols[0]] = 0; // zero-bit code, single leaf
		return depth;
	}
	if (layout == Layout::SKEWED) {
		// comb spine: leaf, (leaf, (leaf, ... )) for the first p symbols,
		// then a balanced subtree for the rest. Capping the spine length
		// keeps every code well under 32 bits while staying a distinctly
		// non-balanced, non-constant shape.
		const size_t p = std::min(m - 1, static_cast<size_t>(22));
		for (size_t i = 0; i < p; i ++)
			depth[symbols[i]] = static_cast<int>(i + 1);
		const size_t rest = m - p; // >= 1, balanced subtree rooted at depth p
		struct Frame { size_t lo, hi; int d; };
		std::vector<Frame> stack = {{p, m, static_cast<int>(p)}};
		while (!stack.empty()) {
			Frame f = stack.back();
			stack.pop_back();
			if (f.hi - f.lo == 1) {
				depth[symbols[f.lo]] = f.d;
				continue;
			}
			size_t mid = f.lo + (f.hi - f.lo) / 2;
			stack.push_back({f.lo, mid, f.d + 1});
			stack.push_back({mid, f.hi, f.d + 1});
		}
		(void)rest;
	} else {
		// balanced split on the sorted symbol list
		struct Frame { size_t lo, hi; int d; };
		std::vector<Frame> stack = {{0, m, 0}};
		while (!stack.empty()) {
			Frame f = stack.back();
			stack.pop_back();
			if (f.hi - f.lo == 1) {
				depth[symbols[f.lo]] = f.d;
				continue;
			}
			size_t mid = f.lo + (f.hi - f.lo) / 2;
			stack.push_back({f.lo, mid, f.d + 1});
			stack.push_back({mid, f.hi, f.d + 1});
		}
	}
	return depth;
}

// insert a canonical code (MSB of the code string first) into a trie
static void trie_insert(TrieNode * root, const Code & c, int symbol)
{
	TrieNode * n = root;
	for (int b = c.len - 1; b >= 0; b --) {
		int side = (c.bits >> b) & 1u;
		if (!n->child[side])
			n->child[side] = new TrieNode();
		n = n->child[side];
	}
	if (n->symbol >= 0)
		throw std::runtime_error("duplicate code in trie");
	n->symbol = symbol;
}

// preorder serialisation expected by _smk_huff8_build_rec:
// branch -> set bit, left subtree, right subtree; leaf -> clear bit + byte
static void huff8_emit_node(const TrieNode * n, BitWriter & w)
{
	if (n->symbol >= 0) {
		w.bit(0);
		w.byte(static_cast<unsigned int>(n->symbol));
		return;
	}
	w.bit(1);
	huff8_emit_node(n->child[0], w);
	huff8_emit_node(n->child[1], w);
}

struct Huff8Table {
	std::map<int, Code> codes;
	// serialise a full huff8 tree: presence bit, nodes, terminating 0 bit
	void emit(BitWriter & w) const {
		TrieNode root;
		for (auto & kv : codes)
			trie_insert(&root, kv.second, kv.first);
		w.bit(1); // tree present
		huff8_emit_node(&root, w);
		w.bit(0); // required terminator
	}
};

static Huff8Table build_huff8(const std::set<int> & symbol_set, Layout layout)
{
	if (symbol_set.empty())
		throw std::runtime_error("huff8 tree needs at least one symbol");
	std::vector<int> symbols(symbol_set.begin(), symbol_set.end());
	std::map<int, int> depth = huff8_depths(symbols, layout);

	// Kraft equality check (floating point so deep skewed codes do not
	// overflow an integer shift): a complete prefix code sums to 1.0
	if (!(depth.size() == 1 && depth.begin()->second == 0)) {
		double kraft = 0.0;
		for (auto & kv : depth)
			kraft += ldexp(1.0, -kv.second);
		if (kraft < 0.999999 || kraft > 1.000001)
			throw std::runtime_error("canonical code fails Kraft equality");
	}

	Huff8Table t;
	t.codes = canonical_codes(depth);
	return t;
}

// One 16-bit huff tree together with the encode map used by the frame
// writer. Leaf values equal to one of cache_marker[k] are exactly the
// values libsmacker converts into escape leaves at build time.
struct Huff16Table {
	unsigned long alloc_size = 0;
	std::map<unsigned int, Code> encode; // leaf value -> tree path bits
};

struct Huff16Spec {
	std::vector<unsigned int> leaves;     // distinct 16-bit leaf values
	unsigned short cache_marker[3];       // stored escape markers, distinct
	Layout layout = Layout::BALANCED;
};

// balanced full binary tree over the leaf list; returns the path code of
// each leaf in preorder-emission order
static std::map<unsigned int, Code> balanced16(const std::vector<unsigned int> & leaves)
{
	std::map<unsigned int, Code> out;
	struct Frame { size_t lo, hi; Code prefix; };
	std::vector<Frame> stack = {{0, leaves.size(), {0, 0}}};
	while (!stack.empty()) {
		Frame f = stack.back();
		stack.pop_back();
		if (f.hi - f.lo == 1) {
			out[leaves[f.lo]] = f.prefix;
			continue;
		}
		size_t mid = f.lo + (f.hi - f.lo) / 2;
		Code left = f.prefix, right = f.prefix;
		left.bits <<= 1; left.len ++;                 // 0 -> left
		right.bits = (right.bits << 1) | 1u; right.len ++; // 1 -> right
		stack.push_back({f.lo, mid, left});
		stack.push_back({mid, f.hi, right});
	}
	return out;
}

static void huff16_emit_path(BitWriter & w, const Code & c)
{
	for (int b = c.len - 1; b >= 0; b --)
		w.bit((c.bits >> b) & 1u);
}

// serialise one complete huff16 tree chunk (presence bit, low/hi huff8
// trees, six cache bytes, node/leaf preorder, terminating zero bit)
static Huff16Table build_huff16(BitWriter & w, const Huff16Spec & spec)
{
	if (spec.leaves.empty())
		throw std::runtime_error("huff16 tree needs at least one leaf");
	std::set<unsigned int> uniq(spec.leaves.begin(), spec.leaves.end());
	if (uniq.size() != spec.leaves.size())
		throw std::runtime_error("huff16 leaves must be distinct");
	if (spec.cache_marker[0] == spec.cache_marker[1] ||
		spec.cache_marker[1] == spec.cache_marker[2] ||
		spec.cache_marker[0] == spec.cache_marker[2])
		throw std::runtime_error("cache markers must be distinct");

	std::set<int> low_syms, hi_syms;
	for (unsigned int v : spec.leaves) {
		low_syms.insert(v & 0xFFu);
		hi_syms.insert((v >> 8) & 0xFFu);
	}
	Huff8Table low8 = build_huff8(low_syms, spec.layout);
	Huff8Table hi8 = build_huff8(hi_syms, spec.layout);

	Huff16Table table;
	table.encode = balanced16(spec.leaves);
	const size_t nodes = 2 * spec.leaves.size() - 1;
	table.alloc_size = 12 + static_cast<unsigned long>(nodes) * 4;

	w.bit(1); // huff16 tree present
	low8.emit(w);
	hi8.emit(w);
	for (int k = 0; k < 3; k ++) {
		w.byte(spec.cache_marker[k] & 0xFFu);
		w.byte((spec.cache_marker[k] >> 8) & 0xFFu);
	}

	// emit the balanced shape as preorder nodes
	struct Emit {
		bool branch;
		size_t lo, hi;
	};
	std::vector<Emit> work;
	work.push_back({true, 0, spec.leaves.size()});
	while (!work.empty()) {
		Emit e = work.back();
		work.pop_back();
		if (e.hi - e.lo == 1) {
			unsigned int v = spec.leaves[e.lo];
			w.bit(0); // leaf
			Code lc = low8.codes.find(v & 0xFFu)->second;
			Code hc = hi8.codes.find((v >> 8) & 0xFFu)->second;
			huff16_emit_path(w, lc);
			huff16_emit_path(w, hc);
			continue;
		}
		size_t mid = e.lo + (e.hi - e.lo) / 2;
		w.bit(1); // branch
		// right pushed first so left is emitted first
		work.push_back({true, mid, e.hi});
		work.push_back({true, e.lo, mid});
	}
	w.bit(0); // required terminator
	return table;
}

} // namespace taskmodel
