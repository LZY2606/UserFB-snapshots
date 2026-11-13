////////////////////////////////////////////////////////////////////////////
// float_wire_packets.h
//
// Public-container material for the float wire regression:
//
//   - little-endian WavPack block / metadata chunk walking that shares the
//     independent model in both directions,
//   - in-memory public-API reader/writer glue,
//   - real encode/decode round trips through WavpackOpenFileOutput(),
//     WavpackPackSamples() and the public reading interface,
//   - hand-assembled new-format WVX material exercising the
//     min_shifted_zeros / max_shifted_ones elision rules.
//
// Nothing here searches for raw bytes: every flag observation is tied to a
// parsed metadata chunk of a specific block.
////////////////////////////////////////////////////////////////////////////

#ifndef FLOAT_WIRE_PACKETS_H
#define FLOAT_WIRE_PACKETS_H

#include <stddef.h>
#include <stdint.h>

#include "float_wire_model.h"
#include "wavpack.h"

#define FW_MAX_CHUNKS 16

typedef struct {
    unsigned char id;             // chunk id with LARGE/ODD markers removed
    int odd_size;                 // ID_ODD_SIZE was present
    int32_t byte_length;          // logical data length
    const unsigned char *data;    // points into the source block
} fw_chunk;

typedef struct {
    const unsigned char *block;
    size_t block_size;
    uint32_t ck_size;
    int16_t version;
    uint32_t total_samples, block_index, block_samples, flags, crc;
    fw_chunk chunks [FW_MAX_CHUNKS];
    int nchunks;
} fw_block_view;

// Decode one complete WavPack block, including every metadata chunk.
// Returns 0 on success, -1 on a structural problem.

int fw_block_parse (const unsigned char *data, size_t size, fw_block_view *view);
const fw_chunk *fw_block_find (const fw_block_view *view, unsigned char id);
const fw_chunk *fw_block_find_wvx (const fw_block_view *view);

// FLOAT_INFO (id 0x8) accessor. Returns 0 when the chunk is well-formed.

int fw_read_float_info (const fw_chunk *chunk, unsigned *float_flags,
    unsigned *float_shift, unsigned *max_exp, unsigned *norm_exp);

// Parsed WVX container: the 32-bit crc_x prefix, whether the new 10-bit
// header is present, the elision parameters and the payload bit array.

typedef struct {
    uint32_t crc_wvx;
    int new_format;
    unsigned min_shifted_zeros, max_shifted_ones;
    const unsigned char *payload;
    size_t payload_bytes;
} fw_wvx_view;

int fw_open_wvx (const fw_chunk *chunk, int is_float, fw_wvx_view *wvx);

// Growable byte blob used by the public block-output callback.

typedef struct {
    unsigned char *data;
    size_t size, cap;
} fw_blob;

void fw_blob_init (fw_blob *blob);
void fw_blob_free (fw_blob *blob);
int fw_blob_append (fw_blob *blob, const void *data, int32_t n);

// Encode interleaved binary32 bit patterns through the public writing API,
// using the requested fixed block size so that several blocks are produced.

int fw_pack_float_blob (const uint32_t *patterns, uint32_t frames, int num_channels,
    uint32_t channel_mask, uint32_t block_samples, fw_blob *blob);

// Decode a blob through the public reading interface in small batches,
// without OPEN_NORMALIZE. Returns the number of frames decoded and records
// the cumulative error count.

uint32_t fw_unpack_float_blob (const fw_blob *blob, uint32_t frames, int num_channels,
    uint32_t batch_frames, uint32_t *out_patterns, uint32_t *error_count);

// What one parsed block actually carries, observed through chunk ids.

typedef struct {
    uint32_t block_samples, flags, block_index;
    int has_float_info, has_wvx, has_new_wvx;
    unsigned float_flags, float_shift, max_exp;
} fw_block_report;

typedef struct {
    fw_block_report *reports;
    int report_cap;
    int nblocks;
    unsigned float_flag_union;     // every FLOAT_INFO flag seen anywhere
    int wvx_blocks, new_wvx_blocks, float_info_blocks;
} fw_container_report;

int fw_report_container (const fw_blob *blob, fw_container_report *report);
void fw_report_free (fw_container_report *report);

// Build a hand-assembled block that uses the new WVX bitstream format
// (ID_WVX_NEW_BITSTREAM) with explicit min_shifted_zeros / max_shifted_ones
// elision. The source patterns must satisfy the elided-zero guarantees; the
// function checks that itself. Returns 0 on success.

int fw_build_new_wvx_material (const uint32_t *patterns, int n,
    unsigned min_shifted_zeros, unsigned max_shifted_ones, fw_blob *blob,
    fw_bits *expected_sideband);

// Public in-memory reader, usable directly with WavpackOpenFileInputEx64().

extern WavpackStreamReader64 fw_memory_reader;

typedef struct {
    const unsigned char *data;
    size_t size, pos;
    int ungetc_flag;
    unsigned char ungetc_char;
} fw_memory_id;

void fw_memory_id_init (fw_memory_id *id, const fw_blob *blob);

// Little-endian helpers shared with the tests.

uint16_t fw_get_le16 (const unsigned char *p);
uint32_t fw_get_le32 (const unsigned char *p);
void fw_put_le16 (unsigned char *p, uint16_t v);
void fw_put_le32 (unsigned char *p, uint32_t v);

#endif
