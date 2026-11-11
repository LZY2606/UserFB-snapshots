package meltysynth

import (
	"errors"
)

type zone struct {
	generators []generator
	modulators []modulator
}

func createZones(infos []*zoneInfo, generators []generator, modulators []modulator) ([]*zone, error) {
	if len(infos) <= 1 {
		return nil, errors.New("no valid zone was found")
	}

	// The last one is the terminator; it belongs to no playable zone.
	count := len(infos) - 1
	zones := make([]*zone, count)

	for i := 0; i < count; i++ {
		info := infos[i]

		zo := new(zone)
		zo.generators = make([]generator, info.generatorCount)
		for j := int32(0); j < info.generatorCount; j++ {
			zo.generators[j] = generators[info.generatorIndex+j]
		}

		zo.modulators = make([]modulator, info.modulatorCount)
		for j := int32(0); j < info.modulatorCount; j++ {
			zo.modulators[j] = modulators[info.modulatorIndex+j]
		}

		zones[i] = zo
	}

	return zones, nil
}

func createEmptyZone() *zone {
	result := new(zone)
	result.generators = make([]generator, 0)
	result.modulators = make([]modulator, 0)
	return result
}
