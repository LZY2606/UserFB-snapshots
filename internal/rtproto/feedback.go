package rtproto

// ExpandNACKWord decodes one RFC 4585 generic NACK control word into the
// inclusive 16-bit sequence span covering its losses. PID is the packet ID;
// BLP is the 16-bit bitmask of following packets (bit 0 => PID+1 ... bit 15
// => PID+16), all arithmetic modulo 2^16. Because BLP can be sparse, use
// ExpandNACKWordSeqs when the exact lost set matters.
func ExpandNACKWord(pid, blp uint16) NACKRange {
	last := int(pid)
	for bit := 0; bit < 16; bit++ {
		if blp&(1<<bit) != 0 {
			last = int(pid) + bit + 1
		}
	}
	return NACKRange{Start: pid, End: uint16(last & 0xffff)}
}

// ExpandNACKWordSeqs returns the exact set of sequence numbers asserted by one
// NACK control word, including holes left by a sparse BLP bitmask.
func ExpandNACKWordSeqs(pid, blp uint16) []uint16 {
	out := []uint16{pid}
	for bit := 0; bit < 16; bit++ {
		if blp&(1<<bit) != 0 {
			out = append(out, pid+uint16(bit)+1)
		}
	}
	return out
}

// NACKSet expands decoded NACK ranges into the set of individual 16-bit
// sequence numbers they cover. Sequences are stored raw (mod 2^16); the
// caller is responsible for attaching the correct SSRC generation.
func NACKSet(ranges []NACKRange) map[uint16]bool {
	out := make(map[uint16]bool)
	for _, r := range ranges {
		// Walk in modular space so a range crossing 65535->0 terminates.
		cur := r.Start
		for {
			out[cur] = true
			if cur == r.End {
				break
			}
			cur++
		}
	}
	return out
}

// MergeNACKSets unions any number of per-feedback NACK sequence sets. The
// result is a new map; inputs are never mutated. Overlapping feedback
// intervals (the same sequence reported by several RTCP packets) collapse to
// a single entry rather than being double counted.
func MergeNACKSets(sets ...map[uint16]bool) map[uint16]bool {
	merged := make(map[uint16]bool)
	for _, s := range sets {
		for seq := range s {
			merged[seq] = true
		}
	}
	return merged
}

// TWCCTransportSeqs returns every transport sequence number covered by a
// transport-wide feedback message, in ascending modular order starting at the
// base sequence. It deliberately works on raw 16-bit values; generation
// assignment happens in the identity layer.
func TWCCTransportSeqs(f *TWCCFeedback) []uint16 {
	if f == nil {
		return nil
	}
	var out []uint16
	cur := f.BaseSequence
	for _, run := range f.Runs {
		for i := 0; i < run.Count; i++ {
			out = append(out, cur)
			cur++
		}
	}
	return out
}
