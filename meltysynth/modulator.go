package meltysynth

import (
	"encoding/binary"
	"errors"
	"io"
)

// Modulator source enumerators (SF2 spec, section 8.4).
const (
	modSrc_None           uint16 = 0
	modSrc_NoteOnVelocity uint16 = 2
	modSrc_CC16           uint16 = 16
	modSrc_CC17           uint16 = 17
)

// Modulator source flags.
const (
	modSrc_TypeMask       uint16 = 0xFC00
	modSrc_Linear         uint16 = 0x0000
	modSrc_DirectionMask  uint16 = 0x0200
	modSrc_DirectionMinus uint16 = 0x0200
	modSrc_PolarityMask   uint16 = 0x0100
	modSrc_Bipolar        uint16 = 0x0100
)

// Modulator transform enumerators. Only the linear transform (0) is supported.
const (
	modTransform_Linear int16 = 0
)

type modulator struct {
	source       uint16
	destination  uint16
	amount       int16
	amountSource uint16
	transform    int16
	supported    bool
}

func readModulatorsFromChunk(r io.Reader, size int32) ([]modulator, error) {
	if size%10 != 0 {
		return nil, errors.New("the modulator list is invalid")
	}

	if size == 0 {
		return make([]modulator, 0), nil
	}

	// The last record is the terminator.
	count := size/10 - 1
	modulators := make([]modulator, count)

	for i := int32(0); i < count; i++ {
		var mod modulator

		if err := binary.Read(r, binary.LittleEndian, &mod.source); err != nil {
			return nil, err
		}
		if err := binary.Read(r, binary.LittleEndian, &mod.destination); err != nil {
			return nil, err
		}
		if err := binary.Read(r, binary.LittleEndian, &mod.amount); err != nil {
			return nil, err
		}
		if err := binary.Read(r, binary.LittleEndian, &mod.amountSource); err != nil {
			return nil, err
		}
		if err := binary.Read(r, binary.LittleEndian, &mod.transform); err != nil {
			return nil, err
		}

		// Unsupported modulators are kept but never applied, so a SoundFont
		// using an unsupported destination still loads and plays.
		sourceSupported := isSupportedModulatorSource(mod.source) && isSupportedModulatorSource(mod.amountSource)
		mod.supported = sourceSupported &&
			mod.transform == modTransform_Linear &&
			isSupportedModulatorDestination(mod.destination)

		modulators[i] = mod
	}

	// The last one is the terminator, which must be entirely zero.
	var terminator [10]byte
	n, err := io.ReadFull(r, terminator[:])
	if err != nil {
		return nil, err
	}
	if n != len(terminator) {
		return nil, errors.New("failed to read the modulator list")
	}
	for _, b := range terminator {
		if b != 0 {
			return nil, errors.New("the modulator list terminator is invalid")
		}
	}

	return modulators, nil
}

func isSupportedModulatorSource(source uint16) bool {
	// Only linear controllers are in the supported subset.
	if source&modSrc_TypeMask != modSrc_Linear {
		return false
	}
	switch source & 0xFF {
	case modSrc_None & 0xFF, modSrc_NoteOnVelocity & 0xFF, modSrc_CC16 & 0xFF, modSrc_CC17 & 0xFF:
		return true
	default:
		return false
	}
}

func isSupportedModulatorDestination(destination uint16) bool {
	switch destination {
	case gen_InitialAttenuation, gen_Pan, gen_InitialFilterCutoffFrequency:
		return true
	default:
		return false
	}
}

// modulatorIdentity identifies a mapping within one layer. Modulators with the
// same identity are merged: the local zone replaces the global zone's entry,
// while entries with different identities pointing at the same destination are
// added together.
type modulatorIdentity struct {
	source       uint16
	destination  uint16
	amountSource uint16
	transform    int16
}

func (m modulator) identity() modulatorIdentity {
	return modulatorIdentity{
		source:       m.source,
		destination:  m.destination,
		amountSource: m.amountSource,
		transform:    m.transform,
	}
}

// mergeModulatorZones folds the global zone into each local zone. Modulators
// with an identity also present in the local zone are replaced (an amount of
// zero therefore masks the global entry); all other global entries are
// prepended. A duplicate identity inside one zone is likewise replaced by the
// later record, as required by the SF2 specification. The terminator record
// belongs to no playable zone and is never passed in.
func mergeModulatorZones(global []modulator, local []modulator) []modulator {
	indexByIdentity := make(map[modulatorIdentity]int, len(global)+len(local))
	merged := make([]modulator, 0, len(global)+len(local))

	appendMod := func(mod modulator) {
		if index, found := indexByIdentity[mod.identity()]; found {
			merged[index] = mod
		} else {
			indexByIdentity[mod.identity()] = len(merged)
			merged = append(merged, mod)
		}
	}

	for i := range global {
		appendMod(global[i])
	}
	for i := range local {
		appendMod(local[i])
	}
	return merged
}

// sourceValue evaluates a modulator source.
// The velocity is the snapshot taken at the note-on; CC16 and CC17 are the
// raw 7-bit values of the voice's channel at the start of the current block.
func modulatorSourceValue(source uint16, v *voice) float32 {
	var value int32
	switch source & 0xFF {
	case modSrc_None & 0xFF:
		return 1
	case modSrc_NoteOnVelocity & 0xFF:
		value = v.velocity
	case modSrc_CC16 & 0xFF:
		value = int32(v.synthesizer.channels[v.channel].cc16)
	case modSrc_CC17 & 0xFF:
		value = int32(v.synthesizer.channels[v.channel].cc17)
	default:
		return 0
	}

	if source&modSrc_PolarityMask == modSrc_Bipolar {
		result := 2*float32(value)/127 - 1
		if source&modSrc_DirectionMask == modSrc_DirectionMinus {
			return -result
		}
		return result
	}

	result := float32(value) / 127
	if source&modSrc_DirectionMask == modSrc_DirectionMinus {
		return 1 - result
	}
	return result
}

// evaluateModulators recomputes the total contribution of all modulators from
// the current sources. It is called once per block per voice, so it must not
// allocate on the heap.
func evaluateModulators(modulators []modulator, v *voice) (attenuationCents float32, panTenths float32, filterCents float32) {
	for i := range modulators {
		mod := &modulators[i]
		if !mod.supported {
			continue
		}

		amount := float32(mod.amount) *
			modulatorSourceValue(mod.source, v) *
			modulatorSourceValue(mod.amountSource, v)

		switch mod.destination {
		case gen_InitialAttenuation:
			// 1 unit = 0.1 dB.
			attenuationCents += amount
		case gen_Pan:
			// 1 unit = 0.1%.
			panTenths += amount
		case gen_InitialFilterCutoffFrequency:
			// 1 unit = 1 cent.
			filterCents += amount
		}
	}
	return
}
