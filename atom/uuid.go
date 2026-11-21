package atom

// UuidBox - Uuid Box
// Box Type: uuid
type UuidBox struct {
	*Box
	Uuid []byte
	Data []byte
}

func (b *UuidBox) parse() error {
	data, err := b.readPayload()
	if err != nil {
		return err
	}
	if len(data) < UuidSize {
		return boxError(b.Name, b.Start, "truncated payload: need at least %d bytes, have %d", UuidSize, len(data))
	}
	b.Uuid = data[:UuidSize]
	b.Data = data[UuidSize:]
	return nil
}
