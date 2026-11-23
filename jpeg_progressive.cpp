// jpeg_progressive.cpp - progressive (SOF2) JPEG scan output for jpge.
//
// jpge.cpp performs all front-end work (color conversion, subsampling, edge replication,
// DCT and quantization) using its normal code paths. In progressive mode the quantized
// coefficients of the whole image are stored and, once the caller submits the final NULL
// scanline, the fixed progressive scan script is written:
//
//   1. interleaved DC first scan              (Ss=0, Se=0,  Ah=0, Al=1)
//   2. one AC first scan per component        (Ss=1, Se=63, Ah=0, Al=1)
//   3. interleaved DC refinement scan         (Ss=0, Se=0,  Ah=1, Al=0)
//   4. one AC refinement scan per component   (Ss=1, Se=63, Ah=1, Al=0)
//
// HUFFMAN STATISTICS
// ------------------
// The baseline Huffman tables do not necessarily contain the symbols used by progressive
// scans (notably EOBRUN lengths and the AC refinement symbols). Each scan is therefore run
// twice: once in "gather" mode (counts symbols only), then a table is generated from the
// counts, and finally the real emission pass replays the exact same event order. Both
// passes share the same routines, so counted events match emitted events exactly.
//
// The scan entropy coding follows ITU-T T.81 G.1.2.2/G.1.2.3, including EOBRUN
// accumulation across adjacent blocks and AC refinement correction-bit buffering.

#include "jpge.h"
#include "jpge_progressive.h"

#include <stdlib.h>
#include <string.h>

namespace jpge {

	// JPEG marker codes used by the progressive writer.
	enum { M_SOI = 0xD8, M_EOI = 0xD9, M_SOS = 0xDA };

	enum { PROG_MAX_HUFF_SYMBOLS = 257, PROG_MAX_HUFF_CODESIZE = 32 };

	// Builds the scan script used by the encoder and the tests.
	int prog_make_scan_script(int num_components, prog::scan_desc* out)
	{
		int n = 0;
		// 1. Interleaved DC first scan, Al=1.
		out[n].num_comps = num_components;
		for (int c = 0; c < num_components; c++) out[n].comps[c] = c;
		out[n].ss = 0; out[n].se = 0; out[n].ah = 0; out[n].al = 1; n++;

		// 2. One AC first scan per component (Ss=1, Se=63, Al=1).
		for (int c = 0; c < num_components; c++)
		{
			out[n].num_comps = 1; out[n].comps[0] = c;
			out[n].ss = 1; out[n].se = 63; out[n].ah = 0; out[n].al = 1; n++;
		}

		// 3. Interleaved DC refinement scan (Ah=1, Al=0).
		out[n].num_comps = num_components;
		for (int c = 0; c < num_components; c++) out[n].comps[c] = c;
		out[n].ss = 0; out[n].se = 0; out[n].ah = 1; out[n].al = 0; n++;

		// 4. One AC refinement scan per component (Ah=1, Al=0).
		for (int c = 0; c < num_components; c++)
		{
			out[n].num_comps = 1; out[n].comps[0] = c;
			out[n].ss = 1; out[n].se = 63; out[n].ah = 1; out[n].al = 0; n++;
		}
		return n;
	}

	// Number of bits needed to represent the magnitude of v (v != 0).
	static inline int prog_nbits(int v)
	{
		int n = 0;
		while (v) { n++; v >>= 1; }
		return n;
	}

	// ---------------------------------------------------------------------------
	// Coefficient storage
	// ---------------------------------------------------------------------------

