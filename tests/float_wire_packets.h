////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//             Public container material and chunk traversal               //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float_wire_packets.h
//
// Container-level helpers for the float wire regression. Everything here is
// expressed through the public WavPack API and the documented on-disk chunk
// layout: in-memory block output/input, little-endian WavpackHeader parsing,
// metadata chunk traversal, and a block rebuilder used to substitute
// handcrafted new-format WVX material into an otherwise real block. The
// independent float model in float_wire_model.[ch] is shared by both
// directions and never sees any of these container details.

#ifndef FLOAT_WIRE_PACKETS_H
#define FLOAT_WIRE_PACKETS_H

#include <stddef.h>
#include <stdint.h>

#include "wavpack.h"

// One metadata chunk inside a block. The pointer aliases the block buffer, so
// it stays valid as long as the block buffer does.

typedef struct {
    unsigned char        id;            // chunk id with ID_LARGE/ID_ODD stripped
    unsigned char        raw_id;        // original id byte on disk
    int                  byte_length;   // decoded data length (odd flag removed)
    const unsigned char *data;          // pointer to the chunk payload
    const unsigned char *raw;           // pointer to the on-disk id byte
} fwp_chunk;

#define FWP_MAX_CHUNKS 24

typedef struct {
    const unsigned char *block;
    size_t               size;
    char                 ckID [4];
    uint32_t             ckSize;
    uint32_t             flags;
    uint32_t             block_samples;
    int64_t              block_index;
    int                  nchunks;
    fwp_chunk            chunks [FWP_MAX_CHUNKS];
} fwp_block;

// Parse one complete Wavpack block (starting at "wvpk"). Returns 0 on success.
// Chunk ids have the ID_LARGE/ID_ODD_SIZE control bits stripped.

int fwp_parse_block (const unsigned char *block, size_t size, fwp_block *out);

// Locate the first chunk with the given stripped id (e.g. ID_FLOAT_INFO,
// ID_WVX_BITSTREAM, ID_WVX_NEW_BITSTREAM), or NULL if absent.

const fwp_chunk *fwp_find_chunk (const fwp_block *b, unsigned char id);

// Walk every complete "wvpk" block in a concatenated in-memory file, calling
// cb() with each parsed block. Stops on a parse error or when cb returns 0.

int fwp_for_each_block (const unsigned char *data, size_t size,
                        int (*cb) (const fwp_block *block, void *opaque), void *opaque);

// Growable sink for the public WavpackBlockOutput callback.

typedef struct {
    unsigned char *data;
    size_t         len, cap;
    int            failed;
} fwp_sink;

void fwp_sink_init (fwp_sink *sink, unsigned char *storage, size_t cap);
int  fwp_sink_block (void *id, void *data, int32_t n);

// Seekable in-memory reader for WavpackOpenFileInputEx64().

typedef struct {
    const unsigned char *data;
    size_t               size;
    int64_t              pos;
    int                  pushed;
    int                  has_pushback;
} fwp_memory;

void fwp_memory_init (fwp_memory *mem, const unsigned char *data, size_t size);
extern WavpackStreamReader64 fwp_memory_reader;

// Chunk accumulator used to rebuild blocks with handcrafted WVX material. The
// 32-byte header is copied from a real public-encoder block so the WV
// entropy/decorrelation chunks stay valid; only the WVX chunk is substituted,
// after which the block checksum is recomputed.

typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         len;
} fwp_builder;

void fwp_builder_init_from (fwp_builder *b, unsigned char *storage, size_t cap,
                            const unsigned char *src_block);
void fwp_builder_add_chunk (fwp_builder *b, unsigned char id,
                            const void *data, int byte_length);
void fwp_builder_copy_chunk (fwp_builder *b, const fwp_chunk *chunk);

// Append a WVX chunk. The payload holds the sideband bits after the 4-byte
// CRC and (for the new format) the 10-bit min/max header; valid_bits excludes
// padding. Physical padding is added internally to satisfy Bitstream reads
// without changing the error handling of truncated streams.

void fwp_builder_wvx_legacy (fwp_builder *b, uint32_t crc_x,
                             const unsigned char *sideband, int valid_bits);
void fwp_builder_wvx_new (fwp_builder *b, uint32_t crc_x,
                          int min_shifted_zeros, int max_shifted_ones,
                          const unsigned char *sideband, int valid_bits);

// Copy every chunk of "src" except the WVX chunks, which are replaced by the
// supplied handcrafted payload, then write the little-endian header (with the
// original audio CRC, because the integers are untouched) and block checksum.
// Returns the total block length, or 0 on overflow.

size_t fwp_rebuild_with_wvx (const fwp_block *src, fwp_builder *b,
                             unsigned char wvx_id,
                             int min_shifted_zeros, int max_shifted_ones,
                             const unsigned char *sideband, int valid_bits,
                             uint32_t crc_x);

// Write the little-endian header and block checksum. The audio CRC in the
// copied header is left untouched. Returns the total block length, or 0.

size_t fwp_builder_finish (fwp_builder *b);

#endif
