// Package op evaluates the small DWARF expression stack language used by
// Delve location descriptions.
//
// # Offline location writes
//
// PlanLocationWrite adds a bit-level write planner for optimized binaries. It
// evaluates DWARF expressions with the same opcode and location-finalization
// code as ExecuteStackProgram, converts selected logical variable bits into
// physical register/memory bits, checks aliases, and reads prepared original
// bytes. LocationWritePlan.Commit rechecks those originals against a newer
// snapshot before issuing any write.
//
// The write planner is intentionally offline: it never starts, stops, or
// otherwise controls a debugged process. Register updates are submitted to
// DwarfRegisters.ChangeFunc and memory updates to the caller-supplied memory
// write callback.
//
// Supported input scope:
//
//   - little-endian register and memory layouts;
//   - ptrSize 4 or 8;
//   - bitSize at most MaxLocationWriteBits (1 << 20);
//   - DW_OP_piece (byte-sized pieces) and DW_OP_bit_piece (bit-sized pieces
//     with a source bit offset);
//   - register locations, including Bytes values wider than eight bytes;
//   - DW_OP_stack_value and DW_OP_implicit_value, treated as immutable
//     constants whose selected bits must equal the requested bits;
//   - ordinary memory addresses, address arithmetic, DW_OP_breg*,
//     DW_OP_fbreg, DW_OP_call_frame_cfa, and dereference opcodes;
//   - unavailable/empty pieces and short composite tails, which are valid
//     planning inputs but fail if a selected logical bit lands in them.
//
// The legacy ExecuteStackProgram API remains byte-piece oriented and continues
// to reject DW_OP_bit_piece explicitly. Use PlanLocationWrite when bit pieces
// are required.
package op