	bool jpeg_encoder::prog_open()
	{
		int max_hs = 1, max_vs = 1;
		for (int c = 0; c < m_num_components; c++)
		{
			if (m_comp_h_samp[c] > max_hs) max_hs = m_comp_h_samp[c];
			if (m_comp_v_samp[c] > max_vs) max_vs = m_comp_v_samp[c];
		}

		const int mcu_rows = m_image_y_mcu / m_mcu_y;
		size_t total_blocks = 0;
		for (int c = 0; c < m_num_components; c++)
		{
			// Padded grid exactly as walked by the interleaved MCU loop.
			m_prog_comp_w[c] = m_mcus_per_row * m_comp_h_samp[c];
			m_prog_comp_h[c] = mcu_rows * m_comp_v_samp[c];

			// True (unpadded) block grid, computed the way decoders do.
			m_prog_true_w[c] = ((m_image_x * m_comp_h_samp[c] + max_hs - 1) / max_hs + 7) / 8;
			m_prog_true_h[c] = ((m_image_y * m_comp_v_samp[c] + max_vs - 1) / max_vs + 7) / 8;
			if (m_prog_true_w[c] > m_prog_comp_w[c]) m_prog_true_w[c] = m_prog_comp_w[c];
			if (m_prog_true_h[c] > m_prog_comp_h[c]) m_prog_true_h[c] = m_prog_comp_h[c];

			// Overflow-safe block-product: pw * ph, then sum across components.
			const size_t w = (size_t)m_prog_comp_w[c], h = (size_t)m_prog_comp_h[c];
			const size_t max_blocks = (size_t)(~(size_t)0) / 64;   // 64 int16 per block
			if (w != 0 && h > max_blocks / w) return false;
			const size_t blocks = w * h;
			if (total_blocks > max_blocks - blocks) return false;
			m_prog_comp_offset[c] = total_blocks;
			total_blocks += blocks;
		}
		for (int c = m_num_components; c < 3; c++)
		{
			m_prog_comp_w[c] = m_prog_comp_h[c] = m_prog_true_w[c] = m_prog_true_h[c] = 0;
			m_prog_comp_offset[c] = total_blocks;
		}

		// Overflow-safe total byte size.
		size_t entries = total_blocks * 64;
		if (entries > (size_t)(~(size_t)0) / sizeof(int16)) return false;
		size_t bytes = entries * sizeof(int16);

		m_prog_coeffs = static_cast<int16*>(malloc(bytes));
		if (!m_prog_coeffs) return false;
		m_prog_coeffs_alloc = bytes;
		memset(m_prog_coeffs, 0, bytes);

		m_prog_mcu_y_ofs = 0;
		m_prog_gather = false;
		memset(m_prog_count, 0, sizeof(m_prog_count));
		m_prog_cur_slot = 0;
		m_prog_eobrun = 0;
		m_prog_be = 0;
		memset(m_prog_last_dc, 0, sizeof(m_prog_last_dc));
		return true;
	}

	const int16* jpeg_encoder::prog_block(int component_num, int block_x, int block_y) const
	{
		// Non-interleaved scans walk the true block grid, which can be smaller than the padded
		// interleaved grid. Clamp so odd image dimensions never read outside the allocation.
		if (block_x >= m_prog_comp_w[component_num]) block_x = m_prog_comp_w[component_num] - 1;
		if (block_y >= m_prog_comp_h[component_num]) block_y = m_prog_comp_h[component_num] - 1;
		if (block_x < 0) block_x = 0;
		if (block_y < 0) block_y = 0;
		size_t idx = m_prog_comp_offset[component_num]
			+ (size_t)block_y * (size_t)m_prog_comp_w[component_num] + (size_t)block_x;
		return &m_prog_coeffs[idx * 64];
	}

	void jpeg_encoder::prog_store_block(int component_num, int block_x, int block_y)
	{
		size_t idx = m_prog_comp_offset[component_num]
			+ (size_t)block_y * (size_t)m_prog_comp_w[component_num] + (size_t)block_x;
		memcpy(&m_prog_coeffs[idx * 64], m_coefficient_array, 64 * sizeof(int16));
	}

