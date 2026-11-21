package atom

import "encoding/binary"

// HdlrBox - Handler Reference Box
// Box Type: hdlr
// Container: Media Box (mdia) or Meta Box (meta)
// Mandatory: Yes
// Quantity: Exactly one
type HdlrBox struct {
	*Box
	Version byte
	Flags   uint32
	Handler string
	Name    string
}

func (b *HdlrBox) parse() error {
	data, err := b.readPayload()
	if err != nil {
		return err
	}
	if err := b.requirePayload(data, 12); err != nil {
		return err
	}
	b.Version = data[0]
	b.Flags = binary.BigEndian.Uint32(data[0:4])
	b.Handler = string(data[8:12])
	if len(data) > 24 {
		b.Name = string(data[24:])
	}
	return nil
}
