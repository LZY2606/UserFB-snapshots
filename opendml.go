package mjpeg

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"os"
)

// OpenDMLOptions configures a writer created with NewOpenDML.
type OpenDMLOptions struct {
	// SegmentBytes is the upper limit of the total byte count of each
	// complete RIFF segment: its 8-byte RIFF shell, headers, padding and
	// the final indexes are all counted. Zero means 1 GiB; a non-zero
	// value must be between 4096 and 1 GiB (inclusive).
	SegmentBytes int64

	// MaxSegments is the number of super index entries reserved in the
	// first segment, and thus the maximum number of RIFF segments the
	// video may consist of. Zero means 256; a non-zero value must be
	// between 1 and 65535.
	MaxSegments uint32
}

const (
	odmlDefaultSegmentBytes = 1 << 30 // 1 GiB
	odmlMinSegmentBytes     = 4096
	odmlMaxSegmentBytes     = 1 << 30 // 1 GiB
	odmlDefaultMaxSegments  = 256
	odmlMaxSegments         = 65535

	// odmlSeg1Fixed is the size of the first segment up to and including
	// the LIST movi shell, excluding the 16-byte reserved super index
	// entries.
	odmlSeg1Fixed = 280
	// odmlSegXFixed is the size of an empty RIFF AVIX segment shell
	// (RIFF header + LIST movi shell).
	odmlSegXFixed = 24
	// odmlIdx1Fixed is the shell of an (empty) idx1 chunk.
	odmlIdx1Fixed = 8
	// odmlIxFixed is the shell plus index header of an ix00 chunk.
	odmlIxFixed = 32
	// odmlIxEntry is the size of one standard index entry.
	odmlIxEntry = 8
	// odmlIdx1Entry is the size of one idx1 entry.
	odmlIdx1Entry = 16
	// odmlRecSize is the size of one temporary index record:
	// frame data offset (8) + frame size (4) + unused (4).
	odmlRecSize = 16
)

// odmlSegEntry describes one finalized segment in the super index.
type odmlSegEntry struct {
	ixOff  uint64 // file offset of the segment's ix00 chunk header
	size   uint32 // total size of the ix00 chunk, shell included
	frames uint32 // number of frames in the segment
}

// odmlWriter is an AviWriter producing an OpenDML (AVI 2.0) file:
// a leading RIFF AVI segment followed by RIFF AVIX segments as needed.
type odmlWriter struct {
	aviFile string
	idxFile string
	avif    *os.File
	idxf    *os.File

	width, height, fps int32
	segLimit           int64
	maxSeg             uint32

	err    error
	closed bool
	pos    int64

	// Positions of header fields patched at Close.
	avihTotalPos, strhLenPos, dmlhTotalPos int64
	indxCountPos, indxEntriesPos           int64

	// Current segment state.
	segStart      int64
	segIndex      uint32
	riffSizePos   int64
	moviSizePos   int64
	moviFourccPos int64
	moviDataPos   int64
	segFrames     uint32
	segRecStart   int64

	seg1Frames  uint32
	totalFrames uint64
	segments    []odmlSegEntry

	buf16 []byte
}