	// Mirrors process_mcu_row()'s block order but stores quantized coefficients.
	void jpeg_encoder::prog_process_mcu_row()
	{
		const int by = m_prog_mcu_y_ofs;
		if (m_num_components == 1)
		{
			for (int i = 0; i < m_mcus_per_row; i++)
			{
				load_block_8_8_grey(i); prog_transform_block(0);
				prog_store_block(0, i, by);
			}
		}
		else if ((m_comp_h_samp[0] == 1) && (m_comp_v_samp[0] == 1))
		{
			for (int i = 0; i < m_mcus_per_row; i++)
			{
				load_block_8_8(i, 0, 0); prog_transform_block(0); prog_store_block(0, i, by);
				load_block_8_8(i, 0, 1); prog_transform_block(1); prog_store_block(1, i, by);
				load_block_8_8(i, 0, 2); prog_transform_block(2); prog_store_block(2, i, by);
			}
		}
		else if ((m_comp_h_samp[0] == 2) && (m_comp_v_samp[0] == 1))
		{
			for (int i = 0; i < m_mcus_per_row; i++)
			{
				load_block_8_8(i * 2 + 0, 0, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 0, by);
				load_block_8_8(i * 2 + 1, 0, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 1, by);
				load_block_16_8_8(i, 1); prog_transform_block(1); prog_store_block(1, i, by);
				load_block_16_8_8(i, 2); prog_transform_block(2); prog_store_block(2, i, by);
			}
		}
		else if ((m_comp_h_samp[0] == 2) && (m_comp_v_samp[0] == 2))
		{
			for (int i = 0; i < m_mcus_per_row; i++)
			{
				load_block_8_8(i * 2 + 0, 0, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 0, by * 2 + 0);
				load_block_8_8(i * 2 + 1, 0, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 1, by * 2 + 0);
				load_block_8_8(i * 2 + 0, 1, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 0, by * 2 + 1);
				load_block_8_8(i * 2 + 1, 1, 0); prog_transform_block(0); prog_store_block(0, i * 2 + 1, by * 2 + 1);
				load_block_16_8(i, 1); prog_transform_block(1); prog_store_block(1, i, by);
				load_block_16_8(i, 2); prog_transform_block(2); prog_store_block(2, i, by);
			}
		}
		m_prog_mcu_y_ofs++;
	}

	// ---------------------------------------------------------------------------
	// Marker emission
	// ---------------------------------------------------------------------------

	void jpeg_encoder::prog_emit_sof()
	{
		emit_marker(0xC2);                            /* SOF2 - progressive */
		emit_word(3 * m_num_components + 2 + 5 + 1);
		emit_byte(8);                                 /* precision */
		emit_word(m_image_y);
		emit_word(m_image_x);
		emit_byte(m_num_components);
		for (int i = 0; i < m_num_components; i++)
		{
			emit_byte(static_cast<uint8>(i + 1));
			emit_byte(static_cast<uint8>((m_comp_h_samp[i] << 4) + m_comp_v_samp[i]));
			emit_byte(static_cast<uint8>(i > 0));
		}
	}

	void jpeg_encoder::prog_emit_sos(int num_comps, const int* comps, int ss, int se, int ah, int al)
	{
		emit_marker(M_SOS);
		emit_word(2 * num_comps + 2 + 1 + 3);
		emit_byte(static_cast<uint8>(num_comps));
		for (int i = 0; i < num_comps; i++)
		{
			const int c = comps[i];
			emit_byte(static_cast<uint8>(c + 1));
			const int tbl = (c == 0) ? 0 : 1;   // Y uses 0, chroma uses 1
			emit_byte(static_cast<uint8>((tbl << 4) + tbl));
		}
		emit_byte(static_cast<uint8>(ss));
		emit_byte(static_cast<uint8>(se));
		emit_byte(static_cast<uint8>((ah << 4) + al));
	}

	// ---------------------------------------------------------------------------
	// Scan entropy coding
	// ---------------------------------------------------------------------------

	void jpeg_encoder::prog_emit_symbol(int slot, int sym)
	{
		if (m_prog_gather)
			m_prog_count[slot][sym]++;
		else
			put_bits(m_huff_codes[slot][sym], m_huff_code_sizes[slot][sym]);
	}

	// Emit the pending EOBRUN event plus its buffered AC refinement correction bits.
	void jpeg_encoder::prog_emit_eobrun()
	{
		if (!m_prog_eobrun) return;
		const int r = prog_nbits((int)m_prog_eobrun) - 1;
		prog_emit_symbol(m_prog_cur_slot, r << 4);
		if (r && !m_prog_gather)
			put_bits((uint)m_prog_eobrun & ((1u << r) - 1), r);
		m_prog_eobrun = 0;
		prog_emit_buffered_bits(0, (int)m_prog_be);
		m_prog_be = 0;
	}

