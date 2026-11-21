package atom

// StblBox - Sample Table Box
// Box Type: stbl
// Container: Media Information Box (minf)
// Mandatory: Yes
// Quantity: Exactly one.
type StblBox struct {
	*Box
	Stts *SttsBox
	Stsd *StsdBox
}

func (b *StblBox) parse() error {
	boxes, err := b.parseChildren()
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "stts":
			b.Stts = &SttsBox{Box: box}
			if err := b.Stts.parse(); err != nil {
				return err
			}

		case "stsd":
			b.Stsd = &StsdBox{Box: box}
			if err := b.Stsd.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
