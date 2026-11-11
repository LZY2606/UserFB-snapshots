package astits

import (
	"encoding/binary"
	"errors"
	"fmt"

	"github.com/asticode/go-astikit"
)

// SCTE-35 splice_info_section() support, implemented from ANSI/SCTE 35 2023r1
// ("Digital Program Insertion Cueing Message for Cable"), section 9. The 2023r1
// text is published at https://www.scte.org/standards/library/catalog/scte-35-digital-program-insertion-cueing-message-and-api/
const (
	scte35TableID uint8 = 0xfc

	// section_length covers every byte after the two length bytes, including the CRC_32.
	scte35MinSectionLength = 17 // fixed fields (11) + empty descriptor loop length (2) + CRC_32 (4)
	scte35MaxSectionLength = 1021
	scte35PTSMask          = uint64(0x1ffffffff) // 33-bit PTS space
	scte35ProtocolVersion  = 0
)

// SCTE-35 splice_command_type values that are parsed structurally. Every other
// declared command is delivered untouched through SCTE35Command.Raw.
const (
	SCTE35SpliceCommandTypeNull       uint8 = 0x00
	SCTE35SpliceCommandTypeInsert     uint8 = 0x05
	SCTE35SpliceCommandTypeTimeSignal uint8 = 0x06
)

// Errors returned for sections that cannot be parsed. Callers can use errors.Is
// to distinguish them; a half-parsed message is never delivered.
var (
	// ErrSCTE35InvalidSection is wrapped for any framing or length inconsistency,
	// including truncated commands and descriptor loops that overrun their region.
	ErrSCTE35InvalidSection = errors.New("astits: invalid SCTE-35 section")
	// ErrSCTE35Encrypted is returned when encrypted_packet indicates scrambling.
	ErrSCTE35Encrypted = errors.New("astits: encrypted SCTE-35 sections are not supported")
	// ErrSCTE35UnsupportedVersion is returned for protocol_version values other than 0.
	ErrSCTE35UnsupportedVersion = errors.New("astits: unsupported SCTE-35 protocol version")
	// ErrSCTE35UnboundedCommand is returned for the reserved splice_command_length 0xFFF.
	ErrSCTE35UnboundedCommand = errors.New("astits: SCTE-35 splice_command_length 0xFFF is not supported")
)

// SCTE35Data represents a parsed SCTE-35 splice_info_section(). It is only
// produced for PIDs that the current PMT declares with stream_type 0x86.
type SCTE35Data struct {
	// Section is a copy of the complete splice_info_section bytes, from table_id
	// through the trailing CRC_32. It never aliases the demuxer's reused buffers.
	Section []byte

	ProtocolVersion uint8
	// PTSAdjustment is the 33-bit pts_adjustment field in 90kHz units.
	PTSAdjustment uint64
	// Tier is the 12-bit tier field.
	Tier uint16

	Command     *SCTE35Command
	Descriptors []*SCTE35Descriptor
}

// SCTE35Descriptor represents one entry of the descriptor loop. Unknown
// descriptors (including segmentation descriptors) are preserved verbatim;
// their internal fields are not decoded.
type SCTE35Descriptor struct {
	Tag uint8
	// Data is a copy of the descriptor content, excluding the tag and length bytes.
	Data []byte
}

// SCTE35Command represents the splice_command. Exactly one of the typed
// pointers is set for the structurally supported commands; Raw is set for any
// other command type and keeps the declared command bytes verbatim.
type SCTE35Command struct {
	Type uint8
	// Raw is a copy of the command bytes when the command type is unknown.
	Raw        []byte
	Null       *SCTE35SpliceNull
	Insert     *SCTE35SpliceInsert
	TimeSignal *SCTE35TimeSignal
}

// SCTE35SpliceNull is the splice_null() command, which carries no fields.
type SCTE35SpliceNull struct{}

// SCTE35TimeSignal is the time_signal() command.
type SCTE35TimeSignal struct {
	// SpliceTime is nil when time_specified_flag is 0.
	SpliceTime *SCTE35SpliceTime
}

