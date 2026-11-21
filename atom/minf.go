package atom

// MinfBox - Media Information Box
// Box Type: minf
// Container: Media Box (mdia)
// Mandatory: Yes
// Quantity: Exactly one.
//
// This box contains all the objects that declare characteristics information of the
// media in the track.
type MinfBox struct {
	*Box
	Vmhd *VmhdBox
	// Dinf *DinfBox
	Stbl *StblBox
	Hmhd *HmhdBox
}

func (b *MinfBox) parse() error {
	boxes, err := b.parseChildren()
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "vmhd":
			b.Vmhd = &VmhdBox{Box: box}
			if err := b.Vmhd.parse(); err != nil {
				return err
			}

		case "stbl":
			b.Stbl = &StblBox{Box: box}
			if err := b.Stbl.parse(); err != nil {
				return err
			}

		case "hmhd":
			b.Hmhd = &HmhdBox{Box: box}
			if err := b.Hmhd.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
