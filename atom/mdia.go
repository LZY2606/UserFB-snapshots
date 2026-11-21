package atom

// MdiaBox - Media Box
// Box Type: mdia
// Container: Track Box (trak)
// Mandatory: Yes
// Quantity: Exactly one.
// The media declaration container contains all the objects that declare information
// about the media data within a track.
type MdiaBox struct {
	*Box
	Hdlr *HdlrBox
	Mdhd *MdhdBox
	Minf *MinfBox
}

func (b *MdiaBox) parse() error {
	boxes, err := b.parseChildren()
	if err != nil {
		return err
	}

	for _, box := range boxes {
		switch box.Name {
		case "hdlr":
			b.Hdlr = &HdlrBox{Box: box}
			if err := b.Hdlr.parse(); err != nil {
				return err
			}

		case "mdhd":
			b.Mdhd = &MdhdBox{Box: box}
			if err := b.Mdhd.parse(); err != nil {
				return err
			}

		case "minf":
			b.Minf = &MinfBox{Box: box}
			if err := b.Minf.parse(); err != nil {
				return err
			}
		}
	}
	return nil
}
