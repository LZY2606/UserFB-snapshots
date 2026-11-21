package atom

// EdtsBox - Edit Box
// Box Type: edts
// Container: Track Box (trak)
// Mandatory: No
// Quantity: Zero or one
type EdtsBox struct {
	*Box
	Elst *ElstBox
}

func (b *EdtsBox) parse() error {
	boxes, err := b.parseChildren()
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "elst":
			b.Elst = &ElstBox{Box: box}
			if err := b.Elst.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
