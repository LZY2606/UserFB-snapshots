package meltysynth

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
)

// NewSoundFontReaderAt loads a 16-bit PCM SoundFont from a random-access
// source without reading the smpl payload. Only the RIFF/LIST headers, the
// INFO list and the pdta list are read at load time; WaveData stays nil.
// Sample frames are fetched on demand during synthesis through a shared,
// bounded LRU page cache.
//
// pageFrames is the number of int16 frames held by each page and must be at
// least 2. maxPages is the capacity of the shared cache and must be at least
// 1. SoundFonts using sm24 (24-bit) or compressed samples are rejected.
//
// The ReaderAt is retained and used during later synthesis. Its lifetime is
// managed by the caller: it must stay readable for as long as any
// synthesizer using this SoundFont renders.
func NewSoundFontReaderAt(r io.ReaderAt, size int64, pageFrames, maxPages int) (*SoundFont, error) {
	if r == nil {
		return nil, errors.New("the reader must not be nil")
	}
	if pageFrames < 2 {
		return nil, errors.New("pageFrames must be greater than or equal to 2")
	}
	if maxPages < 1 {
		return nil, errors.New("maxPages must be greater than or equal to 1")
	}
	// Sample coordinates are int32 and one page must fit into one byte
	// allocation, so reject sizes the fixed-point oscillator could not
	// address anyway.
	if int64(pageFrames) > math.MaxInt32/2 {
		return nil, errors.New("pageFrames is too large")
	}
	if int64(maxPages) > math.MaxInt32 {
		return nil, errors.New("maxPages is too large")
	}
	if size < 12 {
		return nil, errors.New("the data is too small to be a RIFF SoundFont")
	}

	root := io.NewSectionReader(r, 0, size)

	chunkId, err := readFourCC(root)
	if err != nil {
		return nil, err
	}
	if chunkId != "RIFF" {
		return nil, errors.New("the riff chunk was not found")
	}

	var riffSize int32
	if err = binary.Read(root, binary.LittleEndian, &riffSize); err != nil {
		return nil, err
	}
	if riffSize < 4 {
		return nil, errors.New("the riff chunk has an invalid size")
	}
	// The declared size excludes word-padding bytes, while the physical
	// source may include up to one pad byte per top-level list.
	declared := int64(riffSize) + 8
	if declared > size || size-declared > 3 {
		return nil, fmt.Errorf("the riff chunk size %d is out of bounds for a source of %d bytes", riffSize, size)
	}

	formType, err := readFourCC(root)
	if err != nil {
		return nil, err
	}
	if formType != "sfbk" {
		return nil, fmt.Errorf(`the type of the riff chunk must be "sfbk", but was %q`, formType)
	}

	result := &SoundFont{BitsPerSample: 16}

	var smplOffset int64 = -1
	var smplBytes int64

	// The three top-level lists must appear in the order INFO, sdta, pdta.
	wantKinds := []string{"INFO", "sdta", "pdta"}
	pos := int64(12)
	for listIndex, wantKind := range wantKinds {
		if pos >= size {
			return nil, fmt.Errorf("the %q list is missing", wantKind)
		}

		header := io.NewSectionReader(r, pos, size-pos)

		listId, err := readFourCC(header)
		if err != nil {
			return nil, err
		}
		if listId != "LIST" {
			return nil, errors.New("the list chunk was not found")
		}

		var listSize int32
		if err = binary.Read(header, binary.LittleEndian, &listSize); err != nil {
			return nil, err
		}
		if listSize < 4 {
			return nil, fmt.Errorf("the %q list has an invalid size", wantKind)
		}

		kind, err := readFourCC(header)
		if err != nil {
			return nil, err
		}
		if kind != wantKind {
			return nil, fmt.Errorf(`the type of the list chunk must be %q, but was %q`, wantKind, kind)
		}

		chunkEnd := pos + 8 + int64(listSize)
		if chunkEnd > size {
			return nil, fmt.Errorf("the %q list extends past the end of the source", wantKind)
		}

		whole := io.NewSectionReader(r, pos, 8+int64(listSize))

		switch listIndex {
		case 0:
			result.Info, err = newSoundFontInfo(whole)
			if err != nil {
				return nil, err
			}

		case 1:
			smplRelOffset, smplSize, err := scanSampleDataList(whole)
			if err != nil {
				return nil, err
			}
			smplOffset = pos + smplRelOffset
			smplBytes = smplSize

		case 2:
			parameters, err := newSoundFontParameters(whole)
			if err != nil {
				return nil, err
			}
			result.SampleHeaders = parameters.sampleHeaders
			result.Presets = parameters.presets
			result.Instruments = parameters.instruments
		}

		// Chunks are word-aligned, but the declared size excludes the pad.
		pos = chunkEnd + (int64(listSize) & 1)
	}

	if pos > size {
		return nil, errors.New("the lists extend past the end of the source")
	}
	if size-pos > 3 {
		return nil, errors.New("unexpected trailing data after the pdta list")
	}
	// Any remaining bytes can only be RIFF word-padding, which must be zero.
	if pos < size {
		padding := make([]byte, size-pos)
		if _, err := r.ReadAt(padding, pos); err != nil {
			return nil, err
		}
		for _, value := range padding {
			if value != 0 {
				return nil, errors.New("unexpected trailing data after the pdta list")
			}
		}
	}
	if smplOffset < 0 {
		return nil, errors.New("no valid sample data was found")
	}
	if smplBytes%2 != 0 {
		return nil, errors.New("the smpl chunk must contain an even number of bytes")
	}

	frameCount := smplBytes / 2
	if frameCount == 0 {
		return nil, errors.New("the smpl chunk is empty")
	}
	if frameCount > math.MaxInt32 {
		return nil, errors.New("the smpl pool is larger than the int32 sample coordinates can address")
	}
	if err := validatePagedMetadata(result, frameCount); err != nil {
		return nil, err
	}

	result.pageCache = newSamplePageCache(r, smplOffset, frameCount, pageFrames, maxPages)

	return result, nil
}

