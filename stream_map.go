package astits

// elementaryStreamMap represents an elementary stream ids map
type elementaryStreamMap struct {
	// We use map[uint32] instead map[uint16] as go runtime provide optimized hash functions for (u)int32/64 keys
	es map[uint32]uint16 // map[StreamID]ProgramNumber

	// scte35 holds the PIDs declared with stream_type 0x86 (SCTE-35) by the
	// current PMTs. Only those PIDs are parsed as splice_info_section streams.
	scte35 map[uint32]struct{}
}

// newElementaryStreamMap creates a new elementary stream ids map
func newElementaryStreamMap() *elementaryStreamMap {
	return &elementaryStreamMap{
		es:     make(map[uint32]uint16),
		scte35: make(map[uint32]struct{}),
	}
}

// setUnlocked sets a new stream id to the elementary stream
func (m elementaryStreamMap) setUnlocked(pid, number uint16) {
	m.es[uint32(pid)] = number
}

// existsUnlocked checks whether the stream with this pid exists
func (m elementaryStreamMap) existsUnlocked(pid uint16) (ok bool) {
	_, ok = m.es[uint32(pid)]
	return
}

func (m elementaryStreamMap) unsetUnlocked(pid uint16) {
	delete(m.es, uint32(pid))
}

// setSCTE35Unlocked marks a PID as an SCTE-35 splice_info_section stream.
func (m elementaryStreamMap) setSCTE35Unlocked(pid uint16) {
	m.scte35[uint32(pid)] = struct{}{}
}

// unsetSCTE35Unlocked removes the SCTE-35 marking for a PID.
func (m elementaryStreamMap) unsetSCTE35Unlocked(pid uint16) {
	delete(m.scte35, uint32(pid))
}

// isSCTE35Unlocked checks whether the PMT declared this PID with stream_type 0x86.
func (m elementaryStreamMap) isSCTE35Unlocked(pid uint16) (ok bool) {
	_, ok = m.scte35[uint32(pid)]
	return
}
