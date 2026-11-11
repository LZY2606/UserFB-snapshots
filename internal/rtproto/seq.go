// Package rtproto contains the offline capture record model and the small
// set of RTP/RTCP parsing helpers the replay tool needs.
//
// The capture format is JSON Lines: one observed datagram per line, in the
// order it was seen. Keeping the original capture index on every record is a
// hard requirement — later analysis must always be able to point back at the
// exact packet the evidence came from.
package rtproto

// SeqMod is the modulus of the 16-bit RTP sequence number space (RFC 3550).
const SeqMod = 1 << 16

// HalfSeq is the wrap-around boundary: forward distances larger than this are
// interpreted as going backwards in sequence space.
const HalfSeq = SeqMod / 2

// SeqDelta returns the signed forward distance from sequence a to sequence b
// in 16-bit modular sequence space. A positive result means b is later than a
// under the modular comparison rule from RFC 3550 appendix A.1. Examples:
//
//	SeqDelta(65534, 1) == 3   // wrapped forward
//	SeqDelta(5, 2)     == -3 // reorder / late packet
func SeqDelta(a, b uint16) int {
	d := (int(b) - int(a)) % SeqMod
	if d < 0 {
		d += SeqMod
	}
	if d >= HalfSeq {
		d -= SeqMod
	}
	return d
}

// SeqUnwrapper converts 16-bit sequence numbers into a monotonic 64-bit
// extended sequence space. Reordered (late) packets get a stable extended
// value relative to the highest sequence seen so far; they never move the
// high-water mark backwards.
type SeqUnwrapper struct {
	started bool
	last    uint16
	ext     int64
}

// NewSeqUnwrapper returns an empty unwrapper.
func NewSeqUnwrapper() *SeqUnwrapper { return &SeqUnwrapper{} }

// Extend feeds one observed sequence number and returns its extended value.
// It is safe to feed reordered sequences.
func (u *SeqUnwrapper) Extend(seq uint16) int64 {
	if !u.started {
		u.started = true
		u.last = seq
		u.ext = int64(seq)
		return u.ext
	}
	ext := u.ext + int64(SeqDelta(u.last, seq))
	if SeqDelta(u.last, seq) > 0 {
		u.last = seq
		u.ext = ext
	}
	return ext
}

// Started reports whether any sequence has been fed in.
func (u *SeqUnwrapper) Started() bool { return u.started }