// SCTE35SpliceInsert is the splice_insert() command.
type SCTE35SpliceInsert struct {
	EventID      uint32
	OutOfNetwork bool
	// Canceled mirrors splice_event_cancel_indicator; a canceled event has no
	// schedule, duration or avail fields.
	Canceled bool
	// Immediate mirrors splice_immediate_flag and is only meaningful for a
	// program splice.
	Immediate bool
	// ProgramSplice mirrors program_splice_flag: true for a program splice,
	// false for a component splice.
	ProgramSplice bool

	// SpliceTime is the program splice time. It stays nil for immediate splices
	// and when program_splice_time carries time_specified_flag = 0, so an
	// unspecified time is never confused with pts_time 0.
	SpliceTime *SCTE35SpliceTime

	// Components holds the component tags for a component splice. A component
	// with time_specified_flag = 0 has a nil SpliceTime.
	Components []*SCTE35ComponentSplice

	// Duration is the optional break_duration() structure.
	Duration *SCTE35BreakDuration

	UniqueProgramID uint16
	AvailNum        uint8
	AvailsExpected  uint8
}

// SCTE35ComponentSplice is one component of a component splice.
type SCTE35ComponentSplice struct {
	Tag uint8
	// SpliceTime is nil when the component time_specified_flag is 0.
	SpliceTime *SCTE35SpliceTime
}

// SCTE35SpliceTime represents a splice_time() PTS value.
type SCTE35SpliceTime struct {
	// PTS is the raw 33-bit pts_time from the section.
	PTS uint64
	// AdjustedPTS is PTS plus the section pts_adjustment, wrapped to 33 bits.
	AdjustedPTS uint64
}

// SCTE35BreakDuration is a break_duration(). Duration is a length of time, so
// pts_adjustment is never applied to it.
type SCTE35BreakDuration struct {
	AutoReturn bool
	// Duration is the 33-bit break duration in 90kHz units.
	Duration uint64
}

// parseSCTE35Payload parses every splice_info_section contained in an assembled
// payload. Trailing 0xFF TS stuffing is ignored, while any other unexpected
// byte, bad length or bad CRC produces an error before any data is returned.
func parseSCTE35Payload(payload []byte, pid uint16, fp *Packet) (ds []*DemuxerData, err error) {
	i := astikit.NewBytesIterator(payload)
	for i.HasBytesLeft() {
		var tableID byte
		if tableID, err = i.NextByte(); err != nil {
			return nil, fmt.Errorf("%w: reading table_id failed: %v", ErrSCTE35InvalidSection, err)
		}

		// 0xFF stuffing marks the end of the section payload.
		if tableID == 0xff {
			break
		}
		if tableID != scte35TableID {
			return nil, fmt.Errorf("%w: table_id 0x%02x is not SCTE-35 on PID %d", ErrSCTE35InvalidSection, tableID, pid)
		}

		var hdr []byte
		if hdr, err = i.NextBytesNoCopy(2); err != nil {
			return nil, fmt.Errorf("%w: reading section length failed: %v", ErrSCTE35InvalidSection, err)
		}
		sectionLength := int(uint16(hdr[0]&0x0f)<<8 | uint16(hdr[1]))
		if sectionLength < scte35MinSectionLength || sectionLength > scte35MaxSectionLength {
			return nil, fmt.Errorf("%w: section_length %d is out of range", ErrSCTE35InvalidSection, sectionLength)
		}

		sectionStart := i.Offset() - 3 // table_id + the two length bytes
		totalLength := 3 + sectionLength
		if sectionStart+totalLength > len(payload) {
			return nil, fmt.Errorf("%w: section declares %d bytes but only %d are available", ErrSCTE35InvalidSection, totalLength, len(payload)-sectionStart)
		}

		// Work on an independent copy: the caller's payload comes from a reused pool buffer.
		section := make([]byte, totalLength)
		copy(section, payload[sectionStart:sectionStart+totalLength])

		crcOffset := totalLength - 4
		wantCRC := binary.BigEndian.Uint32(section[crcOffset:])
		if gotCRC := computeCRC32(section[:crcOffset]); gotCRC != wantCRC {
			return nil, errors.Join(ErrCRC32Mismatch, fmt.Errorf("astits: SCTE-35 section CRC32 %08x != computed CRC32 %08x", wantCRC, gotCRC))
		}

		var d *SCTE35Data
		if d, err = parseSCTE35Section(section); err != nil {
			return nil, err
		}
		ds = append(ds, &DemuxerData{SCTE35: d, FirstPacket: fp, PID: pid})

		i.Seek(sectionStart + totalLength)
	}
	return ds, nil
}