// NewOpenDML returns a new AviWriter that writes an OpenDML 1.02 extended
// AVI file: frames are recorded into a leading RIFF AVI segment, and when
// the next frame would not fit into the current segment (including the
// indexes needed to finalize it), the segment is closed and recording
// continues in a new RIFF AVIX segment of the same file.
//
// The Close() method of the AviWriter must be called to finalize the video
// file.
func NewOpenDML(aviFile string, width, height, fps int32, opts OpenDMLOptions) (awr AviWriter, err error) {
	if width <= 0 || height <= 0 || fps <= 0 {
		return nil, fmt.Errorf("mjpeg: width, height and fps must be positive, got %d, %d, %d", width, height, fps)
	}
	segLimit := opts.SegmentBytes
	if segLimit == 0 {
		segLimit = odmlDefaultSegmentBytes
	} else if segLimit < odmlMinSegmentBytes || segLimit > odmlMaxSegmentBytes {
		return nil, fmt.Errorf("mjpeg: SegmentBytes must be 0 or between %d and %d, got %d",
			odmlMinSegmentBytes, odmlMaxSegmentBytes, segLimit)
	}
	maxSeg := opts.MaxSegments
	if maxSeg == 0 {
		maxSeg = odmlDefaultMaxSegments
	} else if maxSeg > odmlMaxSegments {
		return nil, fmt.Errorf("mjpeg: MaxSegments must be 0 or between 1 and %d, got %d",
			odmlMaxSegments, maxSeg)
	}
	// The reserved first segment header and the finalization of an empty
	// segment must fit into the limit, checked before any file is created.
	if int64(odmlSeg1Fixed)+16*int64(maxSeg)+odmlIdx1Fixed > segLimit ||
		int64(odmlSegXFixed) > segLimit {
		return nil, fmt.Errorf("mjpeg: reserved headers and empty segment finalization (%d bytes) exceed SegmentBytes (%d)",
			int64(odmlSeg1Fixed)+16*int64(maxSeg)+odmlIdx1Fixed, segLimit)
	}

	aw := &odmlWriter{
		aviFile:  aviFile,
		idxFile:  aviFile + ".idx_",
		width:    width,
		height:   height,
		fps:      fps,
		segLimit: segLimit,
		maxSeg:   maxSeg,
		buf16:    make([]byte, 16),
	}

	defer func() {
		if err == nil {
			return
		}
		logErr := func(e error) {
			if e != nil {
				log.Printf("Error: %v\n", e)
			}
		}
		if aw.avif != nil {
			logErr(aw.avif.Close())
			logErr(os.Remove(aviFile))
		}
		if aw.idxf != nil {
			logErr(aw.idxf.Close())
			logErr(os.Remove(aw.idxFile))
		}
	}()

	aw.avif, err = os.Create(aviFile)
	if err != nil {
		return nil, err
	}
	aw.idxf, err = os.Create(aw.idxFile)
	if err != nil {
		return nil, err
	}

	aw.writeHeader()
	if aw.err != nil {
		return nil, aw.err
	}

	return aw, nil
}