// scanSampleDataList walks an sdta LIST without reading any chunk payload.
// It returns the offset of the smpl payload relative to the list start and
// the payload size in bytes.
func scanSampleDataList(list io.Reader) (int64, int64, error) {
	chunkId, err := readFourCC(list)
	if err != nil {
		return 0, 0, err
	}
	if chunkId != "LIST" {
		return 0, 0, errors.New("the list chunk was not found")
	}

	var listSize int32
	if err = binary.Read(list, binary.LittleEndian, &listSize); err != nil {
		return 0, 0, err
	}

	listType, err := readFourCC(list)
	if err != nil {
		return 0, 0, err
	}
	if listType != "sdta" {
		return 0, 0, fmt.Errorf(`the type of the list chunk must be "sdta", but was %q`, listType)
	}

	// All positions below are local to the list start, so the 8-byte LIST
	// header is included and the list type ends at offset 12.
	end := 8 + int64(listSize)
	var pos int64 = 12
	var smplRelOffset int64 = -1
	var smplSize int64

	seeker, ok := list.(io.Seeker)
	if !ok {
		return 0, 0, errors.New("the sdta list does not support seeking")
	}

	for pos < end {
		id, err := readFourCC(list)
		if err != nil {
			return 0, 0, err
		}
		pos += 4

		var subSize int32
		if err = binary.Read(list, binary.LittleEndian, &subSize); err != nil {
			return 0, 0, err
		}
		pos += 4

		payloadEnd := pos + int64(subSize)
		if payloadEnd > end {
			return 0, 0, fmt.Errorf("the %q sub-chunk extends past the sdta list", id)
		}

		switch id {
		case "smpl":
			if smplRelOffset >= 0 {
				return 0, 0, errors.New("multiple smpl chunks are not supported")
			}
			smplRelOffset = pos
			smplSize = int64(subSize)

		case "sm24":
			return 0, 0, errors.New("24 bit (sm24) sample data is not supported")

		default:
			return 0, 0, fmt.Errorf("the sample data list contains an unknown id %q", id)
		}

		// Skip the payload without reading a single byte of it.
		if _, err = seeker.Seek(payloadEnd+(int64(subSize)&1), io.SeekStart); err != nil {
			return 0, 0, err
		}
		pos = payloadEnd + (int64(subSize) & 1)
	}

	if smplRelOffset < 0 {
		return 0, 0, errors.New("no valid sample data was found")
	}

	return smplRelOffset, smplSize, nil
}

