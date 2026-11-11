// Copyright 2014 Quoc-Viet Nguyen. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.

package modbus

import (
	"encoding/binary"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

// scriptCall records one ReadHoldingRegisters invocation.
type scriptCall struct {
	address  uint16
	quantity uint16
}

// scriptStep is one canned response of a scriptReader.
type scriptStep struct {
	address  uint16
	quantity uint16
	data     []byte
	err      error
}

// scriptReader is a HoldingRegisterReader double that replays canned
// responses and records the exact call sequence.
type scriptReader struct {
	steps []scriptStep
	calls []scriptCall
	index int
}

func (r *scriptReader) ReadHoldingRegisters(address, quantity uint16) ([]byte, error) {
	r.calls = append(r.calls, scriptCall{address, quantity})
	if r.index >= len(r.steps) {
		return nil, fmt.Errorf("unexpected read at address %d quantity %d", address, quantity)
	}
	step := r.steps[r.index]
	r.index++
	if step.address != address || step.quantity != quantity {
		return nil, fmt.Errorf(
			"call %d: got address %d quantity %d, want address %d quantity %d",
			r.index, address, quantity, step.address, step.quantity)
	}
	return step.data, step.err
}

// reuseBufferReader returns every response out of one reused backing array,
// mimicking clients that hand out the same buffer on every call.
type reuseBufferReader struct {
	steps []scriptStep
	calls []scriptCall
	index int
	buf   [250]byte
}

func (r *reuseBufferReader) ReadHoldingRegisters(address, quantity uint16) ([]byte, error) {
	r.calls = append(r.calls, scriptCall{address, quantity})
	if r.index >= len(r.steps) {
		return nil, fmt.Errorf("unexpected read at address %d quantity %d", address, quantity)
	}
	step := r.steps[r.index]
	r.index++
	if step.address != address || step.quantity != quantity {
		return nil, fmt.Errorf(
			"call %d: got address %d quantity %d, want address %d quantity %d",
			r.index, address, quantity, step.address, step.quantity)
	}
	copy(r.buf[:], step.data)
	// Deliberately alias the single internal buffer every time.
	return r.buf[:len(step.data)], step.err
}

// noCallReader fails the test if any read is issued.
type noCallReader struct {
	t *testing.T
}

func (r noCallReader) ReadHoldingRegisters(address, quantity uint16) ([]byte, error) {
	r.t.Fatalf("no I/O expected, but ReadHoldingRegisters(%d, %d) was called", address, quantity)
	return nil, nil
}

func regValue(v uint16) []byte {
	b := make([]byte, 2)
	binary.BigEndian.PutUint16(b, v)
	return b
}

func guardStep(address, value uint16) scriptStep {
	return scriptStep{address: address, quantity: 1, data: regValue(value)}
}

func spanStep(address uint16, values ...uint16) scriptStep {
	data := make([]byte, 2*len(values))
	for i, v := range values {
		binary.BigEndian.PutUint16(data[i*2:], v)
	}
	return scriptStep{address: address, quantity: uint16(len(values)), data: data}
}

// TestReadSnapshotThreeAttempts replays the reference scenario:
// round 1 mixes revisions (2 -> 4), round 2 only sees an odd guard (5),
// round 3 is stable (6 -> 6) and is delivered.
func TestReadSnapshotThreeAttempts(t *testing.T) {
	const guard = uint16(7)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{100, 1}, {110, 1}},
		Attempts: 3,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(100, 10),
		spanStep(110, 22),
		guardStep(guard, 4), // round 1: revision changed, discard

		guardStep(guard, 5), // round 2: odd, spans must not be read

		guardStep(guard, 6),
		spanStep(100, 11),
		spanStep(110, 22),
		guardStep(guard, 6), // round 3: stable
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	if snap.Sequence != 6 {
		t.Fatalf("Sequence: expected 6, actual %d", snap.Sequence)
	}
	if snap.Attempts != 3 {
		t.Fatalf("Attempts: expected 3, actual %d", snap.Attempts)
	}
	wantBlocks := [][]byte{{0x00, 0x0B}, {0x00, 0x16}}
	if !reflect.DeepEqual(snap.Blocks, wantBlocks) {
		t.Fatalf("Blocks: expected %v, actual %v", wantBlocks, snap.Blocks)
	}

	wantCalls := []scriptCall{
		{guard, 1}, {100, 1}, {110, 1}, {guard, 1},
		{guard, 1},
		{guard, 1}, {100, 1}, {110, 1}, {guard, 1},
	}
	if !reflect.DeepEqual(reader.calls, wantCalls) {
		t.Fatalf("call sequence: expected %v, actual %v", wantCalls, reader.calls)
	}
}

