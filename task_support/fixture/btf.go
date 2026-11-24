// Package fixture builds legal synthetic raw BTF without accessing the kernel.
package fixture

import (
	"encoding/binary"
	"strings"
)

func Words(order binary.ByteOrder, values ...uint32) []byte {
	b := make([]byte, 4*len(values))
	for i, v := range values {
		order.PutUint32(b[i*4:], v)
	}
	return b
}
func Blob(order binary.ByteOrder, types []byte, names string) []byte {
	b := make([]byte, 24)
	order.PutUint16(b, 0xeb9f)
	b[2] = 1
	order.PutUint32(b[4:], 24)
	order.PutUint32(b[12:], uint32(len(types)))
	order.PutUint32(b[16:], uint32(len(types)))
	order.PutUint32(b[20:], uint32(len(names)))
	b = append(b, types...)
	return append(b, []byte(names)...)
}
func Original(order binary.ByteOrder) (base, split []byte) {
	names := "\x00int\x00member\x00value\x00"
	base = Blob(order, Words(order, 1, 1<<24, 4, 1<<24|32), names)
	leaf := uint32(len(names))
	mark := leaf + 5
	member := uint32(strings.Index(names, "member"))
	value := uint32(strings.Index(names, "value"))
	// Local IDs: 2 pointer, 3 recursive structure, 4 member declaration tag.
	types := Words(order, 0, 2<<24, 3)
	types = append(types, Words(order, leaf, 4<<24|2, 16, member, 2, 0, value, 1, 64)...)
	types = append(types, Words(order, mark, 17<<24, 3, 0)...)
	split = Blob(order, types, "leaf\x00mark\x00")
	return
}
