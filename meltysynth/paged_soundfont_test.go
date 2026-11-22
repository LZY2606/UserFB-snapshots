package meltysynth

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"testing"

	"github.com/sinshu/go-meltysynth/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Minimal SF2 fixture builder (standard library only).
// ---------------------------------------------------------------------------

func sfWord(b []byte, x uint16) []byte { return binary.LittleEndian.AppendUint16(b, x) }
func sfLong(b []byte, x uint32) []byte { return binary.LittleEndian.AppendUint32(b, x) }

func sfName(b []byte, s string) []byte {
	for i := 0; i < 20; i++ {
		if i < len(s) {
			b = append(b, s[i])
		} else {
			b = append(b, 0)
		}
	}
	return b
}

func sfChunk(b []byte, id string, payload []byte) []byte {
	b = append(b, []byte(id)...)
	b = sfLong(b, uint32(len(payload)))
	b = append(b, payload...)
	if len(payload)%2 != 0 {
		b = append(b, 0)
	}
	return b
}

// testSample describes one shdr record and its instrument region.
type testSample struct {
	name       string
	start      uint32
	end        uint32
	startLoop  uint32
	endLoop    uint32
	mode       uint16
	keyLo      byte
	keyHi      byte
	compressed bool
}

// fontLayout is the physical layout of a generated SF2. The smpl payload
// itself is never held here: callers either append waveBytes or serve the
// range virtually.
type fontLayout struct {
	head       []byte // everything up to and including the smpl chunk header
	tail       []byte // everything after the smpl payload
	smplOffset int64
	smplBytes  int64
	frameCount int
}

// buildFontLayout assembles the metadata of a 16-bit PCM SF2 with one
// preset, one instrument and one region per sample. No smpl payload is
// allocated, so the layout cost is independent of the pool's length.
func buildFontLayout(samples []testSample, frameCount int, includeSM24 bool) fontLayout {
	var info []byte
	info = sfChunk(info, "ifil", sfWord(sfWord(nil, 2), 1))
	info = sfChunk(info, "isng", []byte("EMU8000\x00"))
	info = sfChunk(info, "INAM", []byte("PagedFont\x00"))

	// phdr: one preset plus the EOP terminator (38 bytes each).
	var phdr []byte
	phdr = sfName(phdr, "PagedPreset")
	phdr = sfWord(phdr, 0) // patch
	phdr = sfWord(phdr, 0) // bank
	phdr = sfWord(phdr, 0) // first zone index
	phdr = sfLong(phdr, 0) // library
	phdr = sfLong(phdr, 0) // genre
	phdr = sfLong(phdr, 0) // morphology
	phdr = sfName(phdr, "EOP")
	for _, x := range []uint16{0, 0, 1} {
		phdr = sfWord(phdr, x)
	}
	for _, x := range []uint32{0, 0, 0} {
		phdr = sfLong(phdr, x)
	}

	// pbag: one preset zone plus terminator (4 bytes each).
	var pbag []byte
	pbag = sfWord(pbag, 0)
	pbag = sfWord(pbag, 0)
	pbag = sfWord(pbag, 1)
	pbag = sfWord(pbag, 0)

	// pgen: instrument id 0, then terminator.
	var pgen []byte
	pgen = sfWord(pgen, gen_Instrument)
	pgen = sfWord(pgen, 0)
	pgen = sfWord(pgen, 0)
	pgen = sfWord(pgen, 0)

	// inst: one instrument plus EOI (22 bytes each).
	var inst []byte
	inst = sfName(inst, "PagedInstrument")
	inst = sfWord(inst, 0)
	inst = sfName(inst, "EOI")
	inst = sfWord(inst, uint16(len(samples)))

	// ibag: one zone per sample plus terminator.
	var ibag []byte
	for i := 0; i <= len(samples); i++ {
		ibag = sfWord(ibag, uint16(i*3))
		ibag = sfWord(ibag, 0)
	}

	// igen: three generators per region, then terminator.
	var igen []byte
	for i, sample := range samples {
		keyRange := uint16(sample.keyLo) | uint16(sample.keyHi)<<8
		igen = sfWord(igen, gen_KeyRange)
		igen = sfWord(igen, keyRange)
		igen = sfWord(igen, gen_SampleModes)
		igen = sfWord(igen, sample.mode)
		igen = sfWord(igen, gen_SampleID)
		igen = sfWord(igen, uint16(i))
	}
	igen = sfWord(igen, 0)
	igen = sfWord(igen, 0)

	// shdr: one record per sample plus the EOS terminator (46 bytes each).
	var shdr []byte
	for _, sample := range samples {
		sampleType := uint16(1) // mono
		if sample.compressed {
			sampleType |= sfSampleTypeCompressed
		}
		shdr = sfName(shdr, sample.name)
		shdr = sfLong(shdr, sample.start)
		shdr = sfLong(shdr, sample.end)
		shdr = sfLong(shdr, sample.startLoop)
		shdr = sfLong(shdr, sample.endLoop)
		shdr = sfLong(shdr, 22050)
		shdr = append(shdr, 60) // original pitch
		shdr = append(shdr, 0)  // pitch correction
		shdr = sfWord(shdr, 0)  // link
		shdr = sfWord(shdr, sampleType)
	}
	shdr = sfName(shdr, "EOS")
	for _, x := range []uint32{uint32(frameCount), uint32(frameCount), uint32(frameCount), uint32(frameCount), 0} {
		shdr = sfLong(shdr, x)
	}
	shdr = append(shdr, 0, 0)
	shdr = sfWord(shdr, 0)
	shdr = sfWord(shdr, 0)

	var pdta []byte
	pdta = sfChunk(pdta, "phdr", phdr)
	pdta = sfChunk(pdta, "pbag", pbag)
	pdta = sfChunk(pdta, "pmod", make([]byte, 10))
	pdta = sfChunk(pdta, "pgen", pgen)
	pdta = sfChunk(pdta, "inst", inst)
	pdta = sfChunk(pdta, "ibag", ibag)
	pdta = sfChunk(pdta, "imod", make([]byte, 10))
	pdta = sfChunk(pdta, "igen", igen)
	pdta = sfChunk(pdta, "shdr", shdr)

	// Assemble the RIFF manually: the smpl payload is virtual and is never
	// held in a buffer. head ends immediately after the smpl chunk header;
	// an optional sm24 chunk and the pdta list form the tail.
	infoList := sfChunk(nil, "LIST", append([]byte("INFO"), info...))
	pdtaList := sfChunk(nil, "LIST", append([]byte("pdta"), pdta...))

	var tailExtra []byte
	if includeSM24 {
		tailExtra = sfChunk(tailExtra, "sm24", make([]byte, frameCount))
	}

	smplBytes := int64(frameCount * 2)
	sdtaPayload := int64(4) + 8 + smplBytes
	if includeSM24 {
		sdtaPayload += int64(8 + frameCount)
	}

	var sdtaHeader []byte
	sdtaHeader = append(sdtaHeader, "LIST"...)
	sdtaHeader = sfLong(sdtaHeader, uint32(sdtaPayload))
	sdtaHeader = append(sdtaHeader, "sdta"...)
	sdtaHeader = append(sdtaHeader, "smpl"...)
	sdtaHeader = sfLong(sdtaHeader, uint32(frameCount*2))

	riffPayload := int64(4) + int64(len(infoList)) + int64(len(sdtaHeader)) + smplBytes +
		int64(len(tailExtra)) + int64(len(pdtaList))

	var head []byte
	head = append(head, "RIFF"...)
	head = sfLong(head, uint32(riffPayload))
	head = append(head, "sfbk"...)
	head = append(head, infoList...)
	head = append(head, sdtaHeader...)

	tail := append(tailExtra, pdtaList...)

	return fontLayout{
		head:       head,
		tail:       tail,
		smplOffset: int64(len(head)),
		smplBytes:  smplBytes,
		frameCount: frameCount,
	}
}

