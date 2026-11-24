package fixture

import "encoding/binary"

// Analysis supplies raw inputs only. Memory is an offline 16-byte object at
// target address 0x4000; addresses are never host pointers.
// The old base ID 1 corresponds to replacement base ID 2.
func Analysis(order binary.ByteOrder) (oldBase, split, newBase, memory []byte) {
	oldBase, split = Original(order)
	names := "\x00unrelated_padding_type\x00int\x00"
	types := Words(order, 1, 1<<24, 8, 64)
	types = append(types, Words(order, 24, 1<<24, 4, 1<<24|32)...)
	newBase = Blob(order, types, names)
	memory = make([]byte, 16)
	order.PutUint64(memory, 0x4000)
	order.PutUint32(memory[8:], 0x12345678)
	return
}