	// Emit n buffered correction bits starting at m_prog_bit_buf[ofs].
	void jpeg_encoder::prog_emit_buffered_bits(int ofs, int num_bits)
	{
		if (m_prog_gather) return;
		for (int i = 0; i < num_bits; i++)
			put_bits(m_prog_bit_buf[ofs + i] & 1, 1);
	}

	// Emit n correction bits from an arbitrary buffer (these are not Huffman symbols).
	void jpeg_encoder::prog_emit_corr_bits(const uint8* pBits, int num_bits)
	{
		if (m_prog_gather) return;
		for (int i = 0; i < num_bits; i++)
			put_bits(pBits[i] & 1, 1);
	}

	// Pad the scan's final partial byte with 1s (T.81 B.1.1.5) and flush. The bit buffer is
	// reset so the next scan starts on a byte boundary.
	void jpeg_encoder::prog_flush_scan_bits()
	{
		put_bits(0x7F, 7);
		flush_output_buffer();
		m_bit_buffer = 0;
		m_bits_in = 0;
	}

	// DC first scan (interleaved): successive approximation of the DC coefficient.
	void jpeg_encoder::prog_scan_dc_first(int num_comps, const int* comps, int al)
	{
		memset(m_prog_last_dc, 0, sizeof(m_prog_last_dc));   // DC predictors reset per scan
		const int mcu_cols = m_mcus_per_row;
		const int mcu_rows = m_image_y_mcu / m_mcu_y;
		for (int my = 0; my < mcu_rows; my++)
		{
			for (int mx = 0; mx < mcu_cols; mx++)
			{
				for (int ci = 0; ci < num_comps; ci++)
				{
					const int c = comps[ci];
					const int slot = (c == 0) ? 0 : 1;
					m_prog_cur_slot = slot;
					for (int y = 0; y < m_comp_v_samp[c]; y++)
						for (int x = 0; x < m_comp_h_samp[c]; x++)
						{
							const int16* p = prog_block(c, mx * m_comp_h_samp[c] + x, my * m_comp_v_samp[c] + y);
							const int dc = p[0] >> al;   // arithmetic shift = point transform
							const int diff = dc - m_prog_last_dc[c];
							m_prog_last_dc[c] = dc;
							const int neg = diff < 0;
							const int mag = neg ? -diff : diff;
							const int nb = prog_nbits(mag);
							prog_emit_symbol(slot, nb);
							if (nb && !m_prog_gather)
							{
								const int bits = neg ? (diff - 1) : diff;
								put_bits((uint)bits & ((1u << nb) - 1), nb);
							}
						}
				}
			}
		}
	}

	// DC refinement scan (interleaved): one bit per block, no Huffman symbols.
	void jpeg_encoder::prog_scan_dc_refine(int num_comps, const int* comps, int al)
	{
		const int mcu_cols = m_mcus_per_row;
		const int mcu_rows = m_image_y_mcu / m_mcu_y;
		for (int my = 0; my < mcu_rows; my++)
		{
			for (int mx = 0; mx < mcu_cols; mx++)
			{
				for (int ci = 0; ci < num_comps; ci++)
				{
					const int c = comps[ci];
					for (int y = 0; y < m_comp_v_samp[c]; y++)
						for (int x = 0; x < m_comp_h_samp[c]; x++)
						{
							const int16* p = prog_block(c, mx * m_comp_h_samp[c] + x, my * m_comp_v_samp[c] + y);
							if (!m_prog_gather)
								put_bits((uint)(p[0] >> al) & 1, 1);
						}
				}
			}
		}
	}

