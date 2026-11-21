package atom

// TrakBox - Track Box
// Box Type: tkhd
// Container: Movie Box (moov)
// Mandatory: Yes
// Quantity: One or more.
type TrakBox struct {
	*Box
	// SamplesDuration
	// SamplesSize
	// SampleGroupsInfo

	Tkhd *TkhdBox
	Mdia *MdiaBox
	Edts *EdtsBox
}

func (b *TrakBox) parse() error {
	boxes, err := b.parseChildren()
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "tkhd":
			b.Tkhd = &TkhdBox{Box: box}
			if err := b.Tkhd.parse(); err != nil {
				return err
			}

		case "mdia":
			b.Mdia = &MdiaBox{Box: box}
			if err := b.Mdia.parse(); err != nil {
				return err
			}

		case "edts":
			b.Edts = &EdtsBox{Box: box}
			if err := b.Edts.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
