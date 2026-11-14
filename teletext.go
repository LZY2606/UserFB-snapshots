package astisub

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"math/bits"
	"sort"
	"strings"
	"time"

	"github.com/asticode/go-astikit"
	"github.com/asticode/go-astits"
)

// Errors
var (
	ErrNoValidTeletextPID     = errors.New("astisub: no valid teletext PID")
	ErrTeletextTimeRegression = errors.New("astisub: teletext timestamps must be non-decreasing")
)

type teletextCharset [96][]byte

type teletextNationalSubset [13][]byte

// Teletext PES data types
const (
	teletextPESDataTypeEBU     = "EBU"
	teletextPESDataTypeUnknown = "unknown"
)

func teletextPESDataType(dataIdentifier uint8) string {
	switch {
	case dataIdentifier >= 0x10 && dataIdentifier <= 0x1f:
		return teletextPESDataTypeEBU
	}
	return teletextPESDataTypeUnknown
}

// Teletext PES data unit ids
const (
	teletextPESDataUnitIDEBUNonSubtitleData = 0x2
	teletextPESDataUnitIDEBUSubtitleData    = 0x3
	teletextPESDataUnitIDStuffing           = 0xff
)

// TeletextOptions represents teletext options
type TeletextOptions struct {
	Page int
	PID  int
}

// ReadFromTeletext parses a teletext content
// http://www.etsi.org/deliver/etsi_en/300400_300499/300472/01.03.01_60/en_300472v010301p.pdf
// http://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf
// TODO Update README
func ReadFromTeletext(r io.Reader, o TeletextOptions) (s *Subtitles, err error) {
	// Init
	s = &Subtitles{}
	var dmx = astits.NewDemuxer(context.Background(), r)

	// Get the teletext PID
	var pid uint16
	if pid, err = teletextPID(dmx, o); err != nil {
		if err != ErrNoValidTeletextPID {
			err = fmt.Errorf("astisub: getting teletext PID failed: %w", err)
		}
		return
	}

	// Page assembly goes through the same incremental entry as live streams
	dec := NewTeletextDecoder(o.Page)

	// Loop in data
	var lastTime time.Time
	var d *astits.DemuxerData
	for {
		// Fetch next data
		if d, err = dmx.NextData(); err != nil {
			if err == astits.ErrNoMorePackets {
				err = nil
				break
			}
			err = fmt.Errorf("astisub: fetching next data failed: %w", err)
			return
		}

		// We only parse PES data
		if d.PES == nil {
			continue
		}

		// This data is not of interest to us
		if d.PID != pid || d.PES.Header.StreamID != astits.StreamIDPrivateStream1 {
			continue
		}

		// Get time
		t := teletextDataTime(d)
		if t.IsZero() {
			continue
		}
		lastTime = t

		// Push payload and collect pages closed by this PES
		var out *Subtitles
		if out, err = dec.Push(d.PES.Data, t); err != nil {
			return
		}
		s.Items = append(s.Items, out.Items...)
	}

	// Dump the last open page
	var out *Subtitles
	if out, err = dec.Flush(lastTime); err != nil {
		return
	}
	s.Items = append(s.Items, out.Items...)
	return
}

// TODO Add tests
func teletextDataTime(d *astits.DemuxerData) time.Time {
	if d.PES.Header != nil && d.PES.Header.OptionalHeader != nil && d.PES.Header.OptionalHeader.PTS != nil {
		return d.PES.Header.OptionalHeader.PTS.Time()
	} else if d.FirstPacket != nil && d.FirstPacket.AdaptationField != nil && d.FirstPacket.AdaptationField.PCR != nil {
		return d.FirstPacket.AdaptationField.PCR.Time()
	}
	return time.Time{}
}