// testWave is the deterministic, non-trivial PCM content used by the
// in-memory fixtures.
func testWave(i int) int16 {
	return int16((i*131+17)%2001 - 1000)
}

// materializeFont joins a layout and its virtual smpl content into one slice.
func materializeFont(layout fontLayout) []byte {
	data := make([]byte, 0, len(layout.head)+int(layout.smplBytes)+len(layout.tail))
	data = append(data, layout.head...)
	for i := 0; i < layout.frameCount; i++ {
		data = sfWord(data, uint16(testWave(i)))
	}
	data = append(data, layout.tail...)
	return data
}

// smallFontSamples spans several 8-frame pages: loops cross page seams and
// the second region runs into the final, partial page.
func smallFontSamples() []testSample {
	return []testSample{
		{name: "WaveLoop", start: 0, end: 40, startLoop: 8, endLoop: 24, mode: 1, keyLo: 0, keyHi: 63},
		{name: "WaveTail", start: 48, end: 141, startLoop: 56, endLoop: 80, mode: 3, keyLo: 64, keyHi: 127},
	}
}

func smallFontLayout() fontLayout {
	return buildFontLayout(smallFontSamples(), 142+46, false)
}

// ---------------------------------------------------------------------------
// ReaderAt test sources.
// ---------------------------------------------------------------------------

type readRange struct {
	off  int64
	size int
}

// trackedReaderAt records every ReadAt forwarded to the base source. A hook
// can inject short reads or arbitrary errors; hook hits are not forwarded.
type trackedReaderAt struct {
	base    io.ReaderAt
	records []readRange
	hook    func(off int64, p []byte) (int, error, bool)
}

func (r *trackedReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if r.hook != nil {
		if n, err, handled := r.hook(off, p); handled {
			return n, err
		}
	}
	r.records = append(r.records, readRange{off: off, size: len(p)})
	return r.base.ReadAt(p, off)
}

func (r *trackedReaderAt) pcmReads(layout fontLayout) []readRange {
	var out []readRange
	for _, rec := range r.records {
		if rec.off >= layout.smplOffset && rec.off+int64(rec.size) <= layout.smplOffset+layout.smplBytes {
			out = append(out, rec)
		}
	}
	return out
}

// virtualReaderAt serves a huge smpl pool without materializing it. Bytes
// outside the smpl range come from the small head/tail buffers.
type virtualReaderAt struct {
	layout    fontLayout
	waveAt    func(i int) int16
	tailBegin int64
}

