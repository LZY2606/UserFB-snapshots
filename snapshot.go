// Copyright 2014 Quoc-Viet Nguyen. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.

package modbus

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Snapshot read stages, used in error messages.
const (
	snapshotStageGuard    = "guard"
	snapshotStageSpan     = "span"
	snapshotStageFinalize = "finalize"
)

// ErrUnstableSnapshot is returned when no consistent snapshot could be
// obtained within the number of attempts given by SnapshotPlan.Attempts.
var ErrUnstableSnapshot = errors.New("modbus: snapshot remains unstable after all attempts")

// HoldingRegisterReader is the read side of a Modbus client. The Client
// interface already implements it, so an existing Client can be passed to
// ReadSnapshot directly.
type HoldingRegisterReader interface {
	ReadHoldingRegisters(address, quantity uint16) (results []byte, err error)
}

// RegisterSpan describes one contiguous block of holding registers to read
// as part of a snapshot. Spans are read in the order supplied by the caller;
// they are never merged, sorted or de-duplicated.
type RegisterSpan struct {
	Address  uint16
	Quantity uint16
}

// SnapshotPlan describes a guarded snapshot read.
//
// Guard is the address of the device's 16-bit version register: an odd value
// means the device is updating its measurements, an even value means a stable
// revision. Spans are only delivered when the guard reads identical even
// values before and after reading all of them.
type SnapshotPlan struct {
	Guard    uint16
	Spans    []RegisterSpan
	Attempts int
}

// RegisterSnapshot is the result of a consistent snapshot read.
//
// Sequence is the even guard value shared by every block. Blocks holds one
// independent byte slice per span, in plan order. Attempts is the number of
// rounds actually consumed.
type RegisterSnapshot struct {
	Sequence uint16
	Blocks   [][]byte
	Attempts int
}

// ReadSnapshot reads the spans of plan and only returns data that belongs to
// a single, stable revision of the device.
//
// Each attempt reads the guard register first. An odd guard means the device
// is updating, so the attempt ends immediately without reading any span.
// Otherwise all spans are read in order and the guard is read again; the
// data is delivered only when both guard reads return the same even value.
// Guard values are compared as wrapping uint16 sequence numbers, never as
// ordered integers.
//
// Transport errors and responses whose length does not match the requested
// quantity are returned immediately, annotated with the failing stage, span
// index and attempt number; they are not retried as device updates. When all
// attempts fail the consistency check, ErrUnstableSnapshot is returned and
// no partial data is exposed.
func ReadSnapshot(reader HoldingRegisterReader, plan SnapshotPlan) (RegisterSnapshot, error) {
	if err := validateSnapshotPlan(plan); err != nil {
		return RegisterSnapshot{}, err
	}

	for attempt := 1; attempt <= plan.Attempts; attempt++ {
		start, err := readGuard(reader, plan.Guard, attempt, snapshotStageGuard)
		if err != nil {
			return RegisterSnapshot{}, err
		}
		// Odd sequence: the device is in the middle of an update.
		if start%2 != 0 {
			continue
		}

		blocks := make([][]byte, len(plan.Spans))
		for i, span := range plan.Spans {
			data, err := reader.ReadHoldingRegisters(span.Address, span.Quantity)
			if err != nil {
				return RegisterSnapshot{}, fmt.Errorf(
					"modbus: snapshot %s read error at span %d on attempt %d: %w",
					snapshotStageSpan, i, attempt, err)
			}
			expected := int(span.Quantity) * 2
			if len(data) != expected {
				return RegisterSnapshot{}, fmt.Errorf(
					"modbus: snapshot %s response length %d does not match expected %d at span %d on attempt %d",
					snapshotStageSpan, len(data), expected, i, attempt)
			}
			// The reader may reuse one underlying buffer for every call, so
			// keep an independent copy of each block.
			blocks[i] = append([]byte(nil), data...)
		}

		end, err := readGuard(reader, plan.Guard, attempt, snapshotStageFinalize)
		if err != nil {
			return RegisterSnapshot{}, err
		}
		// Equality only: wrapping versions (e.g. 65534 -> 0) must not be
		// treated as ordered, monotonic integers.
		if end == start {
			return RegisterSnapshot{
				Sequence: end,
				Blocks:   blocks,
				Attempts: attempt,
			}, nil
		}
		// Mismatched revisions: discard the whole round and try again.
	}

	return RegisterSnapshot{}, ErrUnstableSnapshot
}

// validateSnapshotPlan checks the whole plan before any I/O happens so that
// an invalid plan never issues a single read.
func validateSnapshotPlan(plan SnapshotPlan) error {
	if plan.Attempts <= 0 {
		return fmt.Errorf("modbus: snapshot attempts '%v' must be greater than 0", plan.Attempts)
	}
	if len(plan.Spans) == 0 {
		return errors.New("modbus: snapshot plan must contain at least one span")
	}
	for i, span := range plan.Spans {
		if span.Quantity < 1 || span.Quantity > 125 {
			return fmt.Errorf(
				"modbus: snapshot span %d quantity '%v' must be between 1 and 125",
				i, span.Quantity)
		}
		// Register addresses span 0..65535, so address + quantity must not
		// exceed 65536.
		if int(span.Address)+int(span.Quantity) > 65536 {
			return fmt.Errorf(
				"modbus: snapshot span %d address '%v' plus quantity '%v' exceeds the register address space",
				i, span.Address, span.Quantity)
		}
	}
	return nil
}

// readGuard reads the single guard register and decodes its big-endian
// uint16 version. stage identifies whether it is the opening or closing
// guard read of the attempt.
func readGuard(reader HoldingRegisterReader, address uint16, attempt int, stage string) (uint16, error) {
	data, err := reader.ReadHoldingRegisters(address, 1)
	if err != nil {
		return 0, fmt.Errorf("modbus: snapshot %s read error on attempt %d: %w", stage, attempt, err)
	}
	if len(data) != 2 {
		return 0, fmt.Errorf(
			"modbus: snapshot %s response length %d does not match expected 2 on attempt %d",
			stage, len(data), attempt)
	}
	return binary.BigEndian.Uint16(data), nil
}