// If the PID teletext option is not indicated, it will walk through the ts data until it reaches a PMT packet to
// detect the first valid teletext PID
// TODO Add tests
func teletextPID(dmx *astits.Demuxer, o TeletextOptions) (pid uint16, err error) {
	// PID is in the options
	if o.PID > 0 {
		pid = uint16(o.PID)
		return
	}

	// Loop in data
	var d *astits.DemuxerData
	for {
		// Fetch next data
		if d, err = dmx.NextData(); err != nil {
			if err == astits.ErrNoMorePackets {
				err = ErrNoValidTeletextPID
				return
			}
			err = fmt.Errorf("astisub: fetching next data failed: %w", err)
			return
		}

		// PMT data
		if d.PMT != nil {
			// Retrieve valid teletext PIDs
			var pids []uint16
			for _, s := range d.PMT.ElementaryStreams {
				for _, dsc := range s.ElementaryStreamDescriptors {
					if dsc.Tag == astits.DescriptorTagTeletext || dsc.Tag == astits.DescriptorTagVBITeletext {
						pids = append(pids, s.ElementaryPID)
					}
				}
			}

			// No valid teletext PIDs
			if len(pids) == 0 {
				err = ErrNoValidTeletextPID
				return
			}

			// Set pid
			pid = pids[0]
			log.Printf("astisub: no teletext pid specified, using pid %d", pid)

			// Rewind
			if _, err = dmx.Rewind(); err != nil {
				err = fmt.Errorf("astisub: rewinding failed: %w", err)
				return
			}
			return
		}
	}
}

// TeletextDecoder incrementally assembles teletext subtitle pages from PES
// payloads. A decoder is not safe for concurrent use.
type TeletextDecoder struct {
	// Target page identity. When page is 0, the first page with the C6
	// subtitle control bit set becomes the target.
	page       int
	targetMag  uint8
	targetPage int
	selected   bool

	// Character set state. M/29 defaults are kept per magazine and survive
	// page switches and automatic page selection; X/28 only overrides the
	// currently received page.
	cd *teletextCharacterDecoder

	// Currently received page
	current        *teletextPage
	receiving      bool
	pageTriplet    uint32
	pageTripletSet bool

	// Timing. The first Push timestamp is the output time zero.
	firstTime time.Time
	lastTime  time.Time
	flushed   bool
}

// NewTeletextDecoder creates a teletext decoder. page follows the
// TeletextOptions convention: 0 automatically selects the first page carrying
// the C6 subtitle flag.
func NewTeletextDecoder(page int) *TeletextDecoder {
	d := &TeletextDecoder{
		page: page,
		cd:   newTeletextCharacterDecoder(),
	}
	// An explicit target page is fixed from construction
	if page != 0 {
		d.targetMag = uint8(page / 100)
		d.targetPage = page % 100
		d.selected = true
	}
	return d
}

// Push feeds one complete PES.Data, including the data_identifier and without
// the PES header, timestamped at. The first Push sets the output time origin;
// later timestamps must be non-decreasing. Subtitles of pages closed by this
// payload are returned immediately. The returned object is owned by the
// caller: subsequent inputs never modify it.
func (d *TeletextDecoder) Push(data []byte, at time.Time) (*Subtitles, error) {
	out := &Subtitles{}

	// Timestamps must be non-decreasing, the first one is the time origin
	if !d.firstTime.IsZero() && at.Before(d.lastTime) {
		return out, ErrTeletextTimeRegression
	}
	if d.firstTime.IsZero() {
		d.firstTime = at
	}
	d.lastTime = at

	// A flushed decoder cannot produce new pages
	if d.flushed {
		return out, nil
	}

	// Need at least a data identifier
	if len(data) == 0 {
		return out, nil
	}

	// Check data type
	if teletextPESDataType(data[0]) != teletextPESDataTypeEBU {
		return out, nil
	}

	// Loop through data units. An incomplete unit drops the rest of the
	// payload, but every complete preceding unit has already been processed.
	offset := 1
	for offset+2 <= len(data) {
		id := data[offset]
		length := int(data[offset+1])
		start := offset + 2
		end := start + length
		if end > len(data) {
			break
		}
		d.parseDataUnit(data[start:end], id, at, out)
		offset = end
	}
	return out, nil
}

// Flush closes the page still being received at time at. Repeated calls do
// not resend the page.
func (d *TeletextDecoder) Flush(at time.Time) (*Subtitles, error) {
	out := &Subtitles{}
	if !d.firstTime.IsZero() && at.Before(d.lastTime) {
		return out, ErrTeletextTimeRegression
	}
	if d.flushed {
		return out, nil
	}
	d.flushed = true
	d.lastTime = at
	d.closeCurrent(at, out)
	d.current = nil
	d.receiving = false
	return out, nil
}