// TestReadSnapshotWrapAround verifies versions are compared by equality,
// not as ordered integers: 65534 -> 0 is unstable while the next 0 -> 0
// round succeeds.
func TestReadSnapshotWrapAround(t *testing.T) {
	const guard = uint16(7)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{200, 2}},
		Attempts: 2,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 65534),
		spanStep(200, 0x1111, 0x2222),
		guardStep(guard, 0), // wrapped: not the same revision

		guardStep(guard, 0),
		spanStep(200, 0x3333, 0x4444),
		guardStep(guard, 0),
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	if snap.Sequence != 0 {
		t.Fatalf("Sequence: expected 0, actual %d", snap.Sequence)
	}
	if snap.Attempts != 2 {
		t.Fatalf("Attempts: expected 2, actual %d", snap.Attempts)
	}
	wantBlocks := [][]byte{{0x33, 0x33, 0x44, 0x44}}
	if !reflect.DeepEqual(snap.Blocks, wantBlocks) {
		t.Fatalf("Blocks: expected %v, actual %v", wantBlocks, snap.Blocks)
	}
}

// TestReadSnapshotUnstableExhausted checks that every failed round is
// retried and exhaustion yields ErrUnstableSnapshot with no partial data.
func TestReadSnapshotUnstableExhausted(t *testing.T) {
	const guard = uint16(7)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{100, 1}},
		Attempts: 2,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(100, 10),
		guardStep(guard, 4), // mismatch

		guardStep(guard, 3), // odd: no span read
	}}

	snap, err := ReadSnapshot(reader, plan)
	if !errors.Is(err, ErrUnstableSnapshot) {
		t.Fatalf("expected ErrUnstableSnapshot, actual %v", err)
	}
	if !reflect.DeepEqual(snap, RegisterSnapshot{}) {
		t.Fatalf("expected zero-value snapshot, actual %+v", snap)
	}
	wantCalls := []scriptCall{
		{guard, 1}, {100, 1}, {guard, 1},
		{guard, 1},
	}
	if !reflect.DeepEqual(reader.calls, wantCalls) {
		t.Fatalf("call sequence: expected %v, actual %v", wantCalls, reader.calls)
	}
}

// TestReadSnapshotSpanError verifies a transport error on the second span
// is surfaced immediately (not retried as an update) with stage, span and
// attempt context, and that the original error remains unwrappable.
func TestReadSnapshotSpanError(t *testing.T) {
	const guard = uint16(8)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{10, 1}, {20, 1}},
		Attempts: 3,
	}
	deviceErr := errors.New("device gone")
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(10, 99),
		{address: 20, quantity: 1, err: deviceErr},
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	if !errors.Is(err, deviceErr) {
		t.Fatalf("error should wrap the transport error, actual %v", err)
	}
	if errors.Is(err, ErrUnstableSnapshot) {
		t.Fatalf("transport error must not be reported as unstable: %v", err)
	}
	for _, want := range []string{"span", "span 1", "attempt 1"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error %q does not contain %q", err.Error(), want)
		}
	}
	if !reflect.DeepEqual(snap, RegisterSnapshot{}) {
		t.Fatalf("expected zero-value snapshot, actual %+v", snap)
	}
	if len(reader.calls) != 3 {
		t.Fatalf("expected exactly 3 reads before failure, actual %d: %v", len(reader.calls), reader.calls)
	}
}