// writeHeader writes the RIFF AVI shell, the hdrl list (avih, strl with
// the indx super index, odml/dmlh) and opens the LIST movi of the first
// segment.
func (aw *odmlWriter) writeHeader() {
	m := aw.maxSeg

	aw.segStart = 0
	aw.wStr("RIFF")
	aw.riffSizePos = aw.pos
	aw.wU32(0) // RIFF size, patched when the segment is finalized
	aw.wStr("AVI ")

	aw.wStr("LIST")
	aw.wU32(uint32(248 + 16*m)) // hdrl list size
	aw.wStr("hdrl")

	aw.wStr("avih")
	aw.wU32(0x38) // avih chunk size
	aw.wU32(uint32(1000000 / aw.fps))
	aw.wU32(0)    // dwMaxBytesPerSec
	aw.wU32(0)    // reserved
	aw.wU32(0x10) // dwFlags: AVIF_HASINDEX
	aw.avihTotalPos = aw.pos
	aw.wU32(0) // dwTotalFrames: frames of the first segment, patched at Close
	aw.wU32(0) // dwInitialFrames
	aw.wU32(1) // dwStreams
	aw.wU32(0) // dwSuggestedBufferSize
	aw.wU32(uint32(aw.width))
	aw.wU32(uint32(aw.height))
	aw.wU32(0)
	aw.wU32(0)
	aw.wU32(0)
	aw.wU32(0)

	aw.wStr("LIST")
	aw.wU32(uint32(148 + 16*m)) // strl list size
	aw.wStr("strl")

	aw.wStr("strh")
	aw.wU32(56)
	aw.wStr("vids")
	aw.wStr("MJPG")
	aw.wU32(0) // dwFlags
	aw.wU32(0) // wPriority, wLanguage
	aw.wU32(0) // dwInitialFrames
	aw.wU32(1) // dwScale
	aw.wU32(uint32(aw.fps))
	aw.wU32(0) // dwStart
	aw.strhLenPos = aw.pos
	aw.wU32(0) // dwLength: total frames of all segments, patched at Close
	aw.wU32(0) // dwSuggestedBufferSize
	aw.wU32(0xFFFFFFFF)
	aw.wU32(0) // dwSampleSize
	aw.wU16(0)
	aw.wU16(0)
	aw.wU16(0)
	aw.wU16(0)

	aw.wStr("strf")
	aw.wU32(40)
	aw.wU32(40) // biSize
	aw.wU32(uint32(aw.width))
	aw.wU32(uint32(aw.height))
	aw.wU16(1)  // biPlanes
	aw.wU16(24) // biBitCount
	aw.wStr("MJPG")
	aw.wU32(uint32(aw.width * aw.height * 3))
	aw.wU32(0)
	aw.wU32(0)
	aw.wU32(0)
	aw.wU32(0)

	// indx super index: entries point to the ix00 standard indexes
	// of the individual segments. Unused reserved entries stay zeroed.
	aw.wStr("indx")
	aw.wU32(uint32(24 + 16*m))
	aw.wU16(4) // wLongsPerEntry
	aw.wU8(0)  // bIndexSubType
	aw.wU8(0)  // bIndexType: AVI_INDEX_OF_INDEXES
	aw.indxCountPos = aw.pos
	aw.wU32(0) // nEntriesInUse, patched at Close
	aw.wStr("00dc")
	aw.wU64(0) // qwBaseOffset
	aw.wU32(0) // dwReserved3
	aw.indxEntriesPos = aw.pos
	aw.w(make([]byte, 16*m))

	aw.wStr("LIST")
	aw.wU32(16) // odml list size
	aw.wStr("odml")
	aw.wStr("dmlh")
	aw.wU32(4)
	aw.dmlhTotalPos = aw.pos
	aw.wU32(0) // dwTotalFrames: total frames of all segments, patched at Close

	aw.wStr("LIST")
	aw.moviSizePos = aw.pos
	aw.wU32(0) // movi list size, patched when the segment is finalized
	aw.moviFourccPos = aw.pos
	aw.wStr("movi")
	aw.moviDataPos = aw.pos
}

// startSegment opens a new RIFF AVIX segment with its own LIST movi.
func (aw *odmlWriter) startSegment() {
	aw.segIndex++
	aw.segStart = aw.pos
	aw.wStr("RIFF")
	aw.riffSizePos = aw.pos
	aw.wU32(0) // RIFF size, patched when the segment is finalized
	aw.wStr("AVIX")
	aw.wStr("LIST")
	aw.moviSizePos = aw.pos
	aw.wU32(0) // movi list size, patched when the segment is finalized
	aw.moviFourccPos = aw.pos
	aw.wStr("movi")
	aw.moviDataPos = aw.pos
	aw.segFrames = 0
	aw.segRecStart = int64(aw.totalFrames)
}

// fits reports whether a frame occupying frameChunk bytes (shell and
// padding included) can be added to the current segment such that the
// segment, once finalized with its indexes, still fits the limit.
func (aw *odmlWriter) fits(frameChunk int64) bool {
	n := int64(aw.segFrames) + 1
	finalize := int64(odmlIxFixed) + odmlIxEntry*n
	if aw.segIndex == 0 {
		finalize += odmlIdx1Fixed + odmlIdx1Entry*n
	}
	return aw.pos-aw.segStart+frameChunk+finalize <= aw.segLimit
}