// parseDataUnit parses one EBU teletext data unit. Bad framing, uncorrectable
// addresses and truncated payloads are ignored.
func (d *TeletextDecoder) parseDataUnit(i []byte, id uint8, t time.Time, out *Subtitles) {
	// Only subtitle data units carry pages
	if id != teletextPESDataUnitIDEBUSubtitleData {
		return
	}

	// A subtitle data unit holds the data control byte, framing code, two
	// address bytes and forty data bytes
	if len(i) < 44 {
		return
	}

	// Framing code
	if i[1] != 0xe4 {
		return
	}

	// Magazine number and packet number
	h1, ok := astikit.ByteHamming84Decode(i[2])
	if !ok {
		return
	}
	h2, ok := astikit.ByteHamming84Decode(i[3])
	if !ok {
		return
	}
	h := h2<<4 | h1
	magazineNumber := h & 0x7
	if magazineNumber == 0 {
		magazineNumber = 8
	}
	packetNumber := h >> 3

	d.parsePacket(i[4:44], magazineNumber, packetNumber, t, out)
}

func (d *TeletextDecoder) parsePacket(i []byte, magazineNumber, packetNumber uint8, t time.Time, out *Subtitles) {
	switch {
	case packetNumber == 0:
		d.parsePacketHeader(i, magazineNumber, t, out)
	case packetNumber >= 1 && packetNumber <= 25:
		// Rows only belong to the page currently being received
		if d.receiving && magazineNumber == d.current.id.magazine {
			d.parsePacketData(i, packetNumber)
		}
	case packetNumber == 28:
		// X/28 only overrides the current page
		if d.receiving && magazineNumber == d.current.id.magazine {
			d.parsePacket28And29(magazineNumber, i, true)
		}
	case packetNumber == 29:
		// M/29 updates magazine defaults even before automatic page selection
		d.parsePacket28And29(magazineNumber, i, false)
	}
}

func (d *TeletextDecoder) parsePacketHeader(i []byte, magazineNumber uint8, t time.Time, out *Subtitles) {
	// Page number units
	pageNumberUnits, ok := astikit.ByteHamming84Decode(i[0])
	if !ok {
		return
	}

	// Page number tens
	pageNumberTens, ok := astikit.ByteHamming84Decode(i[1])
	if !ok {
		return
	}
	pageNumber := int(pageNumberTens)*10 + int(pageNumberUnits)

	// 0xff is a reserved page number value
	if pageNumberTens == 0xf && pageNumberUnits == 0xf {
		return
	}

	// C5 --> C8, the C6 bit marks subtitle pages
	controlBits, ok := astikit.ByteHamming84Decode(i[5])
	if !ok {
		return
	}
	subtitleFlag := controlBits&0x8 > 0

	// Automatic page selection
	if !d.selected && d.page == 0 {
		if subtitleFlag {
			d.targetMag = magazineNumber
			d.targetPage = pageNumber
			d.selected = true
			log.Printf("astisub: no teletext page specified, using page %d%.2d", d.targetMag, d.targetPage)
		}
	}

	// C11 --> C14
	controlBits, ok = astikit.ByteHamming84Decode(i[7])
	if !ok {
		return
	}
	magazineSerial := controlBits&0x1 > 0
	charsetCode := controlBits >> 1

	headerID := teletextPageID{magazine: magazineNumber, page: pageNumber}
	targetID := teletextPageID{magazine: d.targetMag, page: d.targetPage}

	// A header can close the page currently being received. In serial
	// transmission any other page ends it; in parallel transmission only a
	// different page of the same magazine does.
	if d.receiving {
		switch {
		case headerID == d.current.id:
			// Next transmission of the same page: close and reopen below
			d.closeCurrent(t, out)
		case magazineSerial:
			d.closeCurrent(t, out)
		case !magazineSerial && magazineNumber == d.current.id.magazine:
			d.closeCurrent(t, out)
		}
	}

	// Only the target page opens a new reception
	if headerID != targetID {
		return
	}

	// X/28 overrides never leak onto the next page
	d.pageTripletSet = false
	d.current = newTeletextStreamPage(magazineNumber, pageNumber, charsetCode, t)
	d.receiving = true
}

// parsePacketData stores one row. Receiving the same row number again only
// replaces the row content.
func (d *TeletextDecoder) parsePacketData(i []byte, packetNumber uint8) {
	row := make([]byte, 40)
	for idx := 0; idx < 40; idx++ {
		v, ok := astikit.ByteParity(bits.Reverse8(i[idx]))
		if !ok {
			v = 0
		}
		row[idx] = v
	}
	d.current.setRow(packetNumber, row)
}

