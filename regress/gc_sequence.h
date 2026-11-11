/*
 * Binary input format for the GC state-sequence regression entry point.
 *
 * Wire format, version 1
 * ----------------------
 *
 *   Header:  'M' 'J' 'S' 'G'  version=0x01                 (5 bytes)
 *   Record:  opcode (1 byte) | payload_len (2 bytes, little endian)
 *            | payload (payload_len bytes)
 *
 * A record boundary is exactly the end of its declared payload.  The format
 * has no other framing: the next record starts immediately after the preceding
 * payload.  payload_len values are small and exact; a record whose size does
 * not match its opcode is invalid.
 *
 * Handles
 * -------
 *   root handle:  slot (1 byte) | generation (2 bytes, LE)
 *   node handle:  slot (1 byte) | generation (2 bytes, LE)
 *   value:        kind (1 byte), followed by:
 *                   GC_VALUE_NODE (0):      node handle (3 bytes)
 *                   GC_VALUE_UNDEFINED (1): nothing
 *                   GC_VALUE_NULL (2):      nothing
 *                   GC_VALUE_BOOLEAN (3):   0 or 1 (1 byte)
 *                   GC_VALUE_NUMBER (4):    uint32 LE (4 bytes)
 *
 * Operand limits: at most 16 external root slots, 64 logical nodes, 16 object
 * key ids ("k0".."k15"), 64 array indices, and strings up to 256 bytes.
 * Strings carry an explicit length; embedded NUL bytes are preserved.
 *
 * Records (opcode, payload):
 *   0x01 NEW_OBJECT   dst_root (u8)
 *   0x02 NEW_ARRAY    dst_root (u8)
 *   0x03 NEW_STRING   dst_root (u8) | len (u16 LE) | len bytes
 *   0x04 SET_PROP     obj_handle (3) | key_id (u8) | value
 *   0x05 DEL_PROP     obj_handle (3) | key_id (u8)
 *   0x06 ARRAY_SET    arr_handle (3) | index (u8) | value
 *   0x07 ARRAY_DEL    arr_handle (3) | index (u8)
 *   0x08 COPY_ROOT    src_root_handle (3) | dst_root_slot (u8)
 *   0x09 RELEASE_ROOT root_handle (3)
 *   0x0A GC_FULL      (empty)
 *   0x0B GC_MINOR     (empty)
 *
 * Invalid/truncated input rule (applies identically to every build):
 *   records are processed in order.  As soon as a record is truncated,
 *   malformed, has an unknown opcode, or fails model validation (bad/stale
 *   handle, wrong target kind, capacity limit, ...), that record is NOT
 *   executed and processing stops.  Every complete, valid record before it
 *   (the valid prefix) has already been executed and checked.  The invalid
 *   record and everything after it are ignored.  This is a controlled
 *   invalid-input result, never an out-of-bounds read and never a stale
 *   mjs_val_t handed back to the interpreter.
 *
 * After every executed record the real graph reachable from the live external
 * roots is compared with the model graph; a comparison failure is reported
 * with the record index, root generation, reachable path and the full input
 * bytes (as hex) by the driver.
 */

#ifndef GC_SEQUENCE_H_
#define GC_SEQUENCE_H_

#include <stddef.h>
#include <stdint.h>

#define GC_SEQUENCE_VERSION 1
#define GC_SEQUENCE_MAX_RECORD_PAYLOAD 4096
#define GC_SEQUENCE_DIAG_MAX 512

enum gc_sequence_status {
  GC_SEQ_STATUS_OK = 0,
  GC_SEQ_STATUS_INVALID_INPUT = 1,
  GC_SEQ_STATUS_MISMATCH = 2,
  GC_SEQ_STATUS_INTERNAL_ERROR = 3
};

struct gc_seq_result {
  int status;             /* enum gc_sequence_status */
  int invalid_prefix;     /* 1 when processing stopped on invalid input */
  size_t records_seen;    /* records whose boundary was reached */
  size_t ops_executed;    /* complete, valid records actually executed */
  size_t bad_record;      /* index of the invalid/mismatching record */
  size_t bad_op;          /* record index where the first deviation occurred */
  int root_slot;          /* root of the reporting path, -1 if unknown */
  unsigned root_gen;      /* generation of that external root */
  char path[GC_SEQUENCE_DIAG_MAX];
  char detail[GC_SEQUENCE_DIAG_MAX];
  uint64_t digest;        /* canonical digest of observed checkpoints */
};

/*
 * Execute and validate one input.  A fresh mJS instance is created and
 * destroyed on every call.  Returns result->status.
 */
int gc_sequence_run(const unsigned char *data, size_t len,
                    struct gc_seq_result *result);

/*
 * libFuzzer entry point.  Invalid input is a controlled no-op.  A real
 * content/identity deviation prints the diagnostic and aborts, so sanitizer
 * and assertion based detection works.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif /* GC_SEQUENCE_H_ */