// AddFrame implements AviWriter.AddFrame().
// ErrTooLarge is returned if the frame does not fit into the current
// segment and cannot start a new one (it would not fit even into an empty
// segment, or the super index capacity is exhausted). In that case nothing
// is written: the accepted frames and the current segment are left
// untouched, and a smaller frame or Close may follow.
func (aw *odmlWriter) AddFrame(jpegData []byte) error {
	if aw.err != nil {
		return aw.err
	}
	if aw.closed {
		return errImproperState
	}
	if len(jpegData) == 0 {
		return errors.New("mjpeg: empty JPEG frame")
	}
	if aw.totalFrames == math.MaxUint32 {
		return ErrTooLarge
	}

	frameChunk := int64(8) + int64(len(jpegData)) + int64(len(jpegData)&1)
	if !aw.fits(frameChunk) {
		switch {
		case aw.segFrames == 0:
			// Does not even fit into the current, empty segment.
			return ErrTooLarge
		case uint64(aw.segIndex)+2 > uint64(aw.maxSeg):
			// Super index capacity exhausted.
			return ErrTooLarge
		case int64(odmlSegXFixed)+frameChunk+odmlIxFixed+odmlIxEntry > aw.segLimit:
			// Would not fit into an empty AVIX segment either.
			return ErrTooLarge
		}
		aw.finalizeSegment()
		aw.startSegment()
		if aw.err != nil {
			return aw.err
		}
	}

	dataOff := aw.pos + 8
	aw.wStr("00dc")
	aw.wU32(uint32(len(jpegData)))
	aw.w(jpegData)
	if len(jpegData)&1 != 0 {
		aw.wU8(0) // pad to even size
	}
	aw.writeRec(dataOff, uint32(len(jpegData)))
	aw.segFrames++
	aw.totalFrames++

	return aw.err
}

// finalizeSegment writes the ix00 standard index of the current segment
// (if non-empty) and the idx1 chunk for the first segment, and patches the
// LIST movi and RIFF sizes.
func (aw *odmlWriter) finalizeSegment() {
	if aw.segFrames > 0 {
		ixOff := aw.pos
		aw.wStr("ix00")
		aw.wU32(uint32(24 + 8*aw.segFrames))
		aw.wU16(2) // wLongsPerEntry
		aw.wU8(0)  // bIndexSubType
		aw.wU8(1)  // bIndexType: AVI_INDEX_OF_CHUNKS
		aw.wU32(aw.segFrames)
		aw.wStr("00dc")
		aw.wU64(uint64(aw.moviDataPos)) // qwBaseOffset
		aw.wU32(0)                      // dwReserved3
		base := aw.moviDataPos
		aw.iterRecords(aw.segRecStart, aw.segFrames, func(dataOff int64, size uint32) {
			aw.wU32(uint32(dataOff - base))
			aw.wU32(size)
		})
		aw.segments = append(aw.segments, odmlSegEntry{
			ixOff:  uint64(ixOff),
			size:   uint32(odmlIxFixed + odmlIxEntry*aw.segFrames),
			frames: aw.segFrames,
		})
	}
	aw.patchU32(aw.moviSizePos, uint32(aw.pos-aw.moviSizePos-4))

	if aw.segIndex == 0 {
		aw.seg1Frames = aw.segFrames
		aw.wStr("idx1")
		aw.wU32(16 * aw.segFrames)
		moviPos := aw.moviFourccPos
		aw.iterRecords(aw.segRecStart, aw.segFrames, func(dataOff int64, size uint32) {
			aw.wStr("00dc")
			aw.wU32(0x10) // AVIIF_KEYFRAME
			aw.wU32(uint32(dataOff - 8 - moviPos))
			aw.wU32(size)
		})
	}
	aw.patchU32(aw.riffSizePos, uint32(aw.pos-aw.riffSizePos-4))
}