	// AC first scan for one component (non-interleaved). EOBRUN spans adjacent blocks.
	void jpeg_encoder::prog_scan_ac_first(int component_num, int ss, int se, int al)
	{
		const int bw = m_prog_true_w[component_num];
		const int bh = m_prog_true_h[component_num];
		m_prog_eobrun = 0;
		m_prog_be = 0;
		for (int by = 0; by < bh; by++)
		{
			for (int bx = 0; bx < bw; bx++)
			{
				const int16* p = prog_block(component_num, bx, by);
				int run = 0;
				for (int k = ss; k <= se; k++)
				{
					const int v = p[k];
					int mag = (v < 0) ? -v : v;
					mag >>= al;                       // point transform toward zero
					if (mag == 0) { run++; continue; }
					prog_emit_eobrun();               // flush pending EOB before a nonzero
					while (run >= 16) { prog_emit_symbol(m_prog_cur_slot, 0xF0); run -= 16; }
					const int nb = prog_nbits(mag);
					prog_emit_symbol(m_prog_cur_slot, (run << 4) + nb);
					if (!m_prog_gather)
					{
						// Negative values are coded as the one's complement of the magnitude.
						const uint mask = (1u << nb) - 1;
						const uint bits = (v < 0) ? ((uint)mag ^ mask) : (uint)mag;
						put_bits(bits & mask, nb);
					}
					run = 0;
				}
				if (run > 0)
				{
					m_prog_eobrun++;                  // this block ends in zeros
					if (m_prog_eobrun == 0x7FFF) prog_emit_eobrun();
				}
			}
		}
		prog_emit_eobrun();
	}

	// AC refinement scan for one component (T.81 G.1.2.3).
	void jpeg_encoder::prog_scan_ac_refine(int component_num, int ss, int se, int al)
	{
		const int bw = m_prog_true_w[component_num];
		const int bh = m_prog_true_h[component_num];
		m_prog_eobrun = 0;
		m_prog_be = 0;
		for (int by = 0; by < bh; by++)
		{
			for (int bx = 0; bx < bw; bx++)
			{
				const int16* p = prog_block(component_num, bx, by);
				int absvals[64];
				int eob = ss - 1;                     // index of last newly significant coef
				for (int k = ss; k <= se; k++)
				{
					const int v = p[k];
					const int a = (v < 0) ? -v : v;
					absvals[k] = a >> al;
					if (absvals[k] == 1) eob = k;
				}

				uint8 br_buf[64];
				int br = 0, run = 0, k = ss;
				while (k <= se)
				{
					const int mag = absvals[k];
					if (mag == 0) { run++; k++; continue; }

					// A nonzero coefficient (either already significant or newly significant)
					// is coming. Emit any required ZRL codes for the preceding zero run first,
					// flushing the correction bits collected so far with them. A run that
					// reaches past the final newly-significant coefficient instead folds into
					// the pending EOBRUN (the k <= eob test).
					while (run > 15 && k <= eob)
					{
						prog_emit_eobrun();
						prog_emit_symbol(m_prog_cur_slot, 0xF0);
						prog_emit_corr_bits(br_buf, br);
						br = 0;
						run -= 16;
					}

					if (mag > 1)
					{
						// Already significant in an earlier scan: it only contributes a
						// correction bit and does not terminate the current zero run.
						br_buf[br++] = (uint8)(mag & 1);
						k++;
						continue;
					}

					// Newly significant coefficient.
					prog_emit_eobrun();
					prog_emit_symbol(m_prog_cur_slot, (run << 4) + 1);
					if (!m_prog_gather)
						put_bits((uint)((p[k] < 0) ? 0 : 1), 1);
					prog_emit_corr_bits(br_buf, br);
					br = 0;
					run = 0;
					k++;
				}

				// Trailing zeros or significant coefs become an EOBRUN with its correction bits.
				if (run > 0 || br > 0)
				{
					m_prog_eobrun++;
					// Buffer is PROG_CORR_BITS+64 bytes; prior flushes keep be <= PROG_CORR_BITS-64.
					memcpy(m_prog_bit_buf + m_prog_be, br_buf, br);
					m_prog_be += br;
					if (m_prog_eobrun == 0x7FFF || m_prog_be > (PROG_CORR_BITS - 64))
						prog_emit_eobrun();
				}
			}
		}
		prog_emit_eobrun();
	}

	void jpeg_encoder::prog_run_scan(int num_comps, const int* comps, int ss, int se, int ah, int al)
	{
		if (ss == 0)
		{
			if (ah == 0) prog_scan_dc_first(num_comps, comps, al);
			else prog_scan_dc_refine(num_comps, comps, al);
		}
		else
		{
			const int c = comps[0];
			m_prog_cur_slot = (c == 0) ? 2 : 3;
			if (ah == 0) prog_scan_ac_first(c, ss, se, al);
			else prog_scan_ac_refine(c, ss, se, al);
		}
	}

	// ---------------------------------------------------------------------------
	// Huffman table construction from gathered statistics
	// ---------------------------------------------------------------------------