// sfSampleTypeCompressed is the flag used by SF3-style compressed samples.
const sfSampleTypeCompressed uint16 = 0x0010

func validatePagedMetadata(sf *SoundFont, frameCount int64) error {
	for _, header := range sf.SampleHeaders {
		if header.SampleType&sfSampleTypeCompressed != 0 {
			return fmt.Errorf("compressed samples are not supported: %q", header.Name)
		}

		if header.Start < 0 || int64(header.Start) >= frameCount {
			return fmt.Errorf("the sample %q starts outside the smpl pool", header.Name)
		}
		// The oscillator always reads the frame right after the last one it
		// may interpolate, so End plus one must still be inside the pool.
		if header.End < header.Start || int64(header.End)+1 > frameCount {
			return fmt.Errorf("the sample %q ends outside the smpl pool", header.Name)
		}
		if header.StartLoop < 0 || header.EndLoop < header.StartLoop {
			return fmt.Errorf("the sample %q has an invalid loop", header.Name)
		}
		if header.EndLoop > header.End {
			return fmt.Errorf("the sample %q has a loop ending outside the sample", header.Name)
		}
	}

	for _, instrument := range sf.Instruments {
		for _, region := range instrument.Regions {
			start := region.GetSampleStart()
			end := region.GetSampleEnd()
			startLoop := region.GetSampleStartLoop()
			endLoop := region.GetSampleEndLoop()

			if start < 0 || int64(start) >= frameCount {
				return fmt.Errorf("the instrument %q references a sample start outside the smpl pool", instrument.Name)
			}
			if end < start || int64(end)+1 > frameCount {
				return fmt.Errorf("the instrument %q references a sample end outside the smpl pool", instrument.Name)
			}
			if startLoop < 0 || endLoop < startLoop || endLoop > end {
				return fmt.Errorf("the instrument %q has an invalid sample loop", instrument.Name)
			}
		}
	}

	return nil
}

// SampleCacheStats reports the state of the shared LRU sample page cache.
// Eagerly loaded SoundFonts, whose samples live in WaveData, return three
// zeroes.
func (sf *SoundFont) SampleCacheStats() (residentPages int, capacityPages int, loads uint64) {
	if sf == nil || sf.pageCache == nil {
		return 0, 0, 0
	}
	return sf.pageCache.resident(), sf.pageCache.maxPages, sf.pageCache.loadCount
}

// samplePage is one fixed-size window into the smpl pool, indexed by the
// absolute sample coordinate of its first frame.
type samplePage struct {
	id     int32
	frames []int16
}

// samplePageCache is a SoundFont-wide, bounded LRU cache of sample pages.
// Its metadata is O(maxPages) and at most maxPages int16 pages plus one
// page-sized byte scratch buffer are ever allocated.
type samplePageCache struct {
	source     io.ReaderAt
	dataOffset int64
	frameCount int64
	pageFrames int64
	maxPages   int

	pages     []*samplePage
	pageIDs   []int32
	lastUsed  []uint64
	clock     uint64
	loadCount uint64

	scratch []byte
}

func newSamplePageCache(source io.ReaderAt, dataOffset int64, frameCount int64, pageFrames, maxPages int) *samplePageCache {
	pageIDs := make([]int32, maxPages)
	for i := range pageIDs {
		pageIDs[i] = -1
	}
	return &samplePageCache{
		source:     source,
		dataOffset: dataOffset,
		frameCount: frameCount,
		pageFrames: int64(pageFrames),
		maxPages:   maxPages,
		pages:      make([]*samplePage, maxPages),
		pageIDs:    pageIDs,
		lastUsed:   make([]uint64, maxPages),
		scratch:    make([]byte, pageFrames*2),
	}
}

