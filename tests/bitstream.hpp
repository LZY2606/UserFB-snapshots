#pragma once
// Independent LSB-first bitstream writer used by the video-model test
// harness. Bits are packed into the low bit of each byte first, exactly
// matching the order libsmacker's smk_bs_read_1 consumes them.
#include <cstddef>
#include <vector>
#include <cstdint>

namespace taskmodel {

using Bytes = std::vector<unsigned char>;

struct BitWriter {
	Bytes out;
	size_t bit_pos = 0;

	void bit(int x) {
		if (bit_pos % 8 == 0)
			out.push_back(0);
		if (x & 1)
			out.back() |= static_cast<unsigned char>(1u << (bit_pos % 8));
		bit_pos ++;
	}

	// write the low k bits of x, least-significant bit first
	void bits(unsigned int x, int k) {
		for (int i = 0; i < k; i ++)
			bit((x >> i) & 1u);
	}

	// aligned 8-bit read path in the decoder (smk_bs_read_8) consumes a
	// whole byte low-bit-first; only used on byte boundaries here
	void byte(unsigned int x) {
		// huff8 leaf bytes are always read while the stream is byte aligned
		for (int i = 0; i < 8; i ++)
			bit((x >> i) & 1u);
	}

	void align_to_byte() {
		while (bit_pos % 8)
			bit(0);
	}

	void append(const Bytes & other) {
		// only valid while byte aligned
		for (unsigned char c : other)
			byte(c);
	}
};

// little-endian fixed-width integer writer for the file/chunk headers
inline void put_u8(Bytes & v, unsigned int x) {
	v.push_back(static_cast<unsigned char>(x & 0xFFu));
}

inline void put_u16(Bytes & v, unsigned int x) {
	v.push_back(static_cast<unsigned char>(x & 0xFFu));
	v.push_back(static_cast<unsigned char>((x >> 8) & 0xFFu));
}

inline void put_u32(Bytes & v, unsigned long x) {
	for (int i = 0; i < 4; i ++)
		v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu));
}

} // namespace taskmodel
