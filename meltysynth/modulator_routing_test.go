package meltysynth

import (
	"bytes"
	"encoding/binary"
	"math"
	"testing"
)

// ===========================================================================
// Minimal in-memory RIFF/SF2 builder.
// The test SoundFonts are assembled as real RIFF bytes and go through
// NewSoundFont, so the whole parsing path (including all validation) is
// exercised.
// ===========================================================================

type sf2TestGen struct {
	typ uint16
	val int16
}

type sf2TestMod struct {
	source       uint16
	destination  uint16
	amount       int16
	amountSource uint16
	transform    int16
}

type sf2TestZone struct {
	gens []sf2TestGen
	mods []sf2TestMod
}

const (
	mrSrcNone uint16 = 0
	mrSrcVel  uint16 = 2
	mrSrcCC16 uint16 = 16
	mrSrcCC17 uint16 = 17
	mrNeg     uint16 = 0x0200
	mrBipolar uint16 = 0x0100
	mrConvex  uint16 = 0x0400
)

func mrWriteFourCC(b *bytes.Buffer, value string) {
	b.WriteString(value)
}

func mrWriteU16(b *bytes.Buffer, value uint16) {
	var raw [2]byte
	binary.LittleEndian.PutUint16(raw[:], value)
	b.Write(raw[:])
}

func mrWriteI16(b *bytes.Buffer, value int16) {
	mrWriteU16(b, uint16(value))
}

func mrWriteI32(b *bytes.Buffer, value int32) {
	var raw [4]byte
	binary.LittleEndian.PutUint32(raw[:], uint32(value))
	b.Write(raw[:])
}

func mrWriteFixedString(b *bytes.Buffer, value string, length int) {
	raw := make([]byte, length)
	copy(raw, value)
	b.Write(raw)
}

func mrChunk(id string, body []byte) []byte {
	var b bytes.Buffer
	mrWriteFourCC(&b, id)
	mrWriteI32(&b, int32(len(body)))
	b.Write(body)
	return b.Bytes()
}

func mrListChunk(listType string, bodies ...[]byte) []byte {
	var body bytes.Buffer
	mrWriteFourCC(&body, listType)
	for _, part := range bodies {
		body.Write(part)
	}
	return mrChunk("LIST", body.Bytes())
}

func mrGeneratorBody(zones []sf2TestZone) []byte {
	var b bytes.Buffer
	for _, zone := range zones {
		for _, gen := range zone.gens {
			mrWriteU16(&b, gen.typ)
			mrWriteI16(&b, gen.val)
		}
	}
	// Terminator.
	mrWriteU16(&b, 0)
	mrWriteU16(&b, 0)
	return b.Bytes()
}

func mrModulatorBody(zones []sf2TestZone) []byte {
	var b bytes.Buffer
	for _, zone := range zones {
		for _, mod := range zone.mods {
			mrWriteU16(&b, mod.source)
			mrWriteU16(&b, mod.destination)
			mrWriteI16(&b, mod.amount)
			mrWriteU16(&b, mod.amountSource)
			mrWriteI16(&b, mod.transform)
		}
	}
	// Terminator: ten zero bytes.
	b.Write(make([]byte, 10))
	return b.Bytes()
}

func mrBagBody(zones []sf2TestZone) []byte {
	var b bytes.Buffer
	var genIndex, modIndex uint16
	for _, zone := range zones {
		mrWriteU16(&b, genIndex)
		mrWriteU16(&b, modIndex)
		genIndex += uint16(len(zone.gens))
		modIndex += uint16(len(zone.mods))
	}
	// Terminator.
	mrWriteU16(&b, genIndex)
	mrWriteU16(&b, modIndex)
	return b.Bytes()
}

// mrBuildWave creates a short-period PCM loop: a 1500 Hz sine (32 samples per
// period at 48 kHz) followed by the 46 zero samples required by the shdr
// terminator alignment.
func mrBuildWave() []int16 {
	const sampleCount = 256
	wave := make([]int16, sampleCount+46)
	for i := 0; i < sampleCount; i++ {
		wave[i] = int16(12000 * math.Sin(2*math.Pi*float64(i)/32))
	}
	return wave
}