	struct prog_sym_freq { uint m_key, m_sym_index; };

	// Radix sorts prog_sym_freq[] by 32-bit key. Returns pointer to sorted values.
	static prog_sym_freq* prog_radix_sort_syms(uint num_syms, prog_sym_freq* pSyms0, prog_sym_freq* pSyms1)
	{
		const uint cMaxPasses = 4;
		uint32 hist[256 * cMaxPasses];
		memset(hist, 0, sizeof(hist));
		for (uint i = 0; i < num_syms; i++) { uint freq = pSyms0[i].m_key; hist[freq & 0xFF]++; hist[256 + ((freq >> 8) & 0xFF)]++; hist[256 * 2 + ((freq >> 16) & 0xFF)]++; hist[256 * 3 + ((freq >> 24) & 0xFF)]++; }
		prog_sym_freq* pCur_syms = pSyms0, * pNew_syms = pSyms1;
		uint total_passes = cMaxPasses; while ((total_passes > 1) && (num_syms == hist[(total_passes - 1) * 256])) total_passes--;
		for (uint pass_shift = 0, pass = 0; pass < total_passes; pass++, pass_shift += 8)
		{
			const uint32* pHist = &hist[pass << 8];
			uint offsets[256], cur_ofs = 0;
			for (uint i = 0; i < 256; i++) { offsets[i] = cur_ofs; cur_ofs += pHist[i]; }
			for (uint i = 0; i < num_syms; i++)
				pNew_syms[offsets[(pCur_syms[i].m_key >> pass_shift) & 0xFF]++] = pCur_syms[i];
			prog_sym_freq* t = pCur_syms; pCur_syms = pNew_syms; pNew_syms = t;
		}
		return pCur_syms;
	}

	// Moffat/Katajainen minimum redundancy code lengths.
	static void prog_calculate_minimum_redundancy(prog_sym_freq* A, int n)
	{
		int root, leaf, next, avbl, used, dpth;
		if (n == 0) return; else if (n == 1) { A[0].m_key = 1; return; }
		A[0].m_key += A[1].m_key; root = 0; leaf = 2;
		for (next = 1; next < n - 1; next++)
		{
			if (leaf >= n || A[root].m_key < A[leaf].m_key) { A[next].m_key = A[root].m_key; A[root++].m_key = next; }
			else A[next].m_key = A[leaf++].m_key;
			if (leaf >= n || (root < next && A[root].m_key < A[leaf].m_key)) { A[next].m_key += A[root].m_key; A[root++].m_key = next; }
			else A[next].m_key += A[leaf++].m_key;
		}
		A[n - 2].m_key = 0;
		for (next = n - 3; next >= 0; next--) A[next].m_key = A[A[next].m_key].m_key + 1;
		avbl = 1; used = dpth = 0; root = n - 2; next = n - 1;
		while (avbl > 0)
		{
			while (root >= 0 && (int)A[root].m_key == dpth) { used++; root--; }
			while (avbl > used) { A[next--].m_key = dpth; avbl--; }
			avbl = 2 * used; dpth++; used = 0;
		}
	}

	// Limits canonical Huffman code table's max code size.
	static void prog_huffman_enforce_max_code_size(int* pNum_codes, int code_list_len, int max_code_size)
	{
		if (code_list_len <= 1) return;
		for (int i = max_code_size + 1; i <= PROG_MAX_HUFF_CODESIZE; i++) pNum_codes[max_code_size] += pNum_codes[i];
		uint32 total = 0;
		for (int i = max_code_size; i > 0; i--)
			total += (((uint32)pNum_codes[i]) << (max_code_size - i));
		while (total != (1UL << max_code_size))
		{
			pNum_codes[max_code_size]--;
			for (int i = max_code_size - 1; i > 0; i--)
			{
				if (pNum_codes[i]) { pNum_codes[i]--; pNum_codes[i + 1] += 2; break; }
			}
			total--;
		}
	}