// parseSCTE35Section parses the body of one splice_info_section. The section has
// already been copied and CRC-checked.
func parseSCTE35Section(section []byte) (d *SCTE35Data, err error) {
	i := astikit.NewBytesIterator(section)
	// Skip table_id and the section length word.
	i.Skip(3)

	d = &SCTE35Data{Section: section}
	crcEnd := len(section) - 4 // everything before CRC_32

	if d.ProtocolVersion, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	if d.ProtocolVersion != scte35ProtocolVersion {
		return nil, fmt.Errorf("%w: protocol_version %d", ErrSCTE35UnsupportedVersion, d.ProtocolVersion)
	}

	// The first five bytes hold encrypted_packet (1 bit), encryption_algorithm
	// (6 bits) and pts_adjustment (33 bits); cw_index is a separate full byte.
	var enc []byte
	if enc, err = scte35ReadBytes(i, 5); err != nil {
		return nil, err
	}
	if enc[0]>>1 != 0 {
		return nil, fmt.Errorf("%w: encrypted_packet/encryption_algorithm=0x%02x", ErrSCTE35Encrypted, enc[0]>>1)
	}
	d.PTSAdjustment = (uint64(enc[0]&0x01) << 32) |
		uint64(enc[1])<<24 | uint64(enc[2])<<16 | uint64(enc[3])<<8 | uint64(enc[4])

	if _, err = scte35ReadByte(i); err != nil { // cw_index, unused while unencrypted
		return nil, err
	}

	var tierBytes []byte
	if tierBytes, err = scte35ReadBytes(i, 2); err != nil {
		return nil, err
	}
	d.Tier = uint16(tierBytes[0]&0x0f)<<8 | uint16(tierBytes[1])

	var cmdLenBytes []byte
	if cmdLenBytes, err = scte35ReadBytes(i, 2); err != nil {
		return nil, err
	}
	commandLength := int(uint16(cmdLenBytes[0]&0x0f)<<8 | uint16(cmdLenBytes[1]))
	if commandLength == 0xfff {
		return nil, ErrSCTE35UnboundedCommand
	}
	commandEnd := i.Offset() + commandLength
	// The descriptor loop length field must still fit after the command region.
	if commandEnd+2 > crcEnd || commandEnd < i.Offset() {
		return nil, fmt.Errorf("%w: splice_command_length %d overruns the section", ErrSCTE35InvalidSection, commandLength)
	}

	var commandType byte
	if commandType, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	if d.Command, err = parseSCTE35Command(i, commandType, commandEnd, d.PTSAdjustment); err != nil {
		return nil, err
	}
	if i.Offset() != commandEnd {
		return nil, fmt.Errorf("%w: command type 0x%02x consumed %d bytes, declared %d", ErrSCTE35InvalidSection, commandType, i.Offset()-(commandEnd-commandLength), commandLength)
	}

	var loopLenBytes []byte
	if loopLenBytes, err = scte35ReadBytes(i, 2); err != nil {
		return nil, err
	}
	descriptorLoopLength := int(binary.BigEndian.Uint16(loopLenBytes))
	descriptorEnd := i.Offset() + descriptorLoopLength
	if descriptorEnd != crcEnd {
		return nil, fmt.Errorf("%w: descriptor_loop_length %d does not end at CRC_32 (offset %d, CRC starts at %d)", ErrSCTE35InvalidSection, descriptorLoopLength, i.Offset(), crcEnd)
	}
	if d.Descriptors, err = parseSCTE35Descriptors(i, descriptorEnd); err != nil {
		return nil, err
	}

	return d, nil
}

