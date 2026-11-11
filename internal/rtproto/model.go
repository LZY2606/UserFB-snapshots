package rtproto

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
)

// Direction marks which side of the transport observed the datagram.
type Direction string

const (
	// DirSend is a packet sent by the local media engine (RTP sender side).
	DirSend Direction = "send"
	// DirRecv is a packet received from the remote peer (feedback side).
	DirRecv Direction = "recv"
)

// Kind discriminates the datagram families stored in a capture.
type Kind string

const (
	KindRTP  Kind = "rtp"
	KindRTCP Kind = "rtcp"
)

// RTCP packet type constants (RFC 3550, RFC 4585, RFC 5104).
const (
	PTSR   = 200
	PTRR   = 201
	PTBYE  = 203
	PTAPP  = 204
	PTFB   = 205 // transport-layer feedback (RTPFB)
	PTPSFB = 206 // payload-specific feedback (PSFB)
)

// FMT constants. On the wire, generic NACK and transport-wide congestion
// control both encode as (PT=205, FMT=1); this capture format disambiguates
// TWCC with the synthetic FMT 15 so the two message families never collide in
// the stored model.
const (
	FMTNACK = 1
	FMTTWCC = 15
	FMTPLI  = 1
	FMTFIR  = 4
)

// RTPHeader stores the fields of an RTP header the analysis cares about
// (RFC 3550 section 5.1). CSRC and extension payload are preserved raw.
type RTPHeader struct {
	Version     int      `json:"version"`
	Padding     bool     `json:"padding"`
	Extension   bool     `json:"extension"`
	CSRCCount   int      `json:"csrc_count"`
	Marker      bool     `json:"marker"`
	PayloadType int      `json:"payload_type"`
	Sequence    uint16   `json:"sequence"`
	Timestamp   uint32   `json:"timestamp"`
	SSRC        uint32   `json:"ssrc"`
	CSRC        []uint32 `json:"csrc,omitempty"`
	ExtProfile  uint16   `json:"ext_profile,omitempty"`
	ExtWords    []uint32 `json:"ext_words,omitempty"`
}

// ReportBlock is one RTCP sender/receiver report block (RFC 3550 6.4.1).
type ReportBlock struct {
	SSRC           uint32 `json:"ssrc"`
	FractionLost   int    `json:"fraction_lost"`
	CumulativeLost int    `json:"cumulative_lost"`
	HighestSeq     uint32 `json:"highest_seq"` // extended highest sequence received
	Jitter         uint32 `json:"jitter"`
	LSR            uint32 `json:"lsr"`
	DLSR           uint32 `json:"dlsr"`
}

// SenderInfo is the 20-byte sender report block (RFC 3550 6.4.1).
type SenderInfo struct {
	NTPMSW       uint32 `json:"ntp_msw"`
	NTPLSW       uint32 `json:"ntp_lsw"`
	RTPTimestamp uint32 `json:"rtp_timestamp"`
	PacketCount  uint32 `json:"packet_count"`
	OctetCount   uint32 `json:"octet_count"`
}

// NACKRange is one decoded NACK control word: [Start, End] inclusive in 16-bit
// sequence space. A plain (non-BLP) loss is represented as Start == End.
type NACKRange struct {
	Start uint16 `json:"start"`
	End   uint16 `json:"end"`
}

// TWCCRun is one run-length or status-chunk decoded from a transport-wide
// feedback packet (draft-holmer-rmcat-transport-wide-cc-extensions). Only the
// transport sequence numbers plus receive delta timing are retained.
type TWCCRun struct {
	FirstTransportSeq uint16 `json:"first_transport_seq"`
	Count             int    `json:"count"`
	// DeltaMicros holds one receive-delta per received packet in the run
	// (negative deltas are allowed by the spec).
	DeltaMicros []int64 `json:"delta_micros,omitempty"`
}

// TWCCFeedback is the parsed content of an FMT=1 transport-wide message.
type TWCCFeedback struct {
	BaseSequence uint16    `json:"base_sequence"`
	PacketStatus uint8     `json:"packet_status_count"`
	ReferenceUS  int64     `json:"reference_time_us"` // 24-bit field * 64us
	Runs         []TWCCRun `json:"runs"`
}