func newVirtualReaderAt(layout fontLayout, waveAt func(int) int16) *virtualReaderAt {
	return &virtualReaderAt{
		layout:    layout,
		waveAt:    waveAt,
		tailBegin: layout.smplOffset + layout.smplBytes,
	}
}

func (v *virtualReaderAt) ReadAt(p []byte, off int64) (int, error) {
	smplEnd := v.layout.smplOffset + v.layout.smplBytes
	for i := 0; i < len(p); {
		pos := off + int64(i)
		switch {
		case pos < v.layout.smplOffset:
			n := copy(p[i:], v.layout.head[pos:])
			i += n
		case pos < smplEnd:
			frame := int((pos - v.layout.smplOffset) / 2)
			value := v.waveAt(frame)
			if (pos-v.layout.smplOffset)%2 == 0 {
				p[i] = byte(uint16(value))
			} else {
				p[i] = byte(uint16(value) >> 8)
			}
			i++
		default:
			tailPos := pos - v.tailBegin
			n := copy(p[i:], v.layout.tail[tailPos:])
			if n == 0 {
				return i, io.EOF
			}
			i += n
		}
	}
	return len(p), nil
}

func (v *virtualReaderAt) Size() int64 {
	return int64(len(v.layout.head)) + v.layout.smplBytes + int64(len(v.layout.tail))
}

// ---------------------------------------------------------------------------
// Rendering helpers.
// ---------------------------------------------------------------------------

func newTestSynth(sf *SoundFont) *Synthesizer {
	settings := NewSynthesizerSettings(22050)
	settings.EnableReverbAndChorus = false
	synth, err := NewSynthesizer(sf, settings)
	if err != nil {
		panic(err)
	}
	return synth
}

// drive plays the same MIDI sequence on every synthesizer it is given. The
// render callback decides how each 64-frame block is delivered.
func drive(s *Synthesizer, render func(int) (int, error)) error {
	steps := []struct {
		note   int32
		on     bool
		render int
	}{
		{60, true, 96},
		{62, true, 96},
		{64, true, 96},
		{60, false, 160},
		{64, false, 256},
	}
	for _, step := range steps {
		if step.on {
			s.NoteOn(0, step.note, 100)
		} else {
			s.NoteOff(0, step.note)
		}
		if _, err := render(step.render); err != nil {
			return err
		}
	}
	return nil
}

const driveTotal = 96 + 96 + 96 + 160 + 256

func renderEager(sf *SoundFont) ([]float32, []float32) {
	synth := newTestSynth(sf)
	left := make([]float32, driveTotal)
	right := make([]float32, driveTotal)
	cursor := 0
	err := drive(synth, func(n int) (int, error) {
		synth.Render(left[cursor:cursor+n], right[cursor:cursor+n])
		cursor += n
		return n, nil
	})
	if err != nil {
		panic(err)
	}
	return left, right
}

func renderPaged(sf *SoundFont, chunkSizes []int) ([]float32, []float32, error) {
	synth := newTestSynth(sf)
	left := make([]float32, driveTotal)
	right := make([]float32, driveTotal)
	cursor := 0
	step := 0
	err := drive(synth, func(n int) (int, error) {
		written := 0
		for written < n {
			chunk := n - written
			if step < len(chunkSizes) {
				chunk = chunkSizes[step]
				step++
			}
			if chunk > n-written {
				chunk = n - written
			}
			got, err := synth.RenderChecked(left[cursor:cursor+chunk], right[cursor:cursor+chunk])
			if err != nil {
				return written, err
			}
			written += got
			cursor += got
		}
		return written, nil
	})
	return left, right, err
}