// parsePacket28And29 handles X/28 page overrides and M/29 magazine defaults.
func (d *TeletextDecoder) parsePacket28And29(magazineNumber uint8, i []byte, x28 bool) {
	// Designation code, Hamming 8/4 protected. Only designation 0 and 4 are
	// supported.
	designationCode, ok := astikit.ByteHamming84Decode(i[0])
	if !ok || (designationCode != 0 && designationCode != 4) {
		return
	}

	// First triplet, Hamming 24/18 protected. A double bit error drops the
	// whole update.
	triplet1, ok := teletextHamming2418Decode([3]byte{i[1], i[2], i[3]})
	if !ok {
		return
	}

	// Format selection happens after Hamming decoding: X/28 format 1 is not
	// supported
	if x28 && triplet1&0xf > 0 {
		return
	}

	if x28 {
		d.pageTriplet = triplet1
		d.pageTripletSet = true
	} else {
		d.cd.setMagazineTriplet(magazineNumber, triplet1)
	}
}

type teletextCharacterDecoder struct {
	// M/29 default triplet per magazine, slots 1 through 8 are used
	magazineTriplet [9]uint32
	magazineSet     [9]bool

	// Legacy global state used by updateCharset
	c                   teletextCharset
	lastPageCharsetCode *uint8
}

func newTeletextCharacterDecoder() *teletextCharacterDecoder {
	return &teletextCharacterDecoder{}
}

func (d *teletextCharacterDecoder) setMagazineTriplet(magazine uint8, triplet uint32) {
	if magazine < 1 || magazine > 8 {
		return
	}
	d.magazineTriplet[magazine] = triplet
	d.magazineSet[magazine] = true
}

// TODO Add tests
func (d *teletextCharacterDecoder) decode(i byte) []byte {
	if i < 0x20 {
		return []byte{}
	}
	return d.c[i-0x20]
}

// updateCharset resolves the charset without magazine context, used by the
// standalone page parser. Live assembly resolves charsets per page through
// teletextResolveCharset.
// TODO Add tests
func (d *teletextCharacterDecoder) updateCharset(pageCharsetCode *uint8, force bool) {
	// Charset is up to date
	if d.lastPageCharsetCode != nil && pageCharsetCode != nil &&
		*pageCharsetCode == *d.lastPageCharsetCode && !force {
		return
	}
	d.lastPageCharsetCode = pageCharsetCode
	var charsetCode uint8
	if pageCharsetCode != nil {
		charsetCode = *pageCharsetCode
	}
	d.c = teletextResolveCharset(0, charsetCode)
}

// teletextResolveCharset builds the G0 charset for a decoded X/28 or M/29
// triplet and a page header charset code. Combinations absent from the
// shipped tables fall back to the default Latin charset.
func teletextResolveCharset(triplet uint32, charsetCode uint8) teletextCharset {
	c := *teletextCharsetG0Latin
	if v1, ok := teletextCharsets[uint8((triplet&0x3f80)>>10)]; ok {
		if v2, ok := v1[charsetCode]; ok {
			c = *v2.g0
			if v2.national != nil {
				for k, v := range v2.national {
					c[teletextNationalSubsetCharactersPositionInG0[k]] = v
				}
			}
		}
	}
	return c
}

// teletextHamming24DataPositions are the 18 data bit positions (1-indexed) of
// an ETSI EN 300 706 Hamming 24/18 codeword, transmission order.
var teletextHamming24DataPositions = []uint{3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23}

// teletextHamming2418Decode decodes one triplet. DVB payload bytes are
// bit-reversed per byte compared with the ETSI transmission order. Single bit
// errors are corrected and double bit errors are reported as uncorrectable.
func teletextHamming2418Decode(b [3]byte) (uint32, bool) {
	r := [3]byte{bits.Reverse8(b[0]), bits.Reverse8(b[1]), bits.Reverse8(b[2])}
	w := uint32(r[0]) | uint32(r[1])<<8 | uint32(r[2])<<16

	// Five Hamming check bits cover positions 1..23; the DVB representation
	// complements them, hence the XOR with the all-ones mask.
	var check uint8
	for p := uint(1); p <= 23; p++ {
		if w&(1<<(p-1)) != 0 {
			check ^= uint8(p)
		}
	}
	syndrome := check ^ 0x1f
	oddParity := bits.OnesCount32(w)%2 == 1

	switch {
	case oddParity && syndrome == 0:
		// Clean codeword
	case !oddParity && syndrome == 0:
		// Single error on the overall parity bit, position 24
		w ^= 1 << 23
	case !oddParity && syndrome <= 24:
		// Single error at the position pointed to by the syndrome
		w ^= 1 << uint(syndrome-1)
	default:
		// Uncorrectable double bit error
		return 0, false
	}

	var data uint32
	for j, pos := range teletextHamming24DataPositions {
		if w&(1<<(pos-1)) != 0 {
			data |= 1 << uint(j)
		}
	}
	return data, true
}