// parseSCTE35Command parses the region bounded by commandEnd. Each length field
// is checked against that boundary so TS stuffing can never be read as a field.
func parseSCTE35Command(i *astikit.BytesIterator, commandType byte, commandEnd int, ptsAdjustment uint64) (c *SCTE35Command, err error) {
	c = &SCTE35Command{Type: commandType}
	switch commandType {
	case SCTE35SpliceCommandTypeNull:
		if i.Offset() != commandEnd {
			return nil, fmt.Errorf("%w: splice_null must be empty", ErrSCTE35InvalidSection)
		}
		c.Null = &SCTE35SpliceNull{}
	case SCTE35SpliceCommandTypeTimeSignal:
		c.TimeSignal = &SCTE35TimeSignal{}
		if c.TimeSignal.SpliceTime, err = parseSCTE35SpliceTime(i, commandEnd, ptsAdjustment); err != nil {
			return nil, err
		}
		if i.Offset() != commandEnd {
			return nil, fmt.Errorf("%w: time_signal overruns its command length", ErrSCTE35InvalidSection)
		}
	case SCTE35SpliceCommandTypeInsert:
		if c.Insert, err = parseSCTE35SpliceInsert(i, commandEnd, ptsAdjustment); err != nil {
			return nil, err
		}
	default:
		// Unknown but fully bounded: keep the declared command bytes (the
		// splice_command_type byte plus its content) and let the descriptor
		// loop parse normally afterwards.
		rest, err := scte35ReadBytes(i, commandEnd-i.Offset())
		if err != nil {
			return nil, err
		}
		c.Raw = append([]byte{commandType}, rest...)
	}
	return c, nil
}

// parseSCTE35SpliceInsert parses splice_insert() per SCTE 35 2023r1 section 9.6.2.
func parseSCTE35SpliceInsert(i *astikit.BytesIterator, commandEnd int, ptsAdjustment uint64) (in *SCTE35SpliceInsert, err error) {
	in = &SCTE35SpliceInsert{}

	var eventID []byte
	if eventID, err = scte35ReadBytes(i, 4); err != nil {
		return nil, err
	}
	in.EventID = binary.BigEndian.Uint32(eventID)

	var flags byte
	if flags, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	in.Canceled = flags&0x80 != 0
	if in.Canceled {
		// A canceled event contains only the cancel indicator and ends immediately.
		if i.Offset() != int(commandEnd) {
			return nil, fmt.Errorf("%w: canceled splice_insert must not carry more fields", ErrSCTE35InvalidSection)
		}
		return in, nil
	}

	var mode byte
	if mode, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	in.OutOfNetwork = mode&0x80 != 0
	in.ProgramSplice = mode&0x40 != 0
	hasDuration := mode&0x20 != 0
	in.Immediate = mode&0x10 != 0

	if in.ProgramSplice {
		if !in.Immediate {
			if in.SpliceTime, err = parseSCTE35SpliceTime(i, int(commandEnd), ptsAdjustment); err != nil {
				return nil, err
			}
		}
	} else {
		var componentCount byte
		if componentCount, err = scte35ReadByte(i); err != nil {
			return nil, err
		}
		if componentCount == 0 {
			return nil, fmt.Errorf("%w: component splice declares zero components", ErrSCTE35InvalidSection)
		}
		for n := byte(0); n < componentCount; n++ {
			comp := &SCTE35ComponentSplice{}
			if comp.Tag, err = scte35ReadByte(i); err != nil {
				return nil, err
			}
			if !in.Immediate {
				if comp.SpliceTime, err = parseSCTE35SpliceTime(i, int(commandEnd), ptsAdjustment); err != nil {
					return nil, err
				}
			}
			in.Components = append(in.Components, comp)
		}
	}

	if hasDuration {
		if in.Duration, err = parseSCTE35BreakDuration(i); err != nil {
			return nil, err
		}
	}

	var upid []byte
	if upid, err = scte35ReadBytes(i, 2); err != nil {
		return nil, err
	}
	in.UniqueProgramID = binary.BigEndian.Uint16(upid)
	if in.AvailNum, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	if in.AvailsExpected, err = scte35ReadByte(i); err != nil {
		return nil, err
	}

	if i.Offset() != int(commandEnd) {
		return nil, fmt.Errorf("%w: splice_insert overruns its command length", ErrSCTE35InvalidSection)
	}
	return in, nil
}