func buffersMatch(c *taskcheck.C, label string, want, got []float32) {
	c.Check(len(want) == len(got), "%s: buffer length want=%d got=%d", label, len(want), len(got))
	nonZero := 0
	for i := range want {
		if math.IsNaN(float64(got[i])) || math.IsInf(float64(got[i]), 0) || want[i] != got[i] {
			c.Check(false, "%s: PCM mismatch at frame %d want=%v got=%v", label, i, want[i], got[i])
			return
		}
		if got[i] != 0 {
			nonZero++
		}
	}
	c.Check(nonZero > 0, "%s: rendered silence", label)
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

func TestPagedFontLoad(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		layout := smallFontLayout()
		data := materializeFont(layout)

		// Eager reference font.
		eager, err := NewSoundFont(bytes.NewReader(data))
		c.Check(err == nil, "eager load failed: %v", err)
		resident, capacity, loads := eager.SampleCacheStats()
		c.Check(resident == 0 && capacity == 0 && loads == 0, "eager stats must be zeroes, got %d %d %d", resident, capacity, loads)
		c.Check(eager.WaveData != nil && len(eager.WaveData) == layout.frameCount, "eager WaveData must be fully populated")

		// Paged load: not one byte of the smpl payload may be read.
		source := &trackedReaderAt{base: bytes.NewReader(data)}
		paged, err := NewSoundFontReaderAt(source, int64(len(data)), 8, 3)
		c.Check(err == nil, "paged load failed: %v", err)
		c.Check(paged != nil, "paged font must be returned")
		c.Check(paged.WaveData == nil, "paged WaveData must stay nil")
		c.Check(paged.BitsPerSample == 16, "paged font must be 16-bit PCM")
		c.Check(len(source.pcmReads(layout)) == 0, "load touched %d smpl bytes", len(source.pcmReads(layout)))
		c.Check(len(source.records) > 0, "load must still read the RIFF/INFO/pdta metadata")

		c.Check(len(paged.Presets) == len(eager.Presets), "preset count mismatch")
		c.Check(len(paged.Instruments) == len(eager.Instruments), "instrument count mismatch")
		c.Check(len(paged.SampleHeaders) == len(eager.SampleHeaders), "sample header count mismatch")
		for i, header := range paged.SampleHeaders {
			c.Check(header.Name == eager.SampleHeaders[i].Name, "sample name %d mismatch", i)
			c.Check(header.Start == eager.SampleHeaders[i].Start, "sample start %d mismatch", i)
			c.Check(header.End == eager.SampleHeaders[i].End, "sample end %d mismatch", i)
		}

		// Invalid parameters are rejected before any metadata work.
		if _, err := NewSoundFontReaderAt(bytes.NewReader(data), int64(len(data)), 1, 3); err == nil {
			c.Check(false, "pageFrames below 2 must be rejected")
		} else {
			c.Check(err != nil, "pageFrames below 2 must be rejected")
		}
		if _, err := NewSoundFontReaderAt(bytes.NewReader(data), int64(len(data)), 8, 0); err == nil {
			c.Check(false, "maxPages below 1 must be rejected")
		} else {
			c.Check(err != nil, "maxPages below 1 must be rejected")
		}
		if _, err := NewSoundFontReaderAt(nil, int64(len(data)), 8, 3); err == nil {
			c.Check(false, "nil reader must be rejected")
		} else {
			c.Check(err != nil, "nil reader must be rejected")
		}

		// Metadata out of bounds and structural problems must fail at load.
		truncated := data[:len(data)-40]
		if _, err := NewSoundFontReaderAt(bytes.NewReader(truncated), int64(len(truncated)), 8, 3); err == nil {
			c.Check(false, "truncated font must be rejected")
		} else {
			c.Check(err != nil, "truncated font must be rejected")
		}

		badEnd := append([]byte(nil), data...)
		badSamples := smallFontSamples()
		badSamples[0].end = 99999
		badLayout := buildFontLayout(badSamples, 142+46, false)
		badEnd = materializeFont(badLayout)
		if _, err := NewSoundFontReaderAt(bytes.NewReader(badEnd), int64(len(badEnd)), 8, 3); err == nil {
			c.Check(false, "sample end outside the pool must be rejected")
		} else {
			c.Check(err != nil, "sample end outside the pool must be rejected")
		}

		sm24Layout := buildFontLayout(smallFontSamples(), 142+46, true)
		sm24Data := make([]byte, 0, len(sm24Layout.head)+int(sm24Layout.smplBytes)+len(sm24Layout.tail))
		sm24Data = append(sm24Data, sm24Layout.head...)
		sm24Data = append(sm24Data, make([]byte, sm24Layout.smplBytes)...)
		sm24Data = append(sm24Data, sm24Layout.tail...)
		if _, err := NewSoundFontReaderAt(bytes.NewReader(sm24Data), int64(len(sm24Data)), 8, 3); err == nil {
			c.Check(false, "sm24 sample data must be rejected")
		} else {
			c.Check(err != nil, "sm24 sample data must be rejected")
		}

		compressed := smallFontSamples()
		compressed[0].compressed = true
		compressedLayout := buildFontLayout(compressed, 142+46, false)
		if _, err := NewSoundFontReaderAt(bytes.NewReader(materializeFont(compressedLayout)), int64(len(materializeFont(compressedLayout))), 8, 3); err == nil {
			c.Check(false, "compressed samples must be rejected")
		} else {
			c.Check(err != nil, "compressed samples must be rejected")
		}

		// Render a little so the cache fills, then check the exact read
		// geometry: aligned page reads, at most one page per read and every
		// read contained in the smpl pool.
		synth := newTestSynth(paged)
		synth.NoteOn(0, 60, 100)
		synth.NoteOn(0, 64, 100)
		lbuf := make([]float32, 400)
		rbuf := make([]float32, 400)
		n, err := synth.RenderChecked(lbuf, rbuf)
		c.Check(err == nil, "warm-up render failed: %v", err)
		c.Check(n == 400, "warm-up render wrote %d frames", n)

		pcm := source.pcmReads(layout)
		c.Check(len(pcm) > 0, "rendering must load sample pages")
		pageBytes := int64(8 * 2)
		lastPageID := int64(layout.frameCount-1) / 8
		lastPageSize := int((int64(layout.frameCount) - lastPageID*8) * 2)
		lastPageOffset := layout.smplOffset + lastPageID*pageBytes
		for _, rec := range pcm {
			rel := rec.off - layout.smplOffset
			c.Check(rel%pageBytes == 0, "page read at %d is not page aligned", rec.off)
			c.Check(int64(rec.size) <= pageBytes, "page read of %d bytes exceeds one page", rec.size)
			c.Check(rec.off >= layout.smplOffset && rec.off+int64(rec.size) <= layout.smplOffset+layout.smplBytes, "read outside the smpl pool")
		}

		// Touch the final frame directly: the last page holds only the
		// remaining real frames, never padding.
		readsBeforeLast := len(source.records)
		lastValue, lastErr := paged.pageCache.sampleAt(int32(layout.frameCount - 1))
		c.Check(lastErr == nil, "last frame lookup failed: %v", lastErr)
		c.Check(lastValue == testWave(layout.frameCount-1), "last frame content mismatch")
		lastReads := source.records[readsBeforeLast:]
		c.Check(len(lastReads) == 1, "last frame must cost exactly one page read, got %d", len(lastReads))
		c.Check(lastReads[0].off == lastPageOffset, "last page read from %d, want %d", lastReads[0].off, lastPageOffset)
		c.Check(lastReads[0].size == lastPageSize, "last page must read only the %d real frames, got %d bytes", lastPageSize/2, lastReads[0].size)

		resident, capacity, loads = paged.SampleCacheStats()
		c.Check(capacity == 3, "cache capacity must be 3, got %d", capacity)
		c.Check(resident <= 3 && resident > 0, "resident pages out of bounds: %d", resident)
		c.Check(int(loads) == len(source.pcmReads(layout)), "each successful page read is one load: loads=%d reads=%d", loads, len(source.pcmReads(layout)))

		// Pin exactly three pages, then re-request their frames: every access
		// is now a hit, so no new ReadAt calls and no new loads may occur.
		pinFont, pinErr := NewSoundFontReaderAt(&trackedReaderAt{base: bytes.NewReader(data)}, int64(len(data)), 8, 3)
		c.Check(pinErr == nil, "pin font load failed: %v", pinErr)
		pinSource := pinFont.pageCache.source.(*trackedReaderAt)
		for _, frame := range []int32{0, 8, 16} {
			_, ferr := pinFont.pageCache.sampleAt(frame)
			c.Check(ferr == nil, "pin read of frame %d failed: %v", frame, ferr)
		}
		pinReadsBefore := len(pinSource.records)
		_, _, pinLoadsBefore := pinFont.SampleCacheStats()
		for _, frame := range []int32{1, 7, 8, 15, 16, 0, 15} {
			_, ferr := pinFont.pageCache.sampleAt(frame)
			c.Check(ferr == nil, "hit read of frame %d failed: %v", frame, ferr)
		}
		c.Check(len(pinSource.records) == pinReadsBefore, "cache hits performed %d unexpected reads", len(pinSource.records)-pinReadsBefore)
		_, _, pinLoadsAfter := pinFont.SampleCacheStats()
		c.Check(pinLoadsAfter == pinLoadsBefore, "cache hits incremented loads: %d -> %d", pinLoadsBefore, pinLoadsAfter)
	})
}