// buildRoutingSF2 builds a SoundFont with one preset, one instrument and one
// looping mono sample. It contains every routing situation required by the
// main test.
func buildRoutingSF2() []byte {
	wave := mrBuildWave()
	// Instrument zones: a global zone followed by one local zone.
	instZones := []sf2TestZone{
		{
			// Global zone (no SampleID generator).
			mods: []sf2TestMod{
				// M1: CC16 -> initialAttenuation, will be masked locally.
				{mrSrcCC16, gen_InitialAttenuation, 200, mrSrcNone, 0},
				// M2: CC17 -> pan.
				{mrSrcCC17, gen_Pan, 500, mrSrcNone, 0},
				// M3: same destination as M1/M9 but a different identity.
				{mrSrcCC16, gen_InitialAttenuation, 100, mrSrcCC17, 0},
			},
		},
		{
			gens: []sf2TestGen{
				{gen_SampleID, 0},
				{gen_SampleModes, int16(loop_Continuous)},
				{gen_InitialFilterCutoffFrequency, 6000},
				// A flat, instant volume envelope so block energies are stable.
				{gen_DelayVolumeEnvelope, -12000},
				{gen_AttackVolumeEnvelope, -12000},
				{gen_HoldVolumeEnvelope, -12000},
				{gen_DecayVolumeEnvelope, -12000},
				{gen_SustainVolumeEnvelope, 0},
				{gen_ExclusiveClass, 1},
			},
			mods: []sf2TestMod{
				// M4: same identity as M1 with an amount of zero -> masks M1.
				{mrSrcCC16, gen_InitialAttenuation, 0, mrSrcNone, 0},
				// M5: bipolar, negative-direction note-on velocity -> pan.
				{mrSrcVel | mrBipolar | mrNeg, gen_Pan, 500, mrSrcNone, 0},
				// M6: CC17 -> filter cutoff in cents.
				{mrSrcCC17, gen_InitialFilterCutoffFrequency, 6000, mrSrcNone, 0},
				// M9: CC16 with CC16 as the amount source -> attenuation.
				{mrSrcCC16, gen_InitialAttenuation, 200, mrSrcCC16, 0},
				// M7: legal destination outside the supported subset -> ignored.
				{mrSrcCC16, gen_ReverbEffectsSend, 50, mrSrcNone, 0},
				// M8: unsupported (convex) source type -> ignored.
				{mrSrcCC16 | mrConvex, gen_InitialAttenuation, 200, mrSrcNone, 0},
			},
		},
	}

	// Preset zones: a global zone with one attenuation modulator, then the
	// local zone selecting the instrument.
	presetZones := []sf2TestZone{
		{
			// Global preset zone (no Instrument generator).
			mods: []sf2TestMod{
				// PM1: CC17 -> attenuation, adds on top of the instrument layer.
				{mrSrcCC17, gen_InitialAttenuation, 100, mrSrcNone, 0},
			},
		},
		{
			gens: []sf2TestGen{
				{gen_Instrument, 0},
			},
		},
	}

	return mrAssembleSF2(wave, instZones, presetZones)
}

func mrAssembleSF2(wave []int16, instZones []sf2TestZone, presetZones []sf2TestZone) []byte {
	// INFO list.
	var ifil bytes.Buffer
	mrWriteI16(&ifil, 2)
	mrWriteI16(&ifil, 1)
	var inam bytes.Buffer
	inam.WriteString("routing-test")
	infoList := mrListChunk("INFO", mrChunk("ifil", ifil.Bytes()), mrChunk("INAM", inam.Bytes()))

	// sdta list.
	var smpl bytes.Buffer
	for _, sample := range wave {
		mrWriteI16(&smpl, sample)
	}
	sdtaList := mrListChunk("sdta", mrChunk("smpl", smpl.Bytes()))

	// phdr.
	var phdr bytes.Buffer
	mrWriteFixedString(&phdr, "p", 20)
	mrWriteU16(&phdr, 0) // patch
	mrWriteU16(&phdr, 0) // bank
	mrWriteU16(&phdr, 0) // first bag index
	mrWriteI32(&phdr, 0) // library
	mrWriteI32(&phdr, 0) // genre
	mrWriteI32(&phdr, 0) // morphology
	mrWriteFixedString(&phdr, "EOI", 20)
	mrWriteU16(&phdr, 0)                        // patch
	mrWriteU16(&phdr, 0)                        // bank
	mrWriteU16(&phdr, uint16(len(presetZones))) // first bag index
	mrWriteI32(&phdr, 0)                        // library
	mrWriteI32(&phdr, 0)                        // genre
	mrWriteI32(&phdr, 0)                        // morphology

	// inst.
	var inst bytes.Buffer
	mrWriteFixedString(&inst, "i", 20)
	mrWriteU16(&inst, 0)
	mrWriteFixedString(&inst, "EOI", 20)
	mrWriteU16(&inst, uint16(len(instZones)))

	// shdr (one mono sample + the terminator).
	var shdr bytes.Buffer
	mrWriteFixedString(&shdr, "s", 20)
	mrWriteI32(&shdr, 0)                   // start
	mrWriteI32(&shdr, int32(len(wave)-46)) // end
	mrWriteI32(&shdr, 32)                  // loop start
	mrWriteI32(&shdr, 64)                  // loop end
	mrWriteI32(&shdr, 48000)               // sample rate
	shdr.WriteByte(60)                     // original pitch
	shdr.WriteByte(0)                      // pitch correction
	mrWriteU16(&shdr, 0)                   // link
	mrWriteU16(&shdr, 1)                   // mono sample
	mrWriteFixedString(&shdr, "EOS", 20)   // terminator
	shdr.Write(make([]byte, 26))

	pdtaList := mrListChunk("pdta",
		mrChunk("phdr", phdr.Bytes()),
		mrChunk("pbag", mrBagBody(presetZones)),
		mrChunk("pmod", mrModulatorBody(presetZones)),
		mrChunk("pgen", mrGeneratorBody(presetZones)),
		mrChunk("inst", inst.Bytes()),
		mrChunk("ibag", mrBagBody(instZones)),
		mrChunk("imod", mrModulatorBody(instZones)),
		mrChunk("igen", mrGeneratorBody(instZones)),
		mrChunk("shdr", shdr.Bytes()),
	)

	var body bytes.Buffer
	mrWriteFourCC(&body, "sfbk")
	body.Write(infoList)
	body.Write(sdtaList)
	body.Write(pdtaList)

	var riff bytes.Buffer
	mrWriteFourCC(&riff, "RIFF")
	mrWriteI32(&riff, int32(body.Len()))
	riff.Write(body.Bytes())
	return riff.Bytes()
}

