package atom

import (
	"encoding/binary"
)

// StsdBox - Sample Description Box
// Box Type: stsd
// Container: Sample Table Box (stbl)
// Mandatory: Yes
// Quantity: Exactly one.
type StsdBox struct {
	*Box
	Version byte
	Flags   uint32
	Avc1    *Avc1Box
}

func (b *StsdBox) parse() error {
	// The sample description box is a full box: a 4 byte version/flags prefix
	// follows the header, then the entry count. Both are part of the payload
	// addressed relative to the actual header size.
	data, err := b.readPayload()
	if err != nil {
		return err
	}
	if err := b.requirePayload(data, 8); err != nil {
		return err
	}
	b.Version = data[0]
	b.Flags = binary.BigEndian.Uint32(data[0:4])

	start := b.Start + b.HeaderSize + 8 // Skip version/flags and entry count.
	boxes, err := b.childBoxes(start)
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "avc1":
			b.Avc1 = &Avc1Box{Box: box}
			if err := b.Avc1.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