// parseSCTE35SpliceTime parses splice_time(). A nil result means the optional
// PTS was not specified (time_specified_flag = 0).
func parseSCTE35SpliceTime(i *astikit.BytesIterator, end int, ptsAdjustment uint64) (t *SCTE35SpliceTime, err error) {
	if i.Offset() >= end {
		return nil, fmt.Errorf("%w: missing splice_time flags", ErrSCTE35InvalidSection)
	}
	var flags byte
	if flags, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	if flags&0x80 == 0 {
		return nil, nil
	}
	if i.Offset()+4 > end {
		return nil, fmt.Errorf("%w: truncated pts_time", ErrSCTE35InvalidSection)
	}
	var ptsBytes []byte
	if ptsBytes, err = scte35ReadBytes(i, 4); err != nil {
		return nil, err
	}
	raw := (uint64(flags&0x01) << 32) | uint64(binary.BigEndian.Uint32(ptsBytes))
	return &SCTE35SpliceTime{
		PTS:         raw,
		AdjustedPTS: (raw + ptsAdjustment) & scte35PTSMask,
	}, nil
}

// parseSCTE35BreakDuration parses break_duration(). The value is a duration and
// pts_adjustment is deliberately not applied to it.
func parseSCTE35BreakDuration(i *astikit.BytesIterator) (bd *SCTE35BreakDuration, err error) {
	var flags byte
	if flags, err = scte35ReadByte(i); err != nil {
		return nil, err
	}
	var durationBytes []byte
	if durationBytes, err = scte35ReadBytes(i, 4); err != nil {
		return nil, err
	}
	return &SCTE35BreakDuration{
		AutoReturn: flags&0x80 != 0,
		Duration:   (uint64(flags&0x01) << 32) | uint64(binary.BigEndian.Uint32(durationBytes)),
	}, nil
}

// parseSCTE35Descriptors parses the descriptor loop up to end. Every descriptor
// is preserved with its tag and a copy of its raw content.
func parseSCTE35Descriptors(i *astikit.BytesIterator, end int) (ds []*SCTE35Descriptor, err error) {
	for i.Offset() < end {
		d := &SCTE35Descriptor{}
		if d.Tag, err = scte35ReadByte(i); err != nil {
			return nil, fmt.Errorf("%w: truncated descriptor tag: %v", ErrSCTE35InvalidSection, err)
		}
		var length byte
		if length, err = scte35ReadByte(i); err != nil {
			return nil, fmt.Errorf("%w: truncated descriptor length: %v", ErrSCTE35InvalidSection, err)
		}
		if i.Offset()+int(length) > end {
			return nil, fmt.Errorf("%w: descriptor (tag 0x%02x, length %d) overruns the descriptor loop", ErrSCTE35InvalidSection, d.Tag, length)
		}
		if d.Data, err = scte35ReadBytes(i, int(length)); err != nil {
			return nil, err
		}
		ds = append(ds, d)
	}
	return ds, nil
}

func scte35ReadByte(i *astikit.BytesIterator) (byte, error) {
	b, err := i.NextByte()
	if err != nil {
		return 0, fmt.Errorf("%w: %v", ErrSCTE35InvalidSection, err)
	}
	return b, nil
}

func scte35ReadBytes(i *astikit.BytesIterator, n int) ([]byte, error) {
	bs, err := i.NextBytes(n) // NextBytes returns an independent copy
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrSCTE35InvalidSection, err)
	}
	return bs, nil
}

// isSCTE35Complete reports whether the assembled payload contains at least one
// fully framed splice_info_section (or only TS stuffing). It only checks
// framing: content validation is left to the parser so that bad lengths and
// bad CRCs surface as errors as soon as the declared bytes are available.
func isSCTE35Complete(ps []*Packet) bool {
	var l int
	for _, p := range ps {
		l += len(p.Payload)
	}

	payload := bytesPool.get(l)
	defer bytesPool.put(payload)

	var o int
	for _, p := range ps {
		o += copy(payload.s[o:], p.Payload)
	}

	i := astikit.NewBytesIterator(payload.s)
	for i.HasBytesLeft() {
		b, err := i.NextByte()
		if err != nil {
			return false
		}
		if b == 0xff {
			return true // end stuffing
		}
		if b != scte35TableID {
			return true // let the parser produce the recognizable error
		}
		hdr, err := i.NextBytesNoCopy(2)
		if err != nil {
			return false
		}
		sectionLength := int(uint16(hdr[0]&0x0f)<<8 | uint16(hdr[1]))
		if i.Len()-i.Offset() < sectionLength {
			return false // wait for the continuation packets
		}
		i.Skip(sectionLength)
	}
	return true
}
