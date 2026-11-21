package atom

import (
	"encoding/binary"
)

// FtypBox - File Type Box
// Box Type: ftyp
// Container: File
// Mandatory: Yes
// Quantity: Exactly one
type FtypBox struct {
	*Box
	MajorBrand       string   // Brand identifer.
	MinorVersion     uint32   // Informative integer for the minor version of the major brand.
	CompatibleBrands []string // A list, to the end of the box, of brands.
}

func (b *FtypBox) parse() error {
	data, err := b.readPayload()
	if err != nil {
		return err
	}
	if len(data) < 8 {
		return boxError(b.Name, b.Start, "truncated payload: need at least 8 bytes, have %d", len(data))
	}
	b.MajorBrand, b.MinorVersion = string(data[0:4]), binary.BigEndian.Uint32(data[4:8])
	if len(data) > 8 {
		if (len(data)-8)%4 != 0 {
			return boxError(b.Name, b.Start, "truncated compatible brand list: %d trailing bytes", len(data)-8)
		}
		for i := 8; i < len(data); i += 4 {
			b.CompatibleBrands = append(b.CompatibleBrands, string(data[i:i+4]))
		}
	}
	return nil
}
