/*
MIT License

Copyright 2016 Comcast Cable Communications Management, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Package retime shifts the media and cue timestamps of a finite, in-order
// MPEG transport stream fragment so it can be spliced onto an existing
// playback timeline.
//
// Shift rewrites PES PTS/DTS values, adaptation field PCR/OPCR values and the
// pts_adjustment of plaintext SCTE-35 sections on caller supplied PIDs. It is
// not a repackager: packet count, order, PIDs, continuity counters, PUSI
// flags, adaptation field lengths, payload boundaries and every non-time byte
// are preserved. Each output packet is independently owned; the input packets
// are never modified.
package retime

import (
	"errors"
	"fmt"

	"github.com/Comcast/gots/v3"
	"github.com/Comcast/gots/v3/packet"
)

// MPEG timestamp domains.
const (
	// maxPTS33 is the modulus of the 33 bit PES PTS/DTS and SCTE-35
	// pts_adjustment timelines (2^33 90 kHz ticks).
	maxPTS33 int64 = 1 << 33
	// maxPCR is the modulus of the 42 bit PCR/OPCR timeline
	// (2^33 90 kHz base units, 300 extension ticks each).
	maxPCR int64 = maxPTS33 * 300
	// maxPID is the largest legal 13 bit transport stream PID.
	maxPID = 0x1FFF
)

// Config selects which elementary streams are retimed and by how much.
//
// PESPIDs carry MPEG-2 PES headers whose PTS and DTS are shifted. PCRPIDs
// carry adaptation fields whose PCR and OPCR are shifted. CuePIDs carry
// plaintext SCTE-35 sections whose pts_adjustment is shifted. A PID may appear
// in more than one role except that a PES PID and a cue PID can never be the
// same. Each list must contain distinct, in-range PIDs.
//
// Delta90k is the shift in 90 kHz units. PCR and OPCR move Delta90k*300
// extension ticks. Negative values shift the clocks backwards; all shifts are
// applied with the correct modular wraparound for their timestamp domain.
type Config struct {
	PESPIDs  []int
	PCRPIDs  []int
	CuePIDs  []int
	Delta90k int64
}

// Shift returns a copy of packets in which every timestamp belonging to the
// configured PIDs is moved by cfg.Delta90k. The input slice and its packets
// are left untouched; the returned packets own their own byte arrays and keep
// the original count and ordering. An empty input produces an empty, non-nil
// result without error.
//
// Validation only touches the selected PIDs. Any malformed PES header or
// SCTE-35 section, illegal timestamp marker bit, bad CRC, encryption hint or
// payload continuity counter break on a selected PID returns an error that
// names the packet index and PID, and no partially rewritten packets are
// returned.
func Shift(packets []*packet.Packet, cfg Config) ([]*packet.Packet, error) {
	roles, err := buildRoles(cfg)
	if err != nil {
		return nil, err
	}
	if len(packets) == 0 {
		return []*packet.Packet{}, nil
	}
	// Fold the shift into the 33 bit domain once; the same reduced value is
	// correct for PTS/DTS, pts_adjustment and (times 300) PCR/OPCR.
	delta := reduceDelta90k(cfg.Delta90k)

	out := make([]*packet.Packet, len(packets))
	cc := make(map[int]*ccState, len(roles.byPID))
	pes := make(map[int]*pesAssembler, len(roles.byPID))
	cue := make(map[int]*sectionAssembler, len(roles.byPID))

	// The whole fragment is rewritten up front: returning a partially edited
	// slice on error is not allowed, so every mutation lands on the private
	// output copy.
	for i, pkt := range packets {
		if pkt == nil {
			return nil, fmt.Errorf("retime: packet index %d is nil", i)
		}
		pid := packet.Pid(pkt)
		role := roles.byPID[pid]

		cp := &packet.Packet{}
		copy(cp[:], pkt[:])
		out[i] = cp

		if role == 0 {
			continue
		}

		if err := checkContinuity(i, pid, cp, cc); err != nil {
			return nil, err
		}

		if role&rolePCR != 0 {
			if err := shiftPacketClocks(i, pid, cp, delta); err != nil {
				return nil, err
			}
		}

		if packet.ContainsPayload(cp) {
			switch {
			case role&rolePES != 0:
				if err := feedPES(i, pid, cp, pes, delta); err != nil {
					return nil, err
				}
			case role&roleCue != 0:
				if err := feedCue(i, pid, cp, cue, delta); err != nil {
					return nil, err
				}
			}
		}
	}

	for pid, asm := range pes {
		if asm.active {
			return nil, fmt.Errorf("retime: PID %d PES header started at packet index %d is truncated at end of fragment", pid, asm.startIndex)
		}
	}
	for pid, asm := range cue {
		if asm.truncated() {
			return nil, fmt.Errorf("retime: PID %d SCTE-35 section started at packet index %d is truncated at end of fragment", pid, asm.startIndex)
		}
	}
	return out, nil
}

// role bits describe what a selected PID carries.
type role uint8

const (
	rolePES role = 1 << iota
	rolePCR
	roleCue
)

type pidRoles struct {
	byPID map[int]role
}

func buildRoles(cfg Config) (*pidRoles, error) {
	r := &pidRoles{byPID: make(map[int]role)}
	add := func(pids []int, bit role, name string) error {
		seen := make(map[int]bool, len(pids))
		for _, pid := range pids {
			if pid < 0 || pid > maxPID {
				return fmt.Errorf("retime: %s PID %d is outside the valid range 0..%d", name, pid, maxPID)
			}
			if seen[pid] {
				return fmt.Errorf("retime: PID %d is listed more than once in %s", pid, name)
			}
			seen[pid] = true
			existing := r.byPID[pid]
			if bit == rolePES && existing&roleCue != 0 {
				return fmt.Errorf("retime: PID %d cannot be both a PES PID and a cue PID", pid)
			}
			if bit == roleCue && existing&rolePES != 0 {
				return fmt.Errorf("retime: PID %d cannot be both a PES PID and a cue PID", pid)
			}
			r.byPID[pid] = existing | bit
		}
		return nil
	}
	if err := add(cfg.PESPIDs, rolePES, "PES"); err != nil {
		return nil, err
	}
	if err := add(cfg.PCRPIDs, rolePCR, "PCR"); err != nil {
		return nil, err
	}
	if err := add(cfg.CuePIDs, roleCue, "cue"); err != nil {
		return nil, err
	}
	return r, nil
}

// ccState tracks the payload continuity counter of one selected PID. Only
// packets carrying a payload advance the counter; adaptation-only and null
// payload packets are ignored.
type ccState struct {
	have bool
	cc   uint8
}

func checkContinuity(index, pid int, pkt *packet.Packet, states map[int]*ccState) error {
	if !packet.ContainsPayload(pkt) {
		return nil
	}
	st, ok := states[pid]
	if !ok {
		st = &ccState{}
		states[pid] = st
	}
	cc := packet.ContinuityCounter(pkt)
	if st.have && cc != (st.cc+1)&0x0F {
		return fmt.Errorf("retime: payload continuity counter break on PID %d at packet index %d: got %d, expected %d",
			pid, index, cc, (st.cc+1)&0x0F)
	}
	st.have = true
	st.cc = cc
	return nil
}

// shiftPacketClocks rewrites PCR and OPCR in place on the output packet.
// Packets without clock fields are left alone; fields are never added.
func shiftPacketClocks(index, pid int, pkt *packet.Packet, delta90k int64) error {
	if !packet.ContainsAdaptationField(pkt) || pkt[4] == 0 {
		return nil
	}
	afLen := int(pkt[4])
	// adaptation field occupies bytes 5 .. 4+afLen
	if 4+afLen > packet.PacketSize {
		return fmt.Errorf("retime: adaptation field length %d overflows packet on PID %d at packet index %d",
			afLen, pid, index)
	}
	flags := pkt[5]
	offset := 6

	if flags&0x10 != 0 { // PCR flag
		if offset+6 > 4+afLen {
			return fmt.Errorf("retime: truncated PCR on PID %d at packet index %d", pid, index)
		}
		shifted, err := shiftPCR(pkt[offset:offset+6], delta90k)
		if err != nil {
			return fmt.Errorf("retime: invalid PCR on PID %d at packet index %d: %w", pid, index, err)
		}
		gots.InsertPCR(pkt[offset:offset+6], shifted)
		offset += 6
	}
	if flags&0x08 != 0 { // OPCR flag
		if offset+6 > 4+afLen {
			return fmt.Errorf("retime: truncated OPCR on PID %d at packet index %d", pid, index)
		}
		shifted, err := shiftPCR(pkt[offset:offset+6], delta90k)
		if err != nil {
			return fmt.Errorf("retime: invalid OPCR on PID %d at packet index %d: %w", pid, index, err)
		}
		gots.InsertPCR(pkt[offset:offset+6], shifted)
	}
	return nil
}

// shiftPCR validates and shifts a 6 byte PCR/OPCR field, preserving the
// extension phase and the reserved 0x7e marker bits.
func shiftPCR(b []byte, delta90k int64) (uint64, error) {
	if b[4]&0x7E != 0x7E {
		return 0, errors.New("reserved marker bits are not 0x7E")
	}
	cur := int64(gots.ExtractPCR(b))
	shifted := mod64(cur+delta90k*300, maxPCR)
	return uint64(shifted), nil
}

// mod64 computes x mod m for arbitrary (including negative) int64 x.
func mod64(x, m int64) int64 {
	r := x % m
	if r < 0 {
		r += m
	}
	return r
}

// reduceDelta90k folds a 90 kHz shift into the 2^33 domain. Reducing before
// any multiplication keeps every later addition inside int64 range even when
// the caller passes values near the int64 limits. For PCR the reduced value is
// multiplied by 300, so the PCR extension phase is inherently preserved.
func reduceDelta90k(delta90k int64) int64 {
	return delta90k & (maxPTS33 - 1)
}