type teletextPageID struct {
	magazine uint8
	page     int
}

type teletextPage struct {
	id          teletextPageID
	charsetCode uint8
	data        map[uint8][]byte
	end         time.Time
	rows        []int
	start       time.Time
}

// newTeletextPage creates a page without a stream identity, used by standalone
// page parsing.
func newTeletextPage(charsetCode uint8, start time.Time) *teletextPage {
	return &teletextPage{
		charsetCode: charsetCode,
		data:        make(map[uint8][]byte),
		start:       start,
	}
}

// newTeletextStreamPage creates a page received on a given magazine and page
// number.
func newTeletextStreamPage(magazine uint8, page int, charsetCode uint8, start time.Time) *teletextPage {
	p := newTeletextPage(charsetCode, start)
	p.id = teletextPageID{magazine: magazine, page: page}
	return p
}

// setRow stores a row, replacing any previous content for the same row number.
func (p *teletextPage) setRow(rowNumber uint8, row []byte) {
	if _, ok := p.data[rowNumber]; !ok {
		p.rows = append(p.rows, int(rowNumber))
	}
	p.data[rowNumber] = row
}

// closeCurrent renders and emits the current page if it carries text and has a
// strictly positive duration. Rendering happens immediately so later charset
// updates cannot change a page once it has been handed out.
func (d *TeletextDecoder) closeCurrent(at time.Time, out *Subtitles) {
	if !d.receiving || d.current == nil {
		d.receiving = false
		d.current = nil
		return
	}
	p := d.current
	d.receiving = false
	d.current = nil
	p.end = at

	// Empty pages and zero-duration pages are not output
	if len(p.data) == 0 || !p.end.After(p.start) {
		return
	}

	// Resolve the charset from the page override, else the magazine default
	triplet := uint32(0)
	if d.pageTripletSet {
		triplet = d.pageTriplet
	} else if d.cd.magazineSet[p.id.magazine] {
		triplet = d.cd.magazineTriplet[p.id.magazine]
	}
	cd := &teletextCharacterDecoder{c: teletextResolveCharset(triplet, p.charsetCode)}

	// Order rows once, duplicates have already been collapsed
	sort.Ints(p.rows)

	item := &Item{
		EndAt:   p.end.Sub(d.firstTime),
		StartAt: p.start.Sub(d.firstTime),
	}
	for _, rowNumber := range p.rows {
		parseTeletextRow(item, cd, nil, p.data[uint8(rowNumber)])
	}
	if len(item.Lines) > 0 {
		out.Items = append(out.Items, item)
	}
}

// parse renders a page without decoder context, kept for standalone page use.
func (p *teletextPage) parse(s *Subtitles, d *teletextCharacterDecoder, firstTime time.Time) {
	// Update charset
	d.updateCharset(astikit.UInt8Ptr(p.charsetCode), false)

	// No data
	if len(p.data) == 0 {
		return
	}

	// Order rows
	sort.Ints(p.rows)

	// Create item
	i := &Item{
		EndAt:   p.end.Sub(firstTime),
		StartAt: p.start.Sub(firstTime),
	}

	// Loop through rows
	for _, idxRow := range p.rows {
		parseTeletextRow(i, d, nil, p.data[uint8(idxRow)])
	}

	// Append item
	s.Items = append(s.Items, i)
}

type decoder interface {
	decode(i byte) []byte
}

type styler interface {
	hasBeenSet() bool
	hasChanged(s *StyleAttributes) bool
	parseSpacingAttribute(i byte)
	propagateStyleAttributes(s *StyleAttributes)
	update(sa *StyleAttributes)
}

