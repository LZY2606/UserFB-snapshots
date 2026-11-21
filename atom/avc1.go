package atom

// Avc1Box defines the avc1 box structure.
type Avc1Box struct {
	*Box
	Version byte
}

func (b *Avc1Box) parse() error {
	data, err := b.readPayload()
	if err != nil {
		return err
	}
	if err := b.requirePayload(data, 1); err != nil {
		return err
	}
	b.Version = data[0]
	return nil
}
