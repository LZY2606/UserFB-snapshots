package mp4_test

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
)

// All keys/IVs here are deterministic public test data. Plain samples are
// the first two AAC access units of the upstream, MIT-licensed test fixture.
// This is a starting-state witness, not a rotation implementation.
func testRotationFixture(t *testing.T, rotate bool, splitGroups bool) (*mp4.Fragment, mp4.DecryptInfo, [][]byte, map[string][]byte) {
	t.Helper()
	keyA := bytes.Repeat([]byte{0x11}, 16)
	keyB := bytes.Repeat([]byte{0x22}, 16)
	kidA := mp4.UUID(bytes.Repeat([]byte{0x31}, 16))
	kidB := mp4.UUID(bytes.Repeat([]byte{0x42}, 16))
	iv := bytes.Repeat([]byte{0x53}, 8)
	initFile, err := mp4.ReadMP4File("testdata/aac_init.mp4")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = mp4.InitProtect(initFile.Init, keyA, iv, "cenc", kidA, nil); err != nil {
		t.Fatal(err)
	}
	var initBytes bytes.Buffer
	if err = initFile.Encode(&initBytes); err != nil {
		t.Fatal(err)
	}
	decodedInit, err := mp4.DecodeFile(bytes.NewReader(initBytes.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	di, err := mp4.DecryptInit(decodedInit.Init)
	if err != nil {
		t.Fatal(err)
	}
	media, err := mp4.ReadMP4File("testdata/aac_1.m4s")
	if err != nil {
		t.Fatal(err)
	}
	sourceSamples, err := media.Segments[0].Fragments[0].GetFullSamples(nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(sourceSamples) < 2 {
		t.Fatal("AAC fixture needs two access units")
	}
	trackID := di.TrackInfos[0].TrackID
	f, err := mp4.CreateFragment(7, trackID)
	if err != nil {
		t.Fatal(err)
	}
	senc := mp4.CreateSencBox()
	plain := make([][]byte, 2)
	selectedKey := keyA
	if rotate {
		selectedKey = keyB
	}
	block, err := aes.NewCipher(selectedKey)
	if err != nil {
		t.Fatal(err)
	}
	for i := range plain {
		plain[i] = bytes.Clone(sourceSamples[i].Data)
		s := sourceSamples[i]
		s.Data = bytes.Clone(plain[i])
		sampleIV := bytes.Clone(iv)
		sampleIV[7] += byte(i)
		counter := make([]byte, 16)
		copy(counter, sampleIV)
		cipher.NewCTR(block, counter).XORKeyStream(s.Data, s.Data)
		f.AddFullSample(s)
		if err = senc.AddSample(mp4.SencSample{IV: sampleIV}); err != nil {
			t.Fatal(err)
		}
	}
	if rotate {
		entry := &mp4.SeigSampleGroupEntry{IsProtected: 1, PerSampleIVSize: 8, KID: kidB}
		sgpd := &mp4.SgpdBox{Version: 1, GroupingType: "seig", DefaultLength: 20, SampleGroupEntries: []mp4.SampleGroupEntry{entry}}
		sbgp := &mp4.SbgpBox{GroupingType: "seig", SampleCounts: []uint32{2}, GroupDescriptionIndices: []uint32{65537}}
		if splitGroups {
			sbgp.SampleCounts = []uint32{1, 1}
			sbgp.GroupDescriptionIndices = []uint32{65537, 65537}
		}
		if err = f.Moof.Traf.AddChild(sgpd); err != nil {
			t.Fatal(err)
		}
		if err = f.Moof.Traf.AddChild(sbgp); err != nil {
			t.Fatal(err)
		}
	}
	if err = f.Moof.Traf.AddChild(senc); err != nil {
		t.Fatal(err)
	}
	return f, di, plain, map[string][]byte{hex.EncodeToString(kidA): keyA, hex.EncodeToString(kidB): keyB}
}

func testWireDecode(t *testing.T, f *mp4.Fragment) (*mp4.Fragment, error) {
	t.Helper()
	var b bytes.Buffer
	if err := f.Encode(&b); err != nil {
		t.Fatal(err)
	}
	decoded, err := mp4.DecodeFile(bytes.NewReader(b.Bytes()))
	if err != nil {
		return nil, err
	}
	return decoded.Segments[0].Fragments[0], nil
}

func TestSeed(t *testing.T) {
	for _, rotated := range []bool{false, true} {
		f, di, plain, keys := testRotationFixture(t, rotated, false)
		decoded, err := testWireDecode(t, f)
		if err != nil {
			t.Fatal(err)
		}
		if err = mp4.DecryptFragmentWithKeys(decoded, di, nil, keys, true); err != nil {
			t.Fatal(err)
		}
		samples, err := decoded.GetFullSamples(di.TrackInfos[0].Trex)
		if err != nil {
			t.Fatal(err)
		}
		equal := len(samples) == len(plain)
		for i := range plain {
			equal = equal && bytes.Equal(samples[i].Data, plain[i])
		}
		if !equal {
			if !rotated {
				t.Fatal("default KID does not recover AAC access units")
			}
			t.Fatal("rotated KID does not recover AAC access units")
		}
		if !rotated {
			t.Log("DEFAULT_KID_AAC_BASELINE_OK samples=2")
		} else {
			t.Log("ROTATED_KID_RECOVERED samples=2")
		}
	}
	// The same rotation split into two sbgp runs, whose counts run continuously
	// over the traf, must also decrypt to the original AAC access units.
	f, di, plain, keys := testRotationFixture(t, true, true)
	decoded, err := testWireDecode(t, f)
	if err != nil {
		t.Fatal(err)
	}
	if err = mp4.DecryptFragmentWithKeys(decoded, di, nil, keys, true); err != nil {
		t.Fatalf("multi-group sbgp must be accepted: %v", err)
	}
	samples, err := decoded.GetFullSamples(di.TrackInfos[0].Trex)
	if err != nil {
		t.Fatal(err)
	}
	if len(samples) != len(plain) {
		t.Fatalf("got %d samples, want %d", len(samples), len(plain))
	}
	for i := range plain {
		if !bytes.Equal(samples[i].Data, plain[i]) {
			t.Fatalf("sample %d of split-group fragment does not match plaintext", i)
		}
	}
	t.Log("MULTI_GROUP_SBGP_ACCEPTED samples=2")
}