	// Builds m_huff_bits[slot]/m_huff_val[slot] from the gathered counts, guaranteeing no
	// all-ones code (via the dummy symbol) and a code size <= 16.
	static void prog_build_huffman_table(uint32* pCount, int table_len, uint8* bits, uint8* val)
	{
		prog_sym_freq syms0[PROG_MAX_HUFF_SYMBOLS], syms1[PROG_MAX_HUFF_SYMBOLS];
		syms0[0].m_key = 1; syms0[0].m_sym_index = 0;  // dummy symbol: avoids all-1s codes
		int num_used_syms = 1;
		for (int i = 0; i < table_len; i++)
			if (pCount[i]) { syms0[num_used_syms].m_key = pCount[i]; syms0[num_used_syms++].m_sym_index = i + 1; }
		prog_sym_freq* pSyms = prog_radix_sort_syms(num_used_syms, syms0, syms1);
		prog_calculate_minimum_redundancy(pSyms, num_used_syms);

		int num_codes[1 + PROG_MAX_HUFF_CODESIZE];
		memset(num_codes, 0, sizeof(num_codes));
		for (int i = 0; i < num_used_syms; i++) num_codes[pSyms[i].m_key]++;

		const uint CODE_SIZE_LIMIT = 16;
		prog_huffman_enforce_max_code_size(num_codes, num_used_syms, CODE_SIZE_LIMIT);

		memset(bits, 0, 17);
		for (int i = 1; i <= (int)CODE_SIZE_LIMIT; i++) bits[i] = static_cast<uint8>(num_codes[i]);

		// Remove the dummy symbol (must sit in the largest bucket).
		for (int i = CODE_SIZE_LIMIT; i >= 1; i--)
			if (bits[i]) { bits[i]--; break; }

		memset(val, 0, 256);
		for (int i = num_used_syms - 1; i >= 1; i--)
			val[num_used_syms - 1 - i] = static_cast<uint8>(pSyms[i].m_sym_index - 1);
	}

	// ---------------------------------------------------------------------------
	// Full progressive output
	// ---------------------------------------------------------------------------

	bool jpeg_encoder::prog_finish()
	{
		prog::scan_desc script[prog::MAX_SCANS];
		const int num_scans = prog_make_scan_script(m_num_components, script);

		// --- Statistics gather pass (same routines, counting only) ---
		memset(m_prog_count, 0, sizeof(m_prog_count));
		m_prog_gather = true;
		for (int s = 0; s < num_scans; s++)
		{
			const prog::scan_desc& d = script[s];
			prog_run_scan(d.num_comps, d.comps, d.ss, d.se, d.ah, d.al);
		}
		m_prog_gather = false;

		// --- Build the four Huffman tables from the counts ---
		// Slot layout matches m_huff_* arrays: 0 = DC luma, 1 = DC chroma, 2 = AC luma, 3 = AC chroma.
		// DC tables only ever carry symbols 0..11 (magnitude categories), AC tables all 256.
		for (int slot = 0; slot < 4; slot++)
			prog_build_huffman_table(m_prog_count[slot], (slot < 2) ? 12 : 256, m_huff_bits[slot], m_huff_val[slot]);
		for (int slot = 0; slot < 4; slot++)
			compute_huffman_table(m_huff_codes[slot], m_huff_code_sizes[slot], m_huff_bits[slot], m_huff_val[slot]);

		// --- Markers ---
		emit_marker(M_SOI);
		emit_jfif_app0();
		emit_dqt();
		prog_emit_sof();
		emit_dht(m_huff_bits[0], m_huff_val[0], 0, false);   // DC luma
		emit_dht(m_huff_bits[2], m_huff_val[2], 0, true);    // AC luma
		if (m_num_components == 3)
		{
			emit_dht(m_huff_bits[1], m_huff_val[1], 1, false); // DC chroma
			emit_dht(m_huff_bits[3], m_huff_val[3], 1, true);  // AC chroma
		}

		// --- Emission pass ---
		for (int s = 0; s < num_scans; s++)
		{
			const prog::scan_desc& d = script[s];
			prog_emit_sos(d.num_comps, d.comps, d.ss, d.se, d.ah, d.al);
			prog_run_scan(d.num_comps, d.comps, d.ss, d.se, d.ah, d.al);
			prog_flush_scan_bits();
		}

		emit_marker(M_EOI);
		m_pass_num = 2;
		return m_all_stream_writes_succeeded;
	}

} // namespace jpge
