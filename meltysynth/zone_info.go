package meltysynth

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

type zoneInfo struct {
	generatorIndex int32
	modulatorIndex int32
	generatorCount int32
	modulatorCount int32
}

func readZonesFromChunk(r io.Reader, size int32) ([]*zoneInfo, error) {
	var err error

	if size%4 != 0 {
		return nil, errors.New("the zone list is invalid")
	}

	count := size / 4
	if count < 2 {
		return nil, errors.New("the zone list is invalid: no terminator was found")
	}
	zones := make([]*zoneInfo, count)

	for i := int32(0); i < count; i++ {
		zone := new(zoneInfo)

		var generatorIndex uint16
		err = binary.Read(r, binary.LittleEndian, &generatorIndex)
		if err != nil {
			return nil, err
		}
		zone.generatorIndex = int32(generatorIndex)

		var modulatorIndex uint16
		err = binary.Read(r, binary.LittleEndian, &modulatorIndex)
		if err != nil {
			return nil, err
		}
		zone.modulatorIndex = int32(modulatorIndex)

		zones[i] = zone
	}

	return zones, nil
}

// validateZoneInfos checks the bag table against the generator and modulator
// tables. It must run after every pdta sub-chunk has been read, since the
// modulator chunks follow the bag chunks in the SF2 layout.
func validateZoneInfos(zones []*zoneInfo, generatorCount int32, modulatorCount int32) error {
	count := int32(len(zones))
	if count < 2 {
		return errors.New("the zone list is invalid: no terminator was found")
	}

	for i := int32(0); i < count-1; i++ {
		// Bag indices must never go backwards.
		if zones[i+1].generatorIndex < zones[i].generatorIndex {
			return fmt.Errorf("the zone list is invalid: the generator index went backwards at zone %d", i)
		}
		if zones[i+1].modulatorIndex < zones[i].modulatorIndex {
			return fmt.Errorf("the zone list is invalid: the modulator index went backwards at zone %d", i)
		}
		zones[i].generatorCount = zones[i+1].generatorIndex - zones[i].generatorIndex
		zones[i].modulatorCount = zones[i+1].modulatorIndex - zones[i].modulatorIndex
	}

	// The terminator must point exactly at the end of the tables.
	last := zones[count-1]
	if last.generatorIndex != generatorCount {
		return fmt.Errorf("the zone list is invalid: the terminator generator index %d does not match the generator count %d", last.generatorIndex, generatorCount)
	}
	if last.modulatorIndex != modulatorCount {
		return fmt.Errorf("the zone list is invalid: the terminator modulator index %d does not match the modulator count %d", last.modulatorIndex, modulatorCount)
	}

	// Every playable zone must stay inside the tables, so one zone can never
	// borrow records that belong to the next one.
	for i := int32(0); i < count-1; i++ {
		zone := zones[i]
		if zone.generatorIndex < 0 || zone.generatorIndex+zone.generatorCount > generatorCount {
			return fmt.Errorf("the zone list is invalid: zone %d references generators outside the table", i)
		}
		if zone.modulatorIndex < 0 || zone.modulatorIndex+zone.modulatorCount > modulatorCount {
			return fmt.Errorf("the zone list is invalid: zone %d references modulators outside the table", i)
		}
	}

	return nil
}