func (c *samplePageCache) resident() int {
	count := 0
	for _, id := range c.pageIDs {
		if id >= 0 {
			count++
		}
	}
	return count
}

// findSlot returns the slot holding the page, or -1 on a miss.
func (c *samplePageCache) findSlot(pageID int32) int {
	for slot := 0; slot < c.maxPages; slot++ {
		if c.pageIDs[slot] == pageID {
			c.clock++
			c.lastUsed[slot] = c.clock
			return slot
		}
	}
	return -1
}

// evictSlot picks an unused slot when available, otherwise the least
// recently used one.
func (c *samplePageCache) evictSlot() int {
	for slot := 0; slot < c.maxPages; slot++ {
		if c.pageIDs[slot] < 0 {
			return slot
		}
	}
	victim := 0
	for slot := 1; slot < c.maxPages; slot++ {
		if c.lastUsed[slot] < c.lastUsed[victim] {
			victim = slot
		}
	}
	return victim
}

// loadPage reads exactly one page. A failed read never enters the cache.
func (c *samplePageCache) loadPage(pageID int32) (*samplePage, error) {
	firstFrame := int64(pageID) * c.pageFrames
	if firstFrame < 0 || firstFrame >= c.frameCount {
		return nil, fmt.Errorf("sample page %d is outside the smpl pool", pageID)
	}

	frameCount := c.pageFrames
	if remaining := c.frameCount - firstFrame; remaining < frameCount {
		frameCount = remaining
	}
	byteCount := frameCount * 2

	offset := c.dataOffset + firstFrame*2
	n, err := c.source.ReadAt(c.scratch[:byteCount], offset)
	if err != nil {
		return nil, err
	}
	if int64(n) != byteCount {
		return nil, io.ErrUnexpectedEOF
	}

	slot := c.evictSlot()
	page := c.pages[slot]
	if page == nil {
		page = &samplePage{frames: make([]int16, c.pageFrames)}
		c.pages[slot] = page
	}

	for i := int64(0); i < frameCount; i++ {
		page.frames[i] = int16(binary.LittleEndian.Uint16(c.scratch[i*2:]))
	}

	page.id = pageID
	c.pageIDs[slot] = pageID
	c.clock++
	c.lastUsed[slot] = c.clock
	c.loadCount++

	return page, nil
}

// sampleAt returns one frame from the absolute smpl coordinate. Hits perform
// no I/O; misses read exactly the single page holding the frame.
func (c *samplePageCache) sampleAt(index int32) (int16, error) {
	if index < 0 || int64(index) >= c.frameCount {
		return 0, fmt.Errorf("sample index %d is outside the smpl pool", index)
	}

	pageID := int32(int64(index) / c.pageFrames)
	localIndex := int64(index) - int64(pageID)*c.pageFrames

	if slot := c.findSlot(pageID); slot >= 0 {
		return c.pages[slot].frames[localIndex], nil
	}

	page, err := c.loadPage(pageID)
	if err != nil {
		return 0, err
	}
	return page.frames[localIndex], nil
}

// A sampler resolves individual sample frames. Eager fonts serve them from
// the fully loaded WaveData, paged fonts through the shared page cache.
type sampler interface {
	sampleAt(index int32) (int16, error)
}

type eagerSampler struct {
	data []int16
}

func (s eagerSampler) sampleAt(index int32) (int16, error) {
	return s.data[index], nil
}

type pagedSampler struct {
	cache *samplePageCache
}

func (s pagedSampler) sampleAt(index int32) (int16, error) {
	return s.cache.sampleAt(index)
}

func (sf *SoundFont) newSampler() sampler {
	if sf.pageCache != nil {
		return pagedSampler{cache: sf.pageCache}
	}
	return eagerSampler{data: sf.WaveData}
}