// Close implements AviWriter.Close().
func (aw *odmlWriter) Close() error {
	if aw.closed {
		return errImproperState
	}
	aw.closed = true
	defer func() {
		aw.avif.Close()
		aw.idxf.Close()
		os.Remove(aw.idxFile)
	}()
	if aw.err != nil {
		return aw.err
	}

	aw.finalizeSegment()
	if aw.err != nil {
		return aw.err
	}

	aw.patchU32(aw.avihTotalPos, uint32(aw.seg1Frames))
	aw.patchU32(aw.strhLenPos, uint32(aw.totalFrames))
	aw.patchU32(aw.dmlhTotalPos, uint32(aw.totalFrames))
	aw.patchU32(aw.indxCountPos, uint32(len(aw.segments)))

	end := aw.pos
	aw.seek(aw.indxEntriesPos)
	for _, e := range aw.segments {
		aw.wU64(e.ixOff)
		aw.wU32(e.size)
		aw.wU32(e.frames)
	}
	aw.seek(end)

	return aw.err
}

// writeRec appends one frame record to the temporary index file.
func (aw *odmlWriter) writeRec(dataOff int64, size uint32) {
	if aw.err != nil {
		return
	}
	binary.LittleEndian.PutUint64(aw.buf16, uint64(dataOff))
	binary.LittleEndian.PutUint32(aw.buf16[8:], size)
	// aw.buf16[12:16] stays zero
	_, aw.err = aw.idxf.Write(aw.buf16)
}

// iterRecords streams n temporary index records starting at record index
// start, invoking fn for each. fn may set aw.err.
func (aw *odmlWriter) iterRecords(start int64, n uint32, fn func(dataOff int64, size uint32)) {
	if aw.err != nil {
		return
	}
	if _, aw.err = aw.idxf.Seek(start*odmlRecSize, 0); aw.err != nil {
		return
	}
	remaining := int64(n) * odmlRecSize
	chunk := make([]byte, 64*1024)
	for remaining > 0 {
		want := int64(len(chunk))
		if remaining < want {
			want = remaining
		}
		if _, aw.err = io.ReadFull(aw.idxf, chunk[:want]); aw.err != nil {
			return
		}
		for off := int64(0); off < want; off += odmlRecSize {
			fn(int64(binary.LittleEndian.Uint64(chunk[off:])), binary.LittleEndian.Uint32(chunk[off+8:]))
			if aw.err != nil {
				return
			}
		}
		remaining -= want
	}
}

// w writes b to the AVI file and advances the tracked position.
func (aw *odmlWriter) w(b []byte) {
	if aw.err != nil {
		return
	}
	var n int
	n, aw.err = aw.avif.Write(b)
	aw.pos += int64(n)
}

// wStr writes a string to the AVI file.
func (aw *odmlWriter) wStr(s string) {
	if aw.err != nil {
		return
	}
	var n int
	n, aw.err = aw.avif.WriteString(s)
	aw.pos += int64(n)
}

// wU32 writes a 32-bit value to the AVI file.
func (aw *odmlWriter) wU32(v uint32) {
	binary.LittleEndian.PutUint32(aw.buf16, v)
	aw.w(aw.buf16[:4])
}

// wU16 writes a 16-bit value to the AVI file.
func (aw *odmlWriter) wU16(v uint16) {
	binary.LittleEndian.PutUint16(aw.buf16, v)
	aw.w(aw.buf16[:2])
}

// wU8 writes an 8-bit value to the AVI file.
func (aw *odmlWriter) wU8(v byte) {
	aw.buf16[0] = v
	aw.w(aw.buf16[:1])
}

// wU64 writes a 64-bit value to the AVI file.
func (aw *odmlWriter) wU64(v uint64) {
	binary.LittleEndian.PutUint64(aw.buf16, v)
	aw.w(aw.buf16[:8])
}

// seek sets the write position of the AVI file.
func (aw *odmlWriter) seek(pos int64) {
	if aw.err != nil {
		return
	}
	if _, aw.err = aw.avif.Seek(pos, 0); aw.err == nil {
		aw.pos = pos
	}
}

// patchU32 writes a 32-bit value at the given position, restoring the
// current write position afterwards.
func (aw *odmlWriter) patchU32(pos int64, v uint32) {
	if aw.err != nil {
		return
	}
	cur := aw.pos
	aw.seek(pos)
	aw.wU32(v)
	aw.seek(cur)
}