// TestReadSnapshotGuardWrongLength covers a short opening guard response:
// no span may be read and the error must identify the guard stage.
func TestReadSnapshotGuardWrongLength(t *testing.T) {
	const guard = uint16(8)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{10, 1}},
		Attempts: 2,
	}
	reader := &scriptReader{steps: []scriptStep{
		{address: guard, quantity: 1, data: []byte{0x02}}, // one byte instead of two
	}}

	_, err := ReadSnapshot(reader, plan)
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	for _, want := range []string{"guard", "attempt 1", "expected 2"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error %q does not contain %q", err.Error(), want)
		}
	}
	if len(reader.calls) != 1 {
		t.Fatalf("expected only the guard read, actual calls %v", reader.calls)
	}
}

// TestReadSnapshotFinalizeGuardWrongLength covers a malformed closing guard.
func TestReadSnapshotFinalizeGuardWrongLength(t *testing.T) {
	const guard = uint16(8)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{10, 1}},
		Attempts: 2,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(10, 99),
		{address: guard, quantity: 1, data: []byte{0x00, 0x02, 0x00}}, // three bytes
	}}

	_, err := ReadSnapshot(reader, plan)
	if err == nil {
		t.Fatal("expected an error, got nil")
	}
	if !strings.Contains(err.Error(), "finalize") {
		t.Fatalf("error %q should identify the finalize stage", err.Error())
	}
	if len(reader.calls) != 3 {
		t.Fatalf("expected 3 reads, actual %v", reader.calls)
	}
}

// TestReadSnapshotResponseLength checks both short and long span responses.
func TestReadSnapshotResponseLength(t *testing.T) {
	cases := []struct {
		name string
		data []byte
	}{
		{"short", []byte{0x00}},
		{"long", []byte{0x00, 0x0A, 0x00, 0x0B}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			const guard = uint16(8)
			plan := SnapshotPlan{
				Guard:    guard,
				Spans:    []RegisterSpan{{10, 1}},
				Attempts: 1,
			}
			reader := &scriptReader{steps: []scriptStep{
				guardStep(guard, 2),
				{address: 10, quantity: 1, data: tc.data},
			}}

			_, err := ReadSnapshot(reader, plan)
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			for _, want := range []string{"span 0", "attempt 1", "expected 2"} {
				if !strings.Contains(err.Error(), want) {
					t.Fatalf("error %q does not contain %q", err.Error(), want)
				}
			}
		})
	}
}

// TestReadSnapshotLastLegalRegister checks the boundary of the 16-bit
// address space: address 65535 with quantity 1 is the last legal span.
func TestReadSnapshotLastLegalRegister(t *testing.T) {
	const guard = uint16(0)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{65535, 1}},
		Attempts: 1,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(65535, 0xABCD),
		guardStep(guard, 2),
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	if snap.Sequence != 2 || snap.Attempts != 1 {
		t.Fatalf("unexpected snapshot: %+v", snap)
	}
	want := [][]byte{{0xAB, 0xCD}}
	if !reflect.DeepEqual(snap.Blocks, want) {
		t.Fatalf("Blocks: expected %v, actual %v", want, snap.Blocks)
	}
}

// TestReadSnapshotInvalidPlanZeroIO validates the plan before any I/O:
// every malformed plan must be rejected without a single register read.
func TestReadSnapshotInvalidPlanZeroIO(t *testing.T) {
	goodSpan := RegisterSpan{100, 1}
	plans := []SnapshotPlan{
		{Guard: 1, Spans: []RegisterSpan{goodSpan}, Attempts: 0},
		{Guard: 1, Spans: nil, Attempts: 1},
		{Guard: 1, Spans: []RegisterSpan{{100, 0}}, Attempts: 1},
		{Guard: 1, Spans: []RegisterSpan{{100, 126}}, Attempts: 1},
		{Guard: 1, Spans: []RegisterSpan{{65535, 2}}, Attempts: 1},
		{Guard: 1, Spans: []RegisterSpan{{65534, 3}}, Attempts: 1},
	}
	reader := noCallReader{t: t}
	for i, plan := range plans {
		_, err := ReadSnapshot(reader, plan)
		if err == nil {
			t.Fatalf("plan %d: expected validation error, plan %+v", i, plan)
		}
	}
}

