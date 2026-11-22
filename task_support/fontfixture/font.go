// Package fontfixture builds an original, small PCM font for the existing loader.
package fontfixture

import "encoding/binary"

func word(b []byte, x uint16) []byte { return binary.LittleEndian.AppendUint16(b, x) }
func long(b []byte, x uint32) []byte { return binary.LittleEndian.AppendUint32(b, x) }
func name(b []byte, s string) []byte {
	for i := 0; i < 20; i++ {
		if i < len(s) {
			b = append(b, s[i])
		} else {
			b = append(b, 0)
		}
	}
	return b
}
func chunk(b []byte, id string, payload []byte) []byte {
	b = append(b, []byte(id)...)
	b = long(b, uint32(len(payload)))
	b = append(b, payload...)
	if len(payload)%2 != 0 {
		b = append(b, 0)
	}
	return b
}
func list(kind string, b []byte) []byte { return append([]byte(kind), b...) }

// Build returns the SF2 bytes and the offset/length of its PCM payload.
func Build() ([]byte, int, int) {
	var sm, sd, pd, b, info, all []byte
	for i := 0; i < 64; i++ {
		sm = word(sm, uint16(int16((i%16)*1000-7500)))
	}
	for i := 0; i < 46; i++ {
		sm = word(sm, 0)
	}
	sd = chunk(sd, "smpl", sm)
	b = word(nil, 2)
	b = word(b, 1)
	info = chunk(info, "ifil", b)
	info = chunk(info, "isng", []byte("EMU8000\x00"))
	info = chunk(info, "INAM", []byte("OriginalFont\x00\x00"))
	b = name(nil, "OriginalTone")
	b = word(b, 7)
	b = word(b, 2)
	b = word(b, 0)
	b = long(b, 0)
	b = long(b, 0)
	b = long(b, 0)
	b = name(b, "EOP")
	b = word(b, 0)
	b = word(b, 0)
	b = word(b, 1)
	b = long(b, 0)
	b = long(b, 0)
	b = long(b, 0)
	pd = chunk(pd, "phdr", b)
	b = nil
	for _, x := range []uint16{0, 0, 1, 0} {
		b = word(b, x)
	}
	pd = chunk(pd, "pbag", b)
	pd = chunk(pd, "pmod", make([]byte, 10))
	b = nil
	for _, x := range []uint16{41, 0, 0, 0} {
		b = word(b, x)
	}
	pd = chunk(pd, "pgen", b)
	b = name(nil, "Instrument")
	b = word(b, 0)
	b = name(b, "EOI")
	b = word(b, 1)
	pd = chunk(pd, "inst", b)
	b = nil
	for _, x := range []uint16{0, 0, 2, 0} {
		b = word(b, x)
	}
	pd = chunk(pd, "ibag", b)
	pd = chunk(pd, "imod", make([]byte, 10))
	b = nil
	for _, x := range []uint16{54, 1, 53, 0, 0, 0} {
		b = word(b, x)
	}
	pd = chunk(pd, "igen", b)
	b = name(nil, "Triangle")
	for _, x := range []uint32{0, 64, 8, 56, 22050} {
		b = long(b, x)
	}
	b = append(b, 60, 0)
	b = word(b, 0)
	b = word(b, 1)
	b = name(b, "EOS")
	for i := 0; i < 4; i++ {
		b = long(b, 110)
	}
	b = long(b, 0)
	b = append(b, 0, 0)
	b = word(b, 0)
	b = word(b, 0)
	pd = chunk(pd, "shdr", b)
	all = chunk(all, "LIST", list("INFO", info))
	smplOffset := 12 + len(all) + 12 + 8
	all = chunk(all, "LIST", list("sdta", sd))
	all = chunk(all, "LIST", list("pdta", pd))
	return chunk(nil, "RIFF", list("sfbk", all)), smplOffset, len(sm)
}