func parseTeletextRow(i *Item, d decoder, fs func() styler, row []byte) {
	// Loop through columns
	var l = Line{}
	var li = LineItem{InlineStyle: &StyleAttributes{}}
	var started bool
	var s styler
	for _, v := range row {
		// Create specific styler
		if fs != nil {
			s = fs()
		}

		// Get spacing attributes
		var color *Color
		var doubleHeight, doubleSize, doubleWidth *bool
		switch v {
		case 0x0:
			color = ColorBlack
		case 0x1:
			color = ColorRed
		case 0x2:
			color = ColorGreen
		case 0x3:
			color = ColorYellow
		case 0x4:
			color = ColorBlue
		case 0x5:
			color = ColorMagenta
		case 0x6:
			color = ColorCyan
		case 0x7:
			color = ColorWhite
		case 0xa:
			started = false
		case 0xb:
			started = true
		case 0xc:
			doubleHeight = astikit.BoolPtr(false)
			doubleSize = astikit.BoolPtr(false)
			doubleWidth = astikit.BoolPtr(false)
		case 0xd:
			doubleHeight = astikit.BoolPtr(true)
		case 0xe:
			doubleWidth = astikit.BoolPtr(true)
		case 0xf:
			doubleSize = astikit.BoolPtr(true)
		default:
			if s != nil {
				s.parseSpacingAttribute(v)
			}
		}

		// Style has been set
		if color != nil || doubleHeight != nil || doubleSize != nil || doubleWidth != nil || (s != nil && s.hasBeenSet()) {
			// Style has changed
			if color != li.InlineStyle.TeletextColor || doubleHeight != li.InlineStyle.TeletextDoubleHeight ||
				doubleSize != li.InlineStyle.TeletextDoubleSize || doubleWidth != li.InlineStyle.TeletextDoubleWidth ||
				(s != nil && s.hasChanged(li.InlineStyle)) {
				// Line has started
				if started {
					// Append line item
					appendTeletextLineItem(&l, li, s)

					// Create new line item
					sa := &StyleAttributes{}
					*sa = *li.InlineStyle
					li = LineItem{InlineStyle: sa}
				}

				// Update style attributes
				if color != nil && color != li.InlineStyle.TeletextColor {
					li.InlineStyle.TeletextColor = color
				}
				if doubleHeight != nil && doubleHeight != li.InlineStyle.TeletextDoubleHeight {
					li.InlineStyle.TeletextDoubleHeight = doubleHeight
				}
				if doubleSize != nil && doubleSize != li.InlineStyle.TeletextDoubleSize {
					li.InlineStyle.TeletextDoubleSize = doubleSize
				}
				if doubleWidth != nil && doubleWidth != li.InlineStyle.TeletextDoubleWidth {
					li.InlineStyle.TeletextDoubleWidth = doubleWidth
				}
				if s != nil {
					s.update(li.InlineStyle)
				}
			}
		} else if started {
			// Append text
			li.Text += string(d.decode(v))
		}
	}

	// Append line item
	appendTeletextLineItem(&l, li, s)

	// Append line
	if len(l.Items) > 0 {
		i.Lines = append(i.Lines, l)
	}
}

func appendTeletextLineItem(l *Line, li LineItem, s styler) {
	// There's some text
	if len(strings.TrimSpace(li.Text)) > 0 {
		// Make sure inline style exists
		if li.InlineStyle == nil {
			li.InlineStyle = &StyleAttributes{}
		}

		// Get number of spaces before
		li.InlineStyle.TeletextSpacesBefore = astikit.IntPtr(0)
		for _, c := range li.Text {
			if c == ' ' {
				*li.InlineStyle.TeletextSpacesBefore++
			} else {
				break
			}
		}

		// Get number of spaces after
		li.InlineStyle.TeletextSpacesAfter = astikit.IntPtr(0)
		for idx := len(li.Text) - 1; idx >= 0; idx-- {
			if li.Text[idx] == ' ' {
				*li.InlineStyle.TeletextSpacesAfter++
			} else {
				break
			}
		}

		// Propagate style attributes
		li.InlineStyle.propagateTeletextAttributes()
		if s != nil {
			s.propagateStyleAttributes(li.InlineStyle)
		}

		// Append line item
		li.Text = strings.TrimSpace(li.Text)
		l.Items = append(l.Items, li)
	}
}