func TestPagedFontRender(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		layout := smallFontLayout()
		data := materializeFont(layout)
		eager, err := NewSoundFont(bytes.NewReader(data))
		c.Check(err == nil, "eager load failed: %v", err)
		wantLeft, wantRight := renderEager(eager)

		// Caches of one to three pages all have to reproduce the eager PCM
		// bit-for-bit, including fractional cross-page interpolation, loop
		// wrap-around and the non-loop tail after note off.
		for _, maxPages := range []int{1, 2, 3} {
			source := &trackedReaderAt{base: bytes.NewReader(data)}
			paged, err := NewSoundFontReaderAt(source, int64(len(data)), 8, maxPages)
			c.Check(err == nil, "paged load (maxPages=%d) failed: %v", maxPages, err)

			chunks := []int{7, 61, 3, 128, 185, 1, 33, 96, 64, 17}
			gotLeft, gotRight, err := renderPaged(paged, chunks)
			c.Check(err == nil, "paged render (maxPages=%d) failed: %v", maxPages, err)
			buffersMatch(c, fmt.Sprintf("maxPages=%d/left", maxPages), wantLeft, gotLeft)
			buffersMatch(c, fmt.Sprintf("maxPages=%d/right", maxPages), wantRight, gotRight)

			resident, capacity, _ := paged.SampleCacheStats()
			c.Check(capacity == maxPages, "capacity mismatch for maxPages=%d", maxPages)
			c.Check(resident <= maxPages, "cache exceeded its capacity: %d > %d", resident, maxPages)
		}

		// The legacy Render path (which panics on a paged read error) must
		// produce the same PCM as RenderChecked in the normal case.
		renderFont, renderErr := NewSoundFontReaderAt(&trackedReaderAt{base: bytes.NewReader(data)}, int64(len(data)), 8, 2)
		c.Check(renderErr == nil, "Render-path font load failed: %v", renderErr)
		renderSynth := newTestSynth(renderFont)
		legacyLeft := make([]float32, driveTotal)
		legacyRight := make([]float32, driveTotal)
		legacyCursor := 0
		legacyErr := drive(renderSynth, func(n int) (int, error) {
			renderSynth.Render(legacyLeft[legacyCursor:legacyCursor+n], legacyRight[legacyCursor:legacyCursor+n])
			legacyCursor += n
			return n, nil
		})
		c.Check(legacyErr == nil, "Render-path drive failed: %v", legacyErr)
		buffersMatch(c, "legacy-Render/left", wantLeft, legacyLeft)
		buffersMatch(c, "legacy-Render/right", wantRight, legacyRight)

		// Two synthesizers sharing one maxPages=1 font interleave irregular
		// renders and evict each other's pages continuously. Each one keeps
		// its own phase, filters and partially consumed blocks, and both
		// outputs still match their private eager references.
		sharedSource := &trackedReaderAt{base: bytes.NewReader(data)}
		shared, err := NewSoundFontReaderAt(sharedSource, int64(len(data)), 8, 1)
		c.Check(err == nil, "shared paged load failed: %v", err)

		synthA := newTestSynth(shared)
		synthB := newTestSynth(shared)
		refA := newTestSynth(eager)
		refB := newTestSynth(eager)

		outAL := make([]float32, driveTotal)
		outAR := make([]float32, driveTotal)
		outBL := make([]float32, driveTotal)
		outBR := make([]float32, driveTotal)
		refAL := make([]float32, driveTotal)
		refAR := make([]float32, driveTotal)
		refBL := make([]float32, driveTotal)
		refBR := make([]float32, driveTotal)

		events := []struct {
			note int32
			on   bool
		}{
			{60, true}, {62, true}, {64, true}, {60, false}, {64, false},
		}
		renderCounts := []int{96, 96, 96, 160, 256}
		irregular := []int{13, 1, 17, 5, 64, 29, 2, 41}

		cursorA, cursorB, cursorRefA, cursorRefB, chunkStep := 0, 0, 0, 0, 0
		var startLoads uint64
		_, _, startLoads = shared.SampleCacheStats()

		for eventIndex, event := range events {
			if event.on {
				synthA.NoteOn(0, event.note, 100)
				synthB.NoteOn(0, event.note, 100)
				refA.NoteOn(0, event.note, 100)
				refB.NoteOn(0, event.note, 100)
			} else {
				synthA.NoteOff(0, event.note)
				synthB.NoteOff(0, event.note)
				refA.NoteOff(0, event.note)
				refB.NoteOff(0, event.note)
			}

			total := renderCounts[eventIndex]
			for delivered := 0; delivered < total; {
				n := irregular[chunkStep%len(irregular)]
				chunkStep++
				if n > total-delivered {
					n = total - delivered
				}

				na, errA := synthA.RenderChecked(outAL[cursorA:cursorA+n], outAR[cursorA:cursorA+n])
				c.Check(errA == nil, "synth A render failed: %v", errA)
				refA.Render(refAL[cursorRefA:cursorRefA+na], refAR[cursorRefA:cursorRefA+na])
				cursorA += na
				cursorRefA += na

				nb, errB := synthB.RenderChecked(outBL[cursorB:cursorB+n], outBR[cursorB:cursorB+n])
				c.Check(errB == nil, "synth B render failed: %v", errB)
				refB.Render(refBL[cursorRefB:cursorRefB+nb], refBR[cursorRefB:cursorRefB+nb])
				cursorB += nb
				cursorRefB += nb

				delivered += n
			}
		}

		buffersMatch(c, "interleave/A/left", refAL, outAL)
		buffersMatch(c, "interleave/A/right", refAR, outAR)
		buffersMatch(c, "interleave/B/left", refBL, outBL)
		buffersMatch(c, "interleave/B/right", refBR, outBR)

		resident, capacity, endLoads := shared.SampleCacheStats()
		c.Check(capacity == 1 && resident == 1, "maxPages=1 cache must hold exactly one page, got %d/%d", resident, capacity)
		c.Check(endLoads > startLoads, "interleaved synths must trigger page reloads, loads %d -> %d", startLoads, endLoads)

		// A very large smpl pool served by a virtual ReaderAt: loading and
		// the steady-state cache footprint must not depend on pool length.
		for _, poolFrames := range []int{250003, 1000003} {
			largeSamples := []testSample{
				{name: "BigLoop", start: 0, end: uint32(poolFrames - 46), startLoop: 8, endLoop: 24, mode: 1, keyLo: 0, keyHi: 127},
			}
			largeLayout := buildFontLayout(largeSamples, poolFrames, false)
			virtual := newVirtualReaderAt(largeLayout, func(i int) int16 {
				return int16((i*131+17)%2001 - 1000)
			})
			tracked := &trackedReaderAt{base: virtual}

			big, err := NewSoundFontReaderAt(tracked, virtual.Size(), 8, 3)
			c.Check(err == nil, "virtual font (pool=%d) load failed: %v", poolFrames, err)
			c.Check(len(tracked.pcmReads(largeLayout)) == 0, "virtual font load read smpl payload bytes")
			c.Check(big.WaveData == nil, "virtual font must not allocate a sample pool")

			bigSynth := newTestSynth(big)
			bigSynth.NoteOn(0, 60, 100)
			bufL := make([]float32, 2000)
			bufR := make([]float32, 2000)
			n, err := bigSynth.RenderChecked(bufL, bufR)
			c.Check(err == nil, "virtual font render failed: %v", err)
			c.Check(n == 2000, "virtual render wrote %d frames", n)
			energy := 0.0
			for _, x := range bufL {
				energy += float64(x * x)
			}
			c.Check(energy > 0.0, "virtual font rendered silence")

			for _, rec := range tracked.pcmReads(largeLayout) {
				c.Check(rec.size <= 16, "virtual read exceeded one page: %d bytes", rec.size)
				c.Check((rec.off-largeLayout.smplOffset)%16 == 0, "virtual read was not page aligned")
			}
			resident, capacity, loads := big.SampleCacheStats()
			c.Check(capacity == 3 && resident == 3, "virtual font should warm a 3-page working set, got %d/%d", resident, capacity)

			// The loop only touches pages 0..2, which now all reside in the
			// cache: a long further render adds no loads regardless of the
			// pool's total length.
			_, _, loadsBefore := big.SampleCacheStats()
			moreL := make([]float32, 4000)
			moreR := make([]float32, 4000)
			_, err = bigSynth.RenderChecked(moreL, moreR)
			c.Check(err == nil, "steady-state virtual render failed: %v", err)
			_, _, loadsAfter := big.SampleCacheStats()
			c.Check(loadsAfter == loadsBefore, "steady-state loads grew with pool length: %d -> %d", loadsBefore, loadsAfter)
			c.Check(int(loads) <= 4, "working set must stay bounded, got %d loads for a %d-frame pool", loads, poolFrames)
		}
	})
}