// RTCPMessage is one parsed compound (or singleton) RTCP packet. The optional
// sub-structures are populated depending on PacketType/FMT.
type RTCPMessage struct {
	FMT          int           `json:"fmt,omitempty"`
	PacketType   int           `json:"packet_type"`
	SSRC         uint32        `json:"ssrc"` // sender SSRC of the RTCP packet
	MediaSSRC    uint32        `json:"media_ssrc,omitempty"`
	ReportBlocks []ReportBlock `json:"report_blocks,omitempty"`
	SenderInfo   *SenderInfo   `json:"sender_info,omitempty"`
	NACK         []NACKRange   `json:"nack,omitempty"`
	PLI          bool          `json:"pli,omitempty"`
	FIR          bool          `json:"fir,omitempty"`
	FIRSeq       uint8         `json:"fir_seq,omitempty"`
	TWCC         *TWCCFeedback `json:"twcc,omitempty"`
	Reason       string        `json:"reason,omitempty"` // BYE reason text
}

// Packet is one datagram from the capture. Exactly one of RTP/RTCP is set.
type Packet struct {
	// Index is the zero-based position in the original capture file and is
	// never reassigned, even after reordering analysis.
	Index     int           `json:"index"`
	TimeUS    int64         `json:"time_us"` // capture-side monotonic timestamp
	Direction Direction     `json:"direction"`
	Kind      Kind          `json:"kind"`
	RTP       *RTPHeader    `json:"rtp,omitempty"`
	RTCP      []RTCPMessage `json:"rtcp,omitempty"`
	// MID/RID are taken from RTP header extension ids configured at import.
	MID string `json:"mid,omitempty"`
	RID string `json:"rid,omitempty"`
	// TransportSeq is the TWCC transport-wide sequence from the RTP header
	// extension, when present.
	TransportSeq *uint16 `json:"transport_seq,omitempty"`
	// RawSize is the on-wire datagram size in bytes.
	RawSize int `json:"raw_size,omitempty"`
}

// StreamKey identifies the RTP identity tuple carried by extension headers.
type StreamKey struct {
	MID string `json:"mid"`
	RID string `json:"rid"`
}

func (k StreamKey) String() string {
	if k.RID == "" {
		return "mid=" + k.MID
	}
	return "mid=" + k.MID + " rid=" + k.RID
}

// DecodeError annotates a JSONL parse failure with the line number.
type DecodeError struct {
	Line int
	Err  error
}

func (e *DecodeError) Error() string {
	return fmt.Sprintf("capture line %d: %v", e.Line, e.Err)
}

// DecodeCapture reads JSON Lines until EOF. Packet indices are assigned from
// the line order when the record omits one; explicit indices are validated to
// be unique.
func DecodeCapture(r io.Reader) ([]Packet, error) {
	sc := bufio.NewScanner(r)
	// Captures can carry large RTCP compound packets; raise the line limit.
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	var out []Packet
	line := 0
	seenIndex := map[int]bool{}
	for sc.Scan() {
		line++
		raw := sc.Bytes()
		if len(bytesTrimSpace(raw)) == 0 {
			continue
		}
		var p Packet
		if err := json.Unmarshal(raw, &p); err != nil {
			return nil, &DecodeError{Line: line, Err: err}
		}
		if p.Index == 0 && len(out) > 0 {
			p.Index = line - 1
		}
		if seenIndex[p.Index] {
			return nil, &DecodeError{Line: line, Err: fmt.Errorf("duplicate packet index %d", p.Index)}
		}
		seenIndex[p.Index] = true
		out = append(out, p)
	}
	if err := sc.Err(); err != nil {
		return nil, &DecodeError{Line: line, Err: err}
	}
	return out, nil
}

func bytesTrimSpace(b []byte) []byte {
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t') {
		b = b[1:]
	}
	for len(b) > 0 && (b[len(b)-1] == ' ' || b[len(b)-1] == '\t' || b[len(b)-1] == '\n' || b[len(b)-1] == '\r') {
		b = b[:len(b)-1]
	}
	return b
}