// TestReadSnapshotReusedBufferNoAliasing ensures blocks are independent
// copies even when the reader hands out the same backing array every time.
func TestReadSnapshotReusedBufferNoAliasing(t *testing.T) {
	const guard = uint16(7)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{100, 1}, {110, 1}},
		Attempts: 1,
	}
	reader := &reuseBufferReader{steps: []scriptStep{
		guardStep(guard, 6),
		spanStep(100, 10),
		spanStep(110, 22),
		guardStep(guard, 6),
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	wantBlocks := [][]byte{{0x00, 0x0A}, {0x00, 0x16}}
	if !reflect.DeepEqual(snap.Blocks, wantBlocks) {
		t.Fatalf("Blocks: expected %v, actual %v", wantBlocks, snap.Blocks)
	}
	if &snap.Blocks[0][0] == &snap.Blocks[1][0] {
		t.Fatal("blocks share the same backing array")
	}

	// Mutating one block must not touch the other one.
	snap.Blocks[0][0] = 0xFF
	if snap.Blocks[1][0] != 0x00 || snap.Blocks[1][1] != 0x16 {
		t.Fatalf("block 1 was altered by editing block 0: %v", snap.Blocks[1])
	}

	// The reader reusing its buffer after the call must not affect results.
	for i := range reader.buf {
		reader.buf[i] = 0xEE
	}
	if !reflect.DeepEqual(snap.Blocks[0], []byte{0xFF, 0x0A}) {
		t.Fatalf("block 0 aliases the reader buffer: %v", snap.Blocks[0])
	}
	if !reflect.DeepEqual(snap.Blocks[1], []byte{0x00, 0x16}) {
		t.Fatalf("block 1 aliases the reader buffer: %v", snap.Blocks[1])
	}
}

// TestReadSnapshotOverlappingSpans verifies spans are read in caller order
// without merging, sorting or de-duplicating overlaps and duplicates.
func TestReadSnapshotOverlappingSpans(t *testing.T) {
	const guard = uint16(7)
	plan := SnapshotPlan{
		Guard: guard,
		Spans: []RegisterSpan{
			{50, 2},
			{50, 2}, // exact duplicate
			{51, 1}, // overlaps the previous span
		},
		Attempts: 1,
	}
	reader := &scriptReader{steps: []scriptStep{
		guardStep(guard, 4),
		spanStep(50, 1, 2),
		spanStep(50, 3, 4),
		spanStep(51, 5),
		guardStep(guard, 4),
	}}

	snap, err := ReadSnapshot(reader, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	if len(snap.Blocks) != 3 {
		t.Fatalf("expected 3 blocks, actual %d", len(snap.Blocks))
	}
	wantCalls := []scriptCall{
		{guard, 1}, {50, 2}, {50, 2}, {51, 1}, {guard, 1},
	}
	if !reflect.DeepEqual(reader.calls, wantCalls) {
		t.Fatalf("call sequence: expected %v, actual %v", wantCalls, reader.calls)
	}
}

// minimalPackager is a test-only Packager that frames a PDU as function
// code followed by its data.
type minimalPackager struct{}

func (minimalPackager) Encode(pdu *ProtocolDataUnit) (adu []byte, err error) {
	adu = make([]byte, 1+len(pdu.Data))
	adu[0] = pdu.FunctionCode
	copy(adu[1:], pdu.Data)
	return adu, nil
}

func (minimalPackager) Verify(aduRequest, aduResponse []byte) error {
	if aduResponse[0] != aduRequest[0] {
		return fmt.Errorf("function code mismatch: request %d response %d",
			aduRequest[0], aduResponse[0])
	}
	return nil
}

func (minimalPackager) Decode(adu []byte) (pdu *ProtocolDataUnit, err error) {
	return &ProtocolDataUnit{FunctionCode: adu[0], Data: adu[1:]}, nil
}

// scriptTransporter is a controllable Transporter that answers function 3
// reads from a script, exercising the real client.send/ReadHoldingRegisters
// path together with NewClient2.
type scriptTransporter struct {
	steps []scriptStep
	calls []scriptCall
	index int
}

func (s *scriptTransporter) Send(aduRequest []byte) (aduResponse []byte, err error) {
	if aduRequest[0] != FuncCodeReadHoldingRegisters {
		return nil, fmt.Errorf("unexpected function code %d", aduRequest[0])
	}
	address := binary.BigEndian.Uint16(aduRequest[1:])
	quantity := binary.BigEndian.Uint16(aduRequest[3:])
	s.calls = append(s.calls, scriptCall{address, quantity})
	if s.index >= len(s.steps) {
		return nil, fmt.Errorf("unexpected transport read at address %d", address)
	}
	step := s.steps[s.index]
	s.index++
	if step.address != address || step.quantity != quantity {
		return nil, fmt.Errorf(
			"call %d: got address %d quantity %d, want address %d quantity %d",
			s.index, address, quantity, step.address, step.quantity)
	}
	if step.err != nil {
		return nil, step.err
	}
	// Function code + byte count + register bytes.
	aduResponse = make([]byte, 2+int(quantity)*2)
	aduResponse[0] = FuncCodeReadHoldingRegisters
	aduResponse[1] = byte(quantity * 2)
	copy(aduResponse[2:], step.data)
	return aduResponse, nil
}

// TestReadSnapshotWithNewClient2 drives ReadSnapshot through a real Client
// built with NewClient2 and a controllable transporter, instead of a
// HoldingRegisterReader double.
func TestReadSnapshotWithNewClient2(t *testing.T) {
	const guard = uint16(400)
	plan := SnapshotPlan{
		Guard:    guard,
		Spans:    []RegisterSpan{{100, 2}, {200, 1}},
		Attempts: 2,
	}
	transporter := &scriptTransporter{steps: []scriptStep{
		guardStep(guard, 2),
		spanStep(100, 0x0102, 0x0304),
		spanStep(200, 0x0506),
		guardStep(guard, 8), // round 1 changes underneath us

		guardStep(guard, 8),
		spanStep(100, 0x0A0B, 0x0C0D),
		spanStep(200, 0x0E0F),
		guardStep(guard, 8), // round 2 stable
	}}
	client := NewClient2(minimalPackager{}, transporter)

	snap, err := ReadSnapshot(client, plan)
	if err != nil {
		t.Fatalf("ReadSnapshot failed: %v", err)
	}
	if snap.Sequence != 8 {
		t.Fatalf("Sequence: expected 8, actual %d", snap.Sequence)
	}
	if snap.Attempts != 2 {
		t.Fatalf("Attempts: expected 2, actual %d", snap.Attempts)
	}
	wantBlocks := [][]byte{
		{0x0A, 0x0B, 0x0C, 0x0D},
		{0x0E, 0x0F},
	}
	if !reflect.DeepEqual(snap.Blocks, wantBlocks) {
		t.Fatalf("Blocks: expected %v, actual %v", wantBlocks, snap.Blocks)
	}
	wantCalls := []scriptCall{
		{guard, 1}, {100, 2}, {200, 1}, {guard, 1},
		{guard, 1}, {100, 2}, {200, 1}, {guard, 1},
	}
	if !reflect.DeepEqual(transporter.calls, wantCalls) {
		t.Fatalf("call sequence: expected %v, actual %v", wantCalls, transporter.calls)
	}
}