func TestPagedFontFailure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		layout := smallFontLayout()
		data := materializeFont(layout)

		// --- Injected short read. ---
		// Phase 1: the hook is armed before the first render, so the first
		// page miss fails immediately and nothing can be delivered.
		shortSource := &trackedReaderAt{base: bytes.NewReader(data)}
		shortFont, err := NewSoundFontReaderAt(shortSource, int64(len(data)), 8, 3)
		c.Check(err == nil, "short-read font load failed: %v", err)

		var shortOff int64
		var shortFired bool
		shortSource.hook = func(off int64, p []byte) (int, error, bool) {
			if !shortFired && off >= layout.smplOffset && off < layout.smplOffset+layout.smplBytes {
				shortFired = true
				shortOff = off
				return len(p) / 2, nil, true
			}
			return 0, nil, false
		}

		shortSynth := newTestSynth(shortFont)
		shortSynth.NoteOn(0, 60, 100)
		immediateL := make([]float32, 64)
		immediateR := make([]float32, 64)
		n, err := shortSynth.RenderChecked(immediateL, immediateR)
		c.Check(shortFired, "the short-read hook was never triggered")
		c.Check(errors.Is(err, io.ErrUnexpectedEOF), "short read must surface ErrUnexpectedEOF, got %v", err)
		c.Check(n == 0, "first-block failure must deliver nothing, got %d", n)

		// The failure is sticky: Reset cannot clear it and no more reads happen.
		readsAfterFailure := len(shortSource.records)
		shortSynth.Reset()
		_, errAgain := shortSynth.RenderChecked(make([]float32, 16), make([]float32, 16))
		c.Check(errAgain == err, "sticky error changed after Reset: %v vs %v", errAgain, err)
		c.Check(len(shortSource.records) == readsAfterFailure, "sticky synth performed new reads")

		// A fresh synthesizer on the same font can still request the failed
		// page: failed pages never enter the shared cache.
		otherSynth := newTestSynth(shortFont)
		otherSynth.NoteOn(0, 60, 100)
		otherL := make([]float32, 128)
		otherR := make([]float32, 128)
		otherN, otherErr := otherSynth.RenderChecked(otherL, otherR)
		c.Check(otherErr == nil, "fresh synth was poisoned by the failed page: %v", otherErr)
		c.Check(otherN == 128, "fresh synth wrote %d frames", otherN)
		reRequested := false
		for _, rec := range shortSource.records[readsAfterFailure:] {
			if rec.off == shortOff {
				reRequested = true
			}
		}
		c.Check(reRequested, "fresh synth could not re-request the failed page at %d", shortOff)

		// Phase 2: deliver 100 frames first (one full 64-frame block plus 36
		// frames of the next), arm the hook, then the remaining 28 frames of
		// the cached block are still delivered before a new block fails.
		// A single-page cache guarantees that the next block misses and
		// reaches the hook.
		prefixSource := &trackedReaderAt{base: bytes.NewReader(data)}
		prefixFont, prefixErr := NewSoundFontReaderAt(prefixSource, int64(len(data)), 8, 1)
		c.Check(prefixErr == nil, "prefix font load failed: %v", prefixErr)
		prefixSynth := newTestSynth(prefixFont)
		prefixSynth.NoteOn(0, 60, 100)
		goodL := make([]float32, 100)
		goodR := make([]float32, 100)
		goodN, goodErr := prefixSynth.RenderChecked(goodL, goodR)
		c.Check(goodErr == nil, "initial render failed: %v", goodErr)
		c.Check(goodN == 100, "initial render wrote %d frames", goodN)

		prefixSource.hook = func(off int64, p []byte) (int, error, bool) {
			if off >= layout.smplOffset && off < layout.smplOffset+layout.smplBytes {
				return len(p) / 2, nil, true
			}
			return 0, nil, false
		}
		const sentinel float32 = 1234.5
		sentinelL := make([]float32, 60)
		sentinelR := make([]float32, 60)
		for i := range sentinelL {
			sentinelL[i] = sentinel
			sentinelR[i] = sentinel
		}
		prefixN, prefixErr2 := prefixSynth.RenderChecked(sentinelL, sentinelR)
		c.Check(errors.Is(prefixErr2, io.ErrUnexpectedEOF), "prefix short read must surface ErrUnexpectedEOF, got %v", prefixErr2)
		c.Check(prefixN == 28, "expected 28 delivered frames before failure, got %d", prefixN)
		for i := prefixN; i < 60; i++ {
			c.Check(sentinelL[i] == sentinel && sentinelR[i] == sentinel, "tail sentinel overwritten at %d", i)
		}
		for i := 0; i < prefixN; i++ {
			c.Check(sentinelL[i] != sentinel, "delivered prefix was not written at %d", i)
		}

		// --- Injected custom I/O error. ---
		errSynthetic := errors.New("synthetic reader failure")
		ioSource := &trackedReaderAt{base: bytes.NewReader(data)}
		ioFont, err := NewSoundFontReaderAt(ioSource, int64(len(data)), 8, 2)
		c.Check(err == nil, "io-error font load failed: %v", err)

		var ioFired bool
		ioSource.hook = func(off int64, p []byte) (int, error, bool) {
			if !ioFired && off >= layout.smplOffset && off < layout.smplOffset+layout.smplBytes {
				ioFired = true
				return 0, errSynthetic, true
			}
			return 0, nil, false
		}

		ioSynth := newTestSynth(ioFont)
		ioSynth.NoteOn(0, 60, 100)
		residentBefore, _, loadsBefore := ioFont.SampleCacheStats()
		recordsBefore := len(ioSource.records)
		failL := make([]float32, 128)
		failR := make([]float32, 128)
		n, err = ioSynth.RenderChecked(failL, failR)
		c.Check(ioFired, "the I/O error hook was never triggered")
		c.Check(err == errSynthetic, "custom error must be returned verbatim, got %v", err)
		c.Check(n == 0, "no frames are deliverable when the first new block fails, got %d", n)

		residentAfter, _, loadsAfter := ioFont.SampleCacheStats()
		c.Check(residentAfter == residentBefore, "failed page changed resident count %d -> %d", residentBefore, residentAfter)
		c.Check(loadsAfter == loadsBefore, "failed page incremented loads %d -> %d", loadsBefore, loadsAfter)

		// Sticky: same cause, no further reads, Reset ineffective.
		ioSynth.Reset()
		_, errSticky := ioSynth.RenderChecked(make([]float32, 8), make([]float32, 8))
		c.Check(errSticky == errSynthetic, "sticky custom error changed: %v", errSticky)
		c.Check(len(ioSource.records) == recordsBefore, "sticky custom-error synth performed new reads")

		// Render must panic with the same error instead of hiding it.
		func() {
			defer func() {
				recovered := recover()
				c.Check(recovered != nil, "Render did not panic on a paged read error")
				if recovered != nil {
					c.Check(recovered == errSynthetic, "Render panicked with %v, want %v", recovered, errSynthetic)
				}
			}()
			ioSynth.Render(make([]float32, 8), make([]float32, 8))
		}()

		// Another synthesizer on the same font is unaffected: the one-shot
		// hook is spent, so all its pages load successfully.
		sibling := newTestSynth(ioFont)
		sibling.NoteOn(0, 60, 100)
		sibL := make([]float32, 192)
		sibR := make([]float32, 192)
		sibN, sibErr := sibling.RenderChecked(sibL, sibR)
		c.Check(sibErr == nil, "sibling synth inherited the failure: %v", sibErr)
		c.Check(sibN == 192, "sibling synth wrote %d frames", sibN)

		// Unequal buffer lengths fail before any state change: the exact
		// same sequence then renders bit-identical to the eager reference.
		eager, eagerErr := NewSoundFont(bytes.NewReader(data))
		c.Check(eagerErr == nil, "eager reference load failed: %v", eagerErr)
		cleanSource := &trackedReaderAt{base: bytes.NewReader(data)}
		cleanFont, cleanErr := NewSoundFontReaderAt(cleanSource, int64(len(data)), 8, 3)
		c.Check(cleanErr == nil, "clean font load failed: %v", cleanErr)
		cleanSynth := newTestSynth(cleanFont)
		_, mismatchErr := cleanSynth.RenderChecked(make([]float32, 10), make([]float32, 11))
		c.Check(mismatchErr != nil, "unequal buffer lengths must be rejected")
		gotLeft, gotRight, renderErr := renderPaged(cleanFont, []int{5, 19, 2, 77, 64, 31})
		c.Check(renderErr == nil, "render after rejected call failed: %v", renderErr)
		wantLeft, wantRight := renderEager(eager)
		buffersMatch(c, "post-mismatch/left", wantLeft, gotLeft)
		buffersMatch(c, "post-mismatch/right", wantRight, gotRight)
	})
}