// ===========================================================================
// Rendering helpers.
// ===========================================================================

func mrNewSynth(t *testing.T, data []byte, polyphony int32) (*Synthesizer, *SoundFont) {
	sf, err := NewSoundFont(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("NewSoundFont failed: %v", err)
	}
	settings := NewSynthesizerSettings(48000)
	settings.EnableReverbAndChorus = false
	if polyphony > 0 {
		settings.MaximumPolyphony = polyphony
	}
	synth, err := NewSynthesizer(sf, settings)
	if err != nil {
		t.Fatalf("NewSynthesizer failed: %v", err)
	}
	return synth, sf
}

func mrRenderBlocks(synth *Synthesizer, blocks int32) ([]float32, []float32) {
	length := blocks * synth.BlockSize
	left := make([]float32, length)
	right := make([]float32, length)
	synth.Render(left, right)
	return left, right
}

func mrEnergy(data []float32) float64 {
	var sum float64
	for _, v := range data {
		sum += float64(v) * float64(v)
	}
	return sum
}

// mrWarmUp renders enough blocks for gain smoothing and filter convergence.
func mrWarmUp(synth *Synthesizer) {
	mrRenderBlocks(synth, 12)
}

// ===========================================================================
// Main test.
// ===========================================================================

func TestModulatorRouting(t *testing.T) {
	data := buildRoutingSF2()

	// -----------------------------------------------------------------------
	// 1. Parsed routing: masking, identities, layering, unsupported entries.
	// -----------------------------------------------------------------------
	t.Run("parsed mappings", func(t *testing.T) {
		sf, err := NewSoundFont(bytes.NewReader(data))
		if err != nil {
			t.Fatalf("the SoundFont must load even with unsupported modulators: %v", err)
		}

		instRegion := sf.Instruments[0].Regions[0]
		// M1 is replaced by the zero-amount M4, M2 and M3 survive, and the
		// four local entries are appended: 8 entries in total.
		if len(instRegion.mods) != 8 {
			t.Fatalf("instrument region mod count = %d, want 8", len(instRegion.mods))
		}

		maskCount := 0
		var m2, m3, m5, m6, m9 *modulator
		for i := range instRegion.mods {
			mod := &instRegion.mods[i]
			switch mod.identity() {
			case (modulator{source: mrSrcCC16, destination: gen_InitialAttenuation, amountSource: mrSrcNone}).identity():
				maskCount++
				if mod.amount != 0 {
					t.Fatalf("the global 200-entry must be masked by the local zero-entry, got amount %d", mod.amount)
				}
			case (modulator{source: mrSrcCC17, destination: gen_Pan, amountSource: mrSrcNone}).identity():
				m2 = mod
			case (modulator{source: mrSrcCC16, destination: gen_InitialAttenuation, amountSource: mrSrcCC17}).identity():
				m3 = mod
			case (modulator{source: mrSrcVel | mrBipolar | mrNeg, destination: gen_Pan, amountSource: mrSrcNone}).identity():
				m5 = mod
			case (modulator{source: mrSrcCC17, destination: gen_InitialFilterCutoffFrequency, amountSource: mrSrcNone}).identity():
				m6 = mod
			case (modulator{source: mrSrcCC16, destination: gen_InitialAttenuation, amountSource: mrSrcCC16}).identity():
				m9 = mod
			}
		}
		if maskCount != 1 {
			t.Fatalf("the masked identity must appear exactly once, got %d", maskCount)
		}
		if m2 == nil || m2.amount != 500 {
			t.Fatal("global CC17 pan modulator missing or replaced")
		}
		if m3 == nil || m3.amount != 100 {
			t.Fatal("global CC16/CC17 attenuation modulator missing")
		}
		if m5 == nil || !m5.supported {
			t.Fatal("bipolar reversed velocity modulator missing or not supported")
		}
		if m6 == nil || m6.amount != 6000 {
			t.Fatal("filter modulator missing")
		}
		if m9 == nil || m9.amount != 200 {
			t.Fatal("amount-source attenuation modulator missing")
		}

		supportedCount := 0
		for i := range instRegion.mods {
			if instRegion.mods[i].supported {
				supportedCount++
			}
		}
		// M7 (unsupported destination) and M8 (unsupported source) are kept
		// but flagged, so 6 of the 8 entries are effective.
		if supportedCount != 6 {
			t.Fatalf("supported modulator count = %d, want 6", supportedCount)
		}

		presetRegion := sf.Presets[0].Regions[0]
		if len(presetRegion.mods) != 1 || presetRegion.mods[0].amount != 100 {
			t.Fatalf("preset global modulator not inherited into the preset region: %+v", presetRegion.mods)
		}
	})

	// -----------------------------------------------------------------------
	// 2. Source value semantics, including direction and polarity.
	// -----------------------------------------------------------------------
	t.Run("source values", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.channels[0].cc16 = 0
		synth.channels[0].cc17 = 127
		synth.NoteOn(0, 60, 64)
		if synth.voices.activeVoiceCount == 0 {
			t.Fatal("no voice started")
		}
		v := synth.voices.voices[0]

		if got := modulatorSourceValue(mrSrcNone, v); got != 1 {
			t.Fatalf("constant source = %v, want 1", got)
		}
		if got := modulatorSourceValue(mrSrcCC16, v); got != 0 {
			t.Fatalf("unipolar CC16 at 0 = %v, want 0", got)
		}
		want127 := float32(127) / 127
		if got := modulatorSourceValue(mrSrcCC17, v); got != want127 {
			t.Fatalf("unipolar CC17 at 127 = %v, want %v", got, want127)
		}
		if got := modulatorSourceValue(mrSrcCC17|mrNeg, v); got != 0 {
			t.Fatalf("negative unipolar at 127 = %v, want 0", got)
		}
		// Velocity 64, bipolar: 2*64/127 - 1.
		wantBi := 2*float32(64)/127 - 1
		if got := modulatorSourceValue(mrSrcVel|mrBipolar, v); math.Abs(float64(got-wantBi)) > 1e-6 {
			t.Fatalf("bipolar velocity = %v, want %v", got, wantBi)
		}
		if got := modulatorSourceValue(mrSrcVel|mrBipolar|mrNeg, v); math.Abs(float64(got+wantBi)) > 1e-6 {
			t.Fatalf("negative bipolar velocity = %v, want %v", got, -wantBi)
		}
	})

	// -----------------------------------------------------------------------
	// 3. Attenuation: amplitude ratio follows the combined SF2 contribution.
	// -----------------------------------------------------------------------
	t.Run("attenuation amplitude ratio", func(t *testing.T) {
		// Baseline: CC16 = 0, no effective attenuation.
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)
		baseLeft, baseRight := mrRenderBlocks(synth, 8)
		base := math.Sqrt((mrEnergy(baseLeft) + mrEnergy(baseRight)) / float64(len(baseLeft)))

		// CC16 = 127: M9 alone contributes 200 tenths of a dB = 20 dB, while
		// CC17 stays zero so pan and filter do not move.
		synth2, _ := mrNewSynth(t, data, 8)
		synth2.ProcessMidiMessage(0, 0xB0, 0x10, 127)
		synth2.NoteOn(0, 60, 64)
		mrWarmUp(synth2)
		attLeft, attRight := mrRenderBlocks(synth2, 8)
		attenuated := math.Sqrt((mrEnergy(attLeft) + mrEnergy(attRight)) / float64(len(attLeft)))

		ratio := attenuated / base
		if ratio < 0.07 || ratio > 0.14 {
			t.Fatalf("attenuation amplitude ratio = %v, want about 0.1 (20 dB)", ratio)
		}
		if attenuated <= 1e-9 {
			t.Fatal("attenuated output is silent")
		}
	})

	// -----------------------------------------------------------------------
	// 4. Pan: stereo energy follows CC17 and the velocity snapshot.
	// -----------------------------------------------------------------------
	t.Run("pan energy", func(t *testing.T) {
		// CC17 = 0: the velocity term is tiny at velocity 64, so centered.
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)
		cLeft, cRight := mrRenderBlocks(synth, 8)
		le, re := mrEnergy(cLeft), mrEnergy(cRight)
		if math.Abs(le-re)/math.Max(le, re) > 0.05 {
			t.Fatalf("centered pan unbalanced: left %v right %v", le, re)
		}

		// CC17 = 127: M2 pushes the pan to the right endpoint.
		synth2, _ := mrNewSynth(t, data, 8)
		synth2.ProcessMidiMessage(0, 0xB0, 0x11, 127)
		synth2.NoteOn(0, 60, 64)
		mrWarmUp(synth2)
		rLeft, rRight := mrRenderBlocks(synth2, 8)
		if mrEnergy(rRight) <= 20*mrEnergy(rLeft) {
			t.Fatalf("right pan not effective: left %v right %v", mrEnergy(rLeft), mrEnergy(rRight))
		}
	})

	// -----------------------------------------------------------------------
	// 5. Bipolar reversed velocity is a per-note snapshot.
	// -----------------------------------------------------------------------
	t.Run("velocity snapshot pan", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)

		// Velocity 127: reversed bipolar value is -1, pan goes fully left.
		synth.NoteOn(0, 60, 127)
		mrWarmUp(synth)
		lLeft, lRight := mrRenderBlocks(synth, 4)
		if mrEnergy(lLeft) <= 20*mrEnergy(lRight) {
			t.Fatalf("velocity 127 should pan left: left %v right %v", mrEnergy(lLeft), mrEnergy(lRight))
		}
		synth.NoteOff(0, 60)
		mrRenderBlocks(synth, 20)

		// Velocity 8: the reversed bipolar value is about +0.874, so the pan
		// goes right. Velocity 1 would be below the audibility threshold.
		synth.NoteOn(0, 60, 8)
		if synth.voices.voices[0].velocity != 8 {
			t.Fatal("the reused voice must take the new note-on velocity snapshot")
		}
		mrWarmUp(synth)
		rLeft, rRight := mrRenderBlocks(synth, 4)
		if mrEnergy(rRight) <= 20*mrEnergy(rLeft) {
			t.Fatalf("velocity 1 should pan right: left %v right %v", mrEnergy(rLeft), mrEnergy(rRight))
		}
	})

	// -----------------------------------------------------------------------
	// 6. Filter: opening the cutoff changes the spectral energy.
	// -----------------------------------------------------------------------
	t.Run("filter response", func(t *testing.T) {
		// CC16/CC17 zero: cutoff stays at 6000 cents (~368 Hz), the 1500 Hz
		// tone is strongly attenuated.
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)
		closedL, closedR := mrRenderBlocks(synth, 16)
		closed := mrEnergy(closedL) + mrEnergy(closedR)

		// CC17 = 127: M6 adds 6000 cents, the cutoff opens far above 1500 Hz.
		synth2, _ := mrNewSynth(t, data, 8)
		synth2.ProcessMidiMessage(0, 0xB0, 0x11, 127)
		synth2.NoteOn(0, 60, 64)
		mrWarmUp(synth2)
		openL, openR := mrRenderBlocks(synth2, 16)
		open := mrEnergy(openL) + mrEnergy(openR)

		if open <= 4*closed {
			t.Fatalf("filter did not open with CC17: closed %v open %v", closed, open)
		}
	})

	// -----------------------------------------------------------------------
	// 7. Live update of a sustained voice, and no drift when the controller
	//    value stays the same.
	// -----------------------------------------------------------------------
	t.Run("sustained update and stability", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)
		beforeL, beforeR := mrRenderBlocks(synth, 8)
		before := mrEnergy(beforeL) + mrEnergy(beforeR)

		synth.ProcessMidiMessage(0, 0xB0, 0x11, 127)
		mrRenderBlocks(synth, 12) // let the filter smoother converge
		afterL, afterR := mrRenderBlocks(synth, 8)
		after := mrEnergy(afterL) + mrEnergy(afterR)
		if after <= before {
			t.Fatalf("the held voice did not respond to CC17: before %v after %v", before, after)
		}

		// The same controller value for more blocks must not keep drifting.
		stableL, stableR := mrRenderBlocks(synth, 8)
		stable := mrEnergy(stableL) + mrEnergy(stableR)
		if math.Abs(stable-after)/math.Max(stable, after) > 0.02 {
			t.Fatalf("parameters drift with an unchanged controller: %v vs %v", after, stable)
		}
	})

	// -----------------------------------------------------------------------
	// 8. Block boundary: a controller change must not rewrite the PCM block
	//    that was already rendered and is still waiting in the cache.
	// -----------------------------------------------------------------------
	t.Run("block boundary cache", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)

		half := synth.BlockSize / 2
		first := make([]float32, half)
		firstR := make([]float32, half)
		synth.Render(first, firstR)

		synth.ProcessMidiMessage(0, 0xB0, 0x11, 127)

		second := make([]float32, half)
		secondR := make([]float32, half)
		synth.Render(second, secondR)

		// Both halves come from the same pre-change block: equal pan balance.
		if math.Abs(mrEnergy(firstR)/mrEnergy(first)-mrEnergy(secondR)/mrEnergy(second)) > 0.02 {
			t.Fatalf("the cached block was altered by the controller change: %v vs %v",
				mrEnergy(firstR)/mrEnergy(first), mrEnergy(secondR)/mrEnergy(second))
		}

		// The next rendered block is computed after the change and pans right.
		nextL, nextR := mrRenderBlocks(synth, 4)
		if mrEnergy(nextR) <= 20*mrEnergy(nextL) {
			t.Fatalf("the controller change did not take effect at the next block: left %v right %v",
				mrEnergy(nextL), mrEnergy(nextR))
		}
	})

	// -----------------------------------------------------------------------
	// 9. Voice slot reuse: the old note's velocity and routing must not leak.
	// -----------------------------------------------------------------------
	t.Run("voice slot reuse", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(0, 60, 127)
		if synth.voices.activeVoiceCount != 1 {
			t.Fatalf("active voice count = %d, want 1", synth.voices.activeVoiceCount)
		}
		voice := synth.voices.voices[0]

		// Same exclusive class: the slot is reused for the new note.
		synth.NoteOn(0, 60, 32)
		if synth.voices.voices[0] != voice {
			t.Fatal("the exclusive-class voice slot was not reused")
		}
		if voice.velocity != 32 {
			t.Fatalf("reused voice kept the old velocity: %d", voice.velocity)
		}
		if len(voice.mods) == 0 || !voice.hasModulators {
			t.Fatal("the reused voice lost its region modulators")
		}
		mrWarmUp(synth)
		outL, outR := mrRenderBlocks(synth, 4)
		if mrEnergy(outL)+mrEnergy(outR) <= 1e-9 {
			t.Fatal("the reused voice is silent")
		}
	})

	// -----------------------------------------------------------------------
	// 9b. When the polyphony limit is reached, the evicted slot must start the
	//     new note fresh instead of inheriting the old velocity or routing.
	// -----------------------------------------------------------------------
	t.Run("polyphony eviction reuse", func(t *testing.T) {
		// A variant without an exclusive class so every note gets a slot.
		wave := mrBuildWave()
		instZones := []sf2TestZone{
			{
				gens: []sf2TestGen{
					{gen_SampleModes, int16(loop_Continuous)},
					{gen_InitialFilterCutoffFrequency, 6000},
					{gen_DelayVolumeEnvelope, -12000},
					{gen_AttackVolumeEnvelope, -12000},
					{gen_HoldVolumeEnvelope, -12000},
					{gen_DecayVolumeEnvelope, -12000},
					{gen_SustainVolumeEnvelope, 0},
					{gen_SampleID, 0},
				},
				mods: []sf2TestMod{
					{mrSrcCC17, gen_Pan, 300, mrSrcNone, 0},
				},
			},
		}
		presetZones := []sf2TestZone{
			{gens: []sf2TestGen{{gen_Instrument, 0}}},
		}
		synth, _ := mrNewSynth(t, mrAssembleSF2(wave, instZones, presetZones), 8)

		for key := int32(60); key < 68; key++ {
			synth.NoteOn(0, key, 100)
		}
		if synth.voices.activeVoiceCount != 8 {
			t.Fatalf("active voice count = %d, want 8", synth.voices.activeVoiceCount)
		}

		// A ninth note evicts the lowest-priority voice and reuses its slot.
		synth.NoteOn(0, 72, 42)
		found := false
		for i := int32(0); i < synth.voices.activeVoiceCount; i++ {
			v := synth.voices.voices[i]
			if v.key == 72 {
				found = true
				if v.velocity != 42 {
					t.Fatalf("the evicted slot kept the old velocity: %d", v.velocity)
				}
				if !v.hasModulators || len(v.mods) == 0 {
					t.Fatal("the evicted slot lost the new region's modulators")
				}
			}
		}
		if !found {
			t.Fatal("the ninth note did not start on a reused slot")
		}
		mrWarmUp(synth)
		left, right := mrRenderBlocks(synth, 4)
		if mrEnergy(left)+mrEnergy(right) <= 1e-9 {
			t.Fatal("the reused slot renders silence")
		}
	})

	// -----------------------------------------------------------------------
	// 10. Two channels: controller changes and resets are per-channel.
	// -----------------------------------------------------------------------
	t.Run("channel isolation and reset", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.NoteOn(1, 60, 64)
		mrWarmUp(synth)
		baseL, baseR := mrRenderBlocks(synth, 8)

		// Driving CC17 on channel 0 must not touch the voice on channel 1.
		synth.ProcessMidiMessage(0, 0xB0, 0x11, 127)
		mrRenderBlocks(synth, 4)
		otherL, otherR := mrRenderBlocks(synth, 8)
		if math.Abs(mrEnergy(otherL)-mrEnergy(baseL))/mrEnergy(baseL) > 0.02 ||
			math.Abs(mrEnergy(otherR)-mrEnergy(baseR))/mrEnergy(baseR) > 0.02 {
			t.Fatal("a controller change on channel 0 affected channel 1")
		}
		if synth.channels[1].cc17 != 0 || synth.channels[0].cc17 != 127 {
			t.Fatalf("controller state leaked across channels: ch0=%d ch1=%d",
				synth.channels[0].cc17, synth.channels[1].cc17)
		}

		// Reset All Controllers (CC121) clears only the targeted channel.
		synth.ProcessMidiMessage(1, 0xB0, 0x10, 64)
		synth.ProcessMidiMessage(1, 0xB0, 0x11, 65)
		synth.ProcessMidiMessage(0, 0xB0, 0x79, 0)
		if synth.channels[0].cc16 != 0 || synth.channels[0].cc17 != 0 {
			t.Fatal("CC121 did not clear CC16/CC17 on the targeted channel")
		}
		if synth.channels[1].cc16 != 64 || synth.channels[1].cc17 != 65 {
			t.Fatal("CC121 on channel 0 must not reset channel 1")
		}

		// A full Reset clears every channel.
		synth.Reset()
		if synth.channels[0].cc16 != 0 || synth.channels[1].cc17 != 0 {
			t.Fatal("Reset did not clear the modulator controllers")
		}
	})

	// -----------------------------------------------------------------------
	// 11. Corrupt tables must be rejected, never read out of bounds.
	// -----------------------------------------------------------------------
	t.Run("corrupt tables rejected", func(t *testing.T) {
		chunkOffset := func(id string) int {
			idx := bytes.Index(data, []byte(id))
			if idx < 0 {
				t.Fatalf("chunk %q not found", id)
			}
			return idx
		}
		patchU16 := func(blob []byte, chunkID string, recordSize, recordIndex, fieldOffset int, value uint16) {
			offset := chunkOffset(chunkID) + 8 + recordIndex*recordSize + fieldOffset
			binary.LittleEndian.PutUint16(blob[offset:], value)
		}

		// (a) Truncated RIFF data.
		if _, err := NewSoundFont(bytes.NewReader(data[:len(data)-16])); err == nil {
			t.Fatal("truncated data was accepted")
		}

		// (b) Truncated modulator record: shrink the pmod chunk by one record.
		short := bytes.Clone(data)
		pmodOff := chunkOffset("pmod")
		pmodSize := int32(binary.LittleEndian.Uint32(short[pmodOff+4:]))
		binary.LittleEndian.PutUint32(short[pmodOff+4:], uint32(pmodSize-10))
		if _, err := NewSoundFont(bytes.NewReader(short)); err == nil {
			t.Fatal("a truncated pmod chunk was accepted")
		}

		// (c) Bag index going backwards. The global zone's generator index is
		// raised above the local zone's, which is strictly backwards.
		backwards := bytes.Clone(data)
		patchU16(backwards, "ibag", 4, 0, 0, 50)
		if _, err := NewSoundFont(bytes.NewReader(backwards)); err == nil {
			t.Fatal("a backwards bag index was accepted")
		}

		// (d) Bag index outside the generator table.
		outOfRange := bytes.Clone(data)
		patchU16(outOfRange, "ibag", 4, 1, 0, 60000)
		if _, err := NewSoundFont(bytes.NewReader(outOfRange)); err == nil {
			t.Fatal("an out-of-range bag index was accepted")
		}

		// (e) Terminator not pointing at the end of the modulator table.
		// The instrument has a global and a playable zone, so the terminator
		// is bag record 2.
		badTerminator := bytes.Clone(data)
		patchU16(badTerminator, "ibag", 4, 2, 2, 60000)
		if _, err := NewSoundFont(bytes.NewReader(badTerminator)); err == nil {
			t.Fatal("a mismatched bag terminator was accepted")
		}

		// (f) A modulator chunk whose size is not a multiple of one record.
		misaligned := bytes.Clone(data)
		pmodOff2 := chunkOffset("pmod")
		pmodSize2 := int32(binary.LittleEndian.Uint32(misaligned[pmodOff2+4:]))
		binary.LittleEndian.PutUint32(misaligned[pmodOff2+4:], uint32(pmodSize2-3))
		if _, err := NewSoundFont(bytes.NewReader(misaligned)); err == nil {
			t.Fatal("a misaligned modulator chunk was accepted")
		}

		// (g) A bag chunk whose size is not a multiple of one record.
		badBagSize := bytes.Clone(data)
		ibagOff := chunkOffset("ibag")
		ibagSize := int32(binary.LittleEndian.Uint32(badBagSize[ibagOff+4:]))
		binary.LittleEndian.PutUint32(badBagSize[ibagOff+4:], uint32(ibagSize-2))
		if _, err := NewSoundFont(bytes.NewReader(badBagSize)); err == nil {
			t.Fatal("a misaligned bag chunk was accepted")
		}

		// (h) A non-zero modulator terminator record.
		badModTerminator := bytes.Clone(data)
		modEnd := chunkOffset("pmod") + 8 + int(binary.LittleEndian.Uint32(badModTerminator[chunkOffset("pmod")+4:]))
		badModTerminator[modEnd-1] = 1
		if _, err := NewSoundFont(bytes.NewReader(badModTerminator)); err == nil {
			t.Fatal("a non-zero modulator terminator was accepted")
		}
	})

	// -----------------------------------------------------------------------
	// 12. Unsupported modulators never block the SoundFont or its audio.
	// -----------------------------------------------------------------------
	t.Run("unsupported modulators stay silent in audio", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.ProcessMidiMessage(0, 0xB0, 0x10, 127)
		synth.NoteOn(0, 60, 64)
		mrWarmUp(synth)
		left, right := mrRenderBlocks(synth, 8)
		if mrEnergy(left)+mrEnergy(right) <= 1e-9 {
			t.Fatal("the SoundFont with unsupported modulators renders silence")
		}
	})

	// -----------------------------------------------------------------------
	// 13. Numeric contributions match the SF2 source semantics exactly.
	// -----------------------------------------------------------------------
	t.Run("numeric contributions", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.channels[0].cc16 = 64
		synth.channels[0].cc17 = 32
		synth.NoteOn(0, 60, 64)
		v := synth.voices.voices[0]

		attenuation, pan, filterCents := evaluateModulators(v.mods, v)

		cc16 := float32(64) / 127
		cc17 := float32(32) / 127
		velBiNeg := -(2*float32(64)/127 - 1)

		// M3 + M4(zero) + M9 from the instrument layer, PM1 from the preset
		// layer, all in tenths of a dB.
		wantAttenuation := 100*cc16*cc17 + 200*cc16*cc16 + 100*cc17
		if math.Abs(float64(attenuation-wantAttenuation)) > 1e-3 {
			t.Fatalf("attenuation contribution = %v, want %v", attenuation, wantAttenuation)
		}
		// M2 plus the bipolar reversed velocity entry M5, in tenths of a %.
		wantPan := 500*cc17 + 500*velBiNeg
		if math.Abs(float64(pan-wantPan)) > 1e-3 {
			t.Fatalf("pan contribution = %v, want %v", pan, wantPan)
		}
		// M6, in cents.
		wantFilter := 6000 * cc17
		if math.Abs(float64(filterCents-wantFilter)) > 1e-2 {
			t.Fatalf("filter contribution = %v, want %v", filterCents, wantFilter)
		}
	})

	// -----------------------------------------------------------------------
	// 14. Preset and instrument layers add even with identical identities;
	//     replacement is only allowed within one layer.
	// -----------------------------------------------------------------------
	t.Run("cross layer contributions add", func(t *testing.T) {
		wave := mrBuildWave()
		instZones := []sf2TestZone{
			{
				// With no global zone, SampleID must be the last generator:
				// that is how the reader tells a local zone from a global one.
				gens: []sf2TestGen{
					{gen_SampleModes, int16(loop_Continuous)},
					{gen_InitialFilterCutoffFrequency, 6000},
					{gen_DelayVolumeEnvelope, -12000},
					{gen_AttackVolumeEnvelope, -12000},
					{gen_HoldVolumeEnvelope, -12000},
					{gen_DecayVolumeEnvelope, -12000},
					{gen_SustainVolumeEnvelope, 0},
					{gen_SampleID, 0},
				},
				mods: []sf2TestMod{
					{mrSrcCC17, gen_Pan, 300, mrSrcNone, 0},
				},
			},
		}
		presetZones := []sf2TestZone{
			{
				mods: []sf2TestMod{
					// Same identity as the instrument-layer entry, but the
					// layers must be merged by addition, not replacement.
					{mrSrcCC17, gen_Pan, 200, mrSrcNone, 0},
				},
			},
			{
				gens: []sf2TestGen{{gen_Instrument, 0}},
			},
		}
		layered := mrAssembleSF2(wave, instZones, presetZones)

		synth, sf := mrNewSynth(t, layered, 8)
		if sf.Presets[0].Regions[0].mods[0].amount != 200 {
			t.Fatal("the preset-layer modulator was lost")
		}
		if sf.Instruments[0].Regions[0].mods[0].amount != 300 {
			t.Fatal("the instrument-layer modulator was lost")
		}

		synth.channels[0].cc17 = 127
		synth.NoteOn(0, 60, 64)
		_, pan, _ := evaluateModulators(synth.voices.voices[0].mods, synth.voices.voices[0])
		if math.Abs(float64(pan-500)) > 1e-3 {
			t.Fatalf("cross-layer pan contribution = %v, want 500", pan)
		}
	})

	// -----------------------------------------------------------------------
	// 15. The render hot path must not allocate.
	// -----------------------------------------------------------------------
	t.Run("no heap allocation while rendering", func(t *testing.T) {
		synth, _ := mrNewSynth(t, data, 8)
		synth.ProcessMidiMessage(0, 0xB0, 0x10, 64)
		synth.ProcessMidiMessage(0, 0xB0, 0x11, 64)
		synth.NoteOn(0, 60, 64)

		left := make([]float32, synth.BlockSize)
		right := make([]float32, synth.BlockSize)
		mrWarmUp(synth)

		allocs := testing.AllocsPerRun(16, func() {
			synth.Render(left, right)
		})
		if allocs != 0 {
			t.Fatalf("Render allocated %v times per block, want 0", allocs)
		}
	})

	// -----------------------------------------------------------------------
	// 16. The filter contribution is clamped to 1500..13500 cents, and a
	//     duplicate identity inside one zone is replaced by the later record.
	// -----------------------------------------------------------------------
	t.Run("filter clamp and duplicate identity", func(t *testing.T) {
		wave := mrBuildWave()
		instZones := []sf2TestZone{
			{
				gens: []sf2TestGen{
					{gen_SampleModes, int16(loop_Continuous)},
					{gen_InitialFilterCutoffFrequency, 6000},
					{gen_DelayVolumeEnvelope, -12000},
					{gen_AttackVolumeEnvelope, -12000},
					{gen_HoldVolumeEnvelope, -12000},
					{gen_DecayVolumeEnvelope, -12000},
					{gen_SustainVolumeEnvelope, 0},
					{gen_SampleID, 0},
				},
				mods: []sf2TestMod{
					// Two records with the same identity: the later one wins.
					{mrSrcCC17, gen_InitialFilterCutoffFrequency, 100, mrSrcNone, 0},
					{mrSrcCC17, gen_InitialFilterCutoffFrequency, -20000, mrSrcNone, 0},
				},
			},
		}
		presetZones := []sf2TestZone{
			{gens: []sf2TestGen{{gen_Instrument, 0}}},
		}
		clamped := mrAssembleSF2(wave, instZones, presetZones)

		synth, sf := mrNewSynth(t, clamped, 8)
		region := sf.Instruments[0].Regions[0]
		if len(region.mods) != 1 || region.mods[0].amount != -20000 {
			t.Fatalf("duplicate identity was not replaced: %+v", region.mods)
		}

		synth.channels[0].cc17 = 127
		synth.NoteOn(0, 60, 64)
		v := synth.voices.voices[0]
		mrWarmUp(synth)

		// Base 6000 cents minus 20000 would be negative; it must be clamped
		// to the 1500-cent floor, so the live cutoff equals the floor in Hz.
		wantCutoff := calcCentsToHertz(1500)
		if math.Abs(float64(v.cutoff-wantCutoff)) > 1e-3 {
			t.Fatalf("cutoff = %v, want clamped floor %v", v.cutoff, wantCutoff)
		}
	})
}
