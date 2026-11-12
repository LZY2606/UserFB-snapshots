# Random-access bzip2: `Index` and `IndexedReader`

The sequential `Reader` decompresses a bzip2 archive from the beginning every
time, skipping earlier blocks purely by decoding them. For archives that hold
many independently addressed records, that makes every read pay for the whole
prefix. The indexed reader adds random access to **ordinary, unmodified bzip2
files**: the source is never rewritten and no recompression is required.

## API overview

```go
func BuildIndex(r io.ReaderAt, compressedSize int64) (*Index, error)
func (*Index) Size() int64
func (*Index) Blocks() []BlockInfo
func NewIndexedReader(r io.ReaderAt, idx *Index) (*IndexedReader, error)
func (*IndexedReader) ReadAt(p []byte, off int64) (int, error)
```

`BlockInfo` locates one block in both the compressed input and the
decompressed output:

| Field | Meaning |
| ----- | ------- |
| `Stream` | Zero-based stream number; an empty stream still consumes a number but yields no block. |
| `Level` | Compression level taken from the stream's `BZh` header. |
| `CompressedBitOffset`, `CompressedBitLength` | Half-open bit range of the block, measured from the MSB of the first input byte. It includes the block magic and header and ends right after the Huffman end-of-block symbol, excluding the next block and the stream footer. |
| `RawOffset`, `RawSize` | Half-open byte interval of the block's decompressed data in the concatenated output. |
| `CRC` | Block CRC-32 as stored in the block header. |

## Cost of building an index

`BuildIndex` performs **one sequential, fully verified decode** of the input.
Block boundaries are obtained by genuinely parsing the bzip2 format (prefix
codes, MTF/RLE2, BWT), never by scanning for magic values or estimating output
lengths from the `BZh` block size. A block's length is only known after its
RLE1 expansion, and the block is only accepted once its CRC matches; the index
is returned only after every concatenated stream's cumulative CRC and footer
have also been verified.

The index stores per-block metadata only, so its memory is proportional to the
number of blocks rather than the decompressed size. No fully expanded block is
retained and no temporary files are used. Build-time scratch space is the
existing block-transform workspace plus one fixed-size output window.

Because a level-1 transform block is capped at 100000 pre-RLE1 bytes while a
single repeated fill can expand to far more, the builder counts RLE1 runs
directly and advances the block CRC by run-length mathematics
(`O(log run length)`). Long fill values are never expanded into a giant slice
and never fed back through a byte-at-a-time CRC loop.

## Random reads

`NewIndexedReader` does not read the source; the index already contains every
block location. A `ReadAt` finds the blocks intersecting the requested byte
range and re-decodes only those blocks from their recorded bit intervals.

* Underlying reads are confined to each block's bit interval rounded out to
  whole bytes. Stream headers, stream footers, and neighboring blocks are never
  fetched. Bits of an adjacent block that share the first or last byte are
  skipped rather than decoded.
* Within one call, each intersected block's compressed interval is read at
  most once. A request that intersects no blocks reads nothing from the source.
* The whole block is decoded and its length and CRC are checked against the
  index **before** any of its bytes are written to the caller's buffer. If a
  block is corrupted after the index was built, none of that block's bytes are
  published, even when the damage lies beyond the requested offset. Bytes from
  earlier, already verified blocks in the same request are still counted in
  `n`. Corruption errors are distinct from the normal end-of-data `io.EOF`.
* `ReadAt` follows the standard `io.ReaderAt` length rules, including
  `(0, io.EOF)` for a non-empty request at or past the end and `(0, nil)`
  without any source access for a zero-length request at a non-negative offset.

Repeated fill values stay compressed during verification as well: a random
read validates an entire block by walking its RLE1 runs, materializing only the
requested fragment.

## Concurrency

An `Index` is immutable once built and may be shared by any number of
`IndexedReader` instances. An `IndexedReader` keeps no sequential cursor and no
mutable decode state between calls; each call obtains an independent decoding
workspace, so the same instance may serve concurrent `ReadAt` calls. The
underlying `io.ReaderAt` must itself be safe for concurrent use; that
guarantee is the caller's responsibility.

## Verification scope

The index is a point-in-time view. Random reads re-verify only the blocks a
request actually intersects; the reader does not detect later changes to
regions that no call has touched. A block that is read is always checked in
full, using the CRC and length recorded in the index, so a corrupted block can
never publish partially decoded data.
