package mp4_test

// Tests for in-fragment key rotation through seig (CencSampleEncryptionInformation)
// sample groups. All keys, KIDs and IVs are deterministic public test data, and
// the encrypted bytes are produced with the Go standard library as an
// independent reference rather than only the library's own encryptor.

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"

	"github.com/Eyevinn/mp4ff/bits"
	"github.com/Eyevinn/mp4ff/mp4"
)

// sampleSpec is the full protection specification of one test sample.
type sampleSpec struct {
	plain      []byte
	protected  bool
	key        []byte
	kid        mp4.UUID
	ivSize     byte   // 8 or 16 for per-sample IVs, 0 for a constant IV or a clear sample
	iv         []byte // per-sample IV, or the 16-byte constant IV when ivSize is 0
	cryptBlock byte
	skipBlock  byte
	subsamples []mp4.SubSamplePattern
	flags      uint32
	dur        uint32
	cto        int32
}

func testKID(b byte) mp4.UUID {
	return mp4.UUID(bytes.Repeat([]byte{b}, 16))
}

func testKey(b byte) []byte {
	return bytes.Repeat([]byte{b}, 16)
}

func distinctPlain(tag byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte((i*7 + int(tag)) & 0xff)
	}
	return b
}

// refCencEncrypt is the independent Go-standard-library cenc reference. One CTR
// stream runs through the whole sample, including its clear subsample ranges.
func refCencEncrypt(data, key, iv []byte, subs []mp4.SubSamplePattern) []byte {
	out := bytes.Clone(data)
	block, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	counter := make([]byte, 16) // an 8-byte IV is zero-padded into the high half
	copy(counter, iv)
	stream := cipher.NewCTR(block, counter)
	if len(subs) == 0 {
		stream.XORKeyStream(out, out)
		return out
	}
	pos := 0
	for _, ss := range subs {
		pos += int(ss.BytesOfClearData)
		end := pos + int(ss.BytesOfProtectedData)
		if end > len(out) {
			// Malformed fixtures intentionally declare ranges beyond the
			// sample; only the existing bytes can be encrypted, and the library
			// rejects such a senc before decrypting.
			end = len(out)
		}
		if pos < end {
			stream.XORKeyStream(out[pos:end], out[pos:end])
		}
		pos = end
	}
	return out
}

// refCbcsEncrypt is the independent Go-standard-library cbcs reference. One CBC
// chain runs through the crypt stripes of a whole sample, the final partial
// block stays clear, and a zero pattern means full-sample CBC.
func refCbcsEncrypt(data, key, iv []byte, cryptBlock, skipBlock byte) []byte {
	out := bytes.Clone(data)
	block, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	cph := cipher.NewCBCEncrypter(block, iv)
	cryptBytes := int(cryptBlock) * 16
	skipBytes := int(skipBlock) * 16
	if cryptBytes == 0 && skipBytes == 0 {
		cph.CryptBlocks(out[:len(out)&^0xf], out[:len(out)&^0xf])
		return out
	}
	pos := 0
	for len(out)-pos >= cryptBytes {
		cph.CryptBlocks(out[pos:pos+cryptBytes], out[pos:pos+cryptBytes])
		pos += cryptBytes
		if len(out)-pos < skipBytes {
			break
		}
		pos += skipBytes
	}
	return out
}

// boxWithHeader prepends a 4-byte size and 4-byte type to payload.
func boxWithHeader(typ string, payload []byte) []byte {
	out := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint32(out, uint32(8+len(payload)))
	copy(out[4:8], typ)
	copy(out[8:], payload)
	return out
}

// rawSenc writes a complete senc box by hand, consuming each sample's IV and
// optional subsample entries exactly as its effective parameters dictate.
func rawSenc(specs []sampleSpec, useSubsamples bool) []byte {
	var body bytes.Buffer
	flags := uint32(0)
	if useSubsamples {
		flags = mp4.UseSubSampleEncryption
	}
	var vf [4]byte
	binary.BigEndian.PutUint32(vf[:], flags)
	body.Write(vf[:])
	binary.Write(&body, binary.BigEndian, uint32(len(specs)))
	for _, sp := range specs {
		if sp.protected && sp.ivSize > 0 {
			body.Write(sp.iv)
		}
		if useSubsamples {
			binary.Write(&body, binary.BigEndian, uint16(len(sp.subsamples)))
			for _, ss := range sp.subsamples {
				binary.Write(&body, binary.BigEndian, ss.BytesOfClearData)
				binary.Write(&body, binary.BigEndian, ss.BytesOfProtectedData)
			}
		}
	}
	return boxWithHeader("senc", body.Bytes())
}

// auxSize is the number of auxiliary bytes one sample consumes.
func auxSize(sp sampleSpec, useSubsamples bool) uint32 {
	if !sp.protected {
		return 0
	}
	size := uint32(sp.ivSize)
	if useSubsamples {
		size += 2 + uint32(len(sp.subsamples))*6
	}
	return size
}

// rawSaiz writes a version-0 saiz box by hand with one entry per sample that
// actually carries auxiliary information, as SaizBox.AddSampleInfo records them.
func rawSaiz(specs []sampleSpec, useSubsamples bool) []byte {
	var sizes []uint32
	for _, sp := range specs {
		if size := auxSize(sp, useSubsamples); size > 0 {
			sizes = append(sizes, size)
		}
	}
	var body bytes.Buffer
	body.Write([]byte{0, 0, 0, 0}) // version 0, no flags
	body.WriteByte(0)              // DefaultSampleInfoSize 0: per-sample sizes follow
	binary.Write(&body, binary.BigEndian, uint32(len(sizes)))
	for _, size := range sizes {
		body.WriteByte(byte(size))
	}
	return boxWithHeader("saiz", body.Bytes())
}

func decodeRawBox(t *testing.T, raw []byte) mp4.Box {
	t.Helper()
	box, err := mp4.DecodeBox(0, bytes.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	return box
}

// seigEntryKey identifies an equivalent seig description, so equal samples
// share one sgpd entry.
type seigEntryKey struct {
	kidHex     string
	protected  bool
	ivSize     byte
	cryptBlock byte
	skipBlock  byte
	constIVHex string
}

func seigKey(sp sampleSpec) seigEntryKey {
	return seigEntryKey{
		kidHex:     hex.EncodeToString(sp.kid),
		protected:  sp.protected,
		ivSize:     sp.ivSize,
		cryptBlock: sp.cryptBlock,
		skipBlock:  sp.skipBlock,
		constIVHex: hex.EncodeToString(sp.iv),
	}
}

// addSeigGroup builds the sgpd/sbgp boxes of one track from the samples that
// differ from the tenc defaults, with sbgp counts running over every trun.
func addSeigGroup(t *testing.T, traf *mp4.TrafBox, specs []sampleSpec, tenc *mp4.TencBox) {
	t.Helper()
	defaultKID := tenc.DefaultKID
	// matchesTenc reports whether a protected sample is fully described by the
	// tenc defaults, so that it needs no seig description of its own.
	matchesTenc := func(sp sampleSpec) bool {
		if !sp.protected || !bytes.Equal(sp.kid, defaultKID) {
			return false
		}
		if sp.ivSize != tenc.DefaultPerSampleIVSize {
			return false
		}
		if sp.cryptBlock != tenc.DefaultCryptByteBlock || sp.skipBlock != tenc.DefaultSkipByteBlock {
			return false
		}
		if sp.ivSize == 0 && !bytes.Equal(sp.iv, tenc.DefaultConstantIV) {
			return false
		}
		return true
	}
	var entries []*mp4.SeigSampleGroupEntry
	var lengths []uint32
	indexOf := map[seigEntryKey]int{}
	// entryOf resolves one sample to an sgpd entry index, or -1 for the tenc defaults.
	entryOf := func(sp sampleSpec) int {
		if matchesTenc(sp) {
			return -1
		}
		key := seigKey(sp)
		idx, ok := indexOf[key]
		if !ok {
			entry := &mp4.SeigSampleGroupEntry{
				CryptByteBlock:  sp.cryptBlock,
				SkipByteBlock:   sp.skipBlock,
				IsProtected:     0,
				PerSampleIVSize: sp.ivSize,
				KID:             sp.kid,
			}
			if sp.protected {
				entry.IsProtected = 1
				if sp.ivSize == 0 {
					entry.ConstantIV = bytes.Clone(sp.iv)
				}
			}
			idx = len(entries)
			indexOf[key] = idx
			entries = append(entries, entry)
			lengths = append(lengths, uint32(entry.Size()))
		}
		return idx
	}
	// The sbgp counts run continuously over every sample, in decode order.
	// A group_description_index of 0 marks a run that keeps the tenc defaults.
	type groupRun struct {
		entryIdx int // -1 for the tenc defaults
		count    int
	}
	var groups []groupRun
	for _, sp := range specs {
		idx := entryOf(sp)
		if n := len(groups); n > 0 && groups[n-1].entryIdx == idx {
			groups[n-1].count++
		} else {
			groups = append(groups, groupRun{entryIdx: idx, count: 1})
		}
	}
	if len(entries) == 0 {
		return
	}
	sgpEntries := make([]mp4.SampleGroupEntry, len(entries))
	for i, e := range entries {
		sgpEntries[i] = e
	}
	sgpd := &mp4.SgpdBox{
		Version:            1,
		GroupingType:       "seig",
		DescriptionLengths: lengths,
		SampleGroupEntries: sgpEntries,
	}
	sbgp := &mp4.SbgpBox{GroupingType: "seig"}
	for _, g := range groups {
		sbgp.SampleCounts = append(sbgp.SampleCounts, uint32(g.count))
		if g.entryIdx < 0 {
			sbgp.GroupDescriptionIndices = append(sbgp.GroupDescriptionIndices, 0)
		} else {
			sbgp.GroupDescriptionIndices = append(sbgp.GroupDescriptionIndices, uint32(65536+g.entryIdx+1))
		}
	}
	if err := traf.AddChild(sgpd); err != nil {
		t.Fatal(err)
	}
	if err := traf.AddChild(sbgp); err != nil {
		t.Fatal(err)
	}
}

// rotationTrack describes one track of a rotation fragment.
type rotationTrack struct {
	trackID  uint32
	tenc     *mp4.TencBox
	specs    []sampleSpec
	trunAt   []int // start sample index of each trun; nil means one trun
	noAux    bool  // omit senc/saiz/saio (constant IVs and clear samples only)
	keepRoll bool  // add a non-seig "roll" sample group that decryption must retain
}

// buildRotationFragment assembles a fragment from real box bytes: multi-trun
// traf boxes, seig groups and hand-written senc/saiz plus an saio box. The
// returned bytes are the encrypted fragment on the wire. Runs of different
// tracks are added in alternation, so the created trun boxes get write orders
// that match their order in mdat.
func buildRotationFragment(t *testing.T, seq uint32, tracks []rotationTrack, scheme string,
	useSubsamples bool) []byte {
	t.Helper()
	trackIDs := make([]uint32, len(tracks))
	for i, tr := range tracks {
		trackIDs[i] = tr.trackID
	}
	frag, err := mp4.CreateMultiTrackFragment(seq, trackIDs)
	if err != nil {
		t.Fatal(err)
	}
	for _, tr := range tracks {
		traf := trafForTrack(t, frag, tr.trackID)
		if err := traf.AddChild(&mp4.TfdtBox{Version: 1}); err != nil {
			t.Fatal(err)
		}
	}
	maxRuns := 0
	for _, tr := range tracks {
		n := len(tr.trunAt)
		if tr.trunAt == nil {
			n = 1
		}
		maxRuns = max(maxRuns, n)
	}
	// Add the runs in interleaved order so trun write orders match mdat.
	for r := 0; r < maxRuns; r++ {
		for _, tr := range tracks {
			trunStarts := tr.trunAt
			if trunStarts == nil {
				trunStarts = []int{0}
			}
			if r >= len(trunStarts) {
				continue
			}
			end := len(tr.specs)
			if r+1 < len(trunStarts) {
				end = trunStarts[r+1]
			}
			for i := trunStarts[r]; i < end; i++ {
				sp := tr.specs[i]
				var encrypted []byte
				if !sp.protected {
					encrypted = bytes.Clone(sp.plain)
				} else if scheme == "cenc" {
					encrypted = refCencEncrypt(sp.plain, sp.key, sp.iv, sp.subsamples)
				} else {
					encrypted = refCbcsEncrypt(sp.plain, sp.key, sp.iv, sp.cryptBlock, sp.skipBlock)
				}
				fs := mp4.FullSample{
					Sample: mp4.Sample{
						Flags:                 sp.flags,
						Dur:                   sp.dur,
						Size:                  uint32(len(encrypted)),
						CompositionTimeOffset: sp.cto,
					},
					DecodeTime: uint64(i) * uint64(sp.dur),
					Data:       encrypted,
				}
				if err := frag.AddFullSampleToTrack(fs, tr.trackID); err != nil {
					t.Fatal(err)
				}
			}
		}
	}
	for _, tr := range tracks {
		traf := trafForTrack(t, frag, tr.trackID)
		addSeigGroup(t, traf, tr.specs, tr.tenc)
		if tr.keepRoll {
			// A different sample group type that must survive decryption.
			rollSgpd := &mp4.SgpdBox{
				Version: 1, GroupingType: "roll", DefaultLength: 2,
				SampleGroupEntries: []mp4.SampleGroupEntry{&mp4.RollSampleGroupEntry{RollDistance: -1}},
			}
			rollSbgp := &mp4.SbgpBox{
				GroupingType:            "roll",
				SampleCounts:            []uint32{uint32(len(tr.specs))},
				GroupDescriptionIndices: []uint32{65537},
			}
			if err := traf.AddChild(rollSgpd); err != nil {
				t.Fatal(err)
			}
			if err := traf.AddChild(rollSbgp); err != nil {
				t.Fatal(err)
			}
		}
		if tr.noAux {
			continue
		}
		senc := decodeRawBox(t, rawSenc(tr.specs, useSubsamples)).(*mp4.SencBox)
		saiz := decodeRawBox(t, rawSaiz(tr.specs, useSubsamples)).(*mp4.SaizBox)
		if err := traf.AddChild(saiz); err != nil {
			t.Fatal(err)
		}
		if err := traf.AddChild(senc); err != nil {
			t.Fatal(err)
		}
	}
	// Compute every inline senc offset before appending the saio boxes. Each
	// traf that carries a senc will get one 20-byte saio, so reserve that size
	// while walking the tree; otherwise inserting an earlier traf's saio shifts
	// the senc position of every later traf. Offsets are relative to moof.
	moof := frag.Moof
	const saioSize = 20
	type sencTarget struct {
		traf   *mp4.TrafBox
		offset int64
	}
	var targets []sencTarget
	offset := uint64(8) // moof header
	for _, c := range moof.Children {
		traf, ok := c.(*mp4.TrafBox)
		if !ok {
			offset += c.Size()
			continue
		}
		offset += 8 // traf header
		var sencOffset uint64
		for _, tc := range traf.Children {
			if tc.Type() == "senc" {
				sencOffset = offset + 12 + 4 // full box header 12, then sample count 4
			}
			offset += tc.Size()
		}
		if sencOffset > 0 {
			targets = append(targets, sencTarget{traf: traf, offset: int64(sencOffset)})
			offset += saioSize // the saio box that is appended below
		}
	}
	for _, target := range targets {
		if got := (&mp4.SaioBox{Version: 0, Offset: []int64{target.offset}}).Size(); got != saioSize {
			t.Fatalf("test helper assumes a %d-byte saio, got %d", saioSize, got)
		}
		if err := target.traf.AddChild(&mp4.SaioBox{Version: 0, Offset: []int64{target.offset}}); err != nil {
			t.Fatal(err)
		}
	}
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func trafForTrack(t *testing.T, frag *mp4.Fragment, trackID uint32) *mp4.TrafBox {
	t.Helper()
	for _, traf := range frag.Moof.Trafs {
		if traf.Tfhd.TrackID == trackID {
			return traf
		}
	}
	t.Fatalf("no traf for trackID %d", trackID)
	return nil
}

func decryptInfoFor(tracks []rotationTrack, scheme string) mp4.DecryptInfo {
	di := mp4.DecryptInfo{}
	for _, tr := range tracks {
		di.TrackInfos = append(di.TrackInfos, mp4.DecryptTrackInfo{
			TrackID: tr.trackID,
			Trex:    &mp4.TrexBox{TrackID: tr.trackID},
			Sinf: &mp4.SinfBox{
				Schm: &mp4.SchmBox{SchemeType: scheme},
				Schi: &mp4.SchiBox{Tenc: tr.tenc},
			},
		})
	}
	return di
}

func keyDictionary(tracks []rotationTrack) map[string][]byte {
	keys := map[string][]byte{}
	for _, tr := range tracks {
		for _, sp := range tr.specs {
			if sp.protected {
				keys[hex.EncodeToString(sp.kid)] = sp.key
			}
		}
	}
	return keys
}

func checkPlaintext(t *testing.T, frag *mp4.Fragment, tracks []rotationTrack) {
	t.Helper()
	for _, tr := range tracks {
		trex := &mp4.TrexBox{TrackID: tr.trackID}
		samples, err := frag.GetFullSamples(trex)
		if err != nil {
			t.Fatal(err)
		}
		if len(samples) != len(tr.specs) {
			t.Fatalf("track %d: got %d samples, want %d", tr.trackID, len(samples), len(tr.specs))
		}
		for i, sp := range tr.specs {
			if !bytes.Equal(samples[i].Data, sp.plain) {
				t.Fatalf("track %d sample %d: decrypted data does not match plaintext", tr.trackID, i+1)
			}
			if samples[i].Dur != sp.dur || samples[i].Flags != sp.flags ||
				samples[i].CompositionTimeOffset != sp.cto {
				t.Fatalf("track %d sample %d: metadata changed (dur=%d flags=%#x cto=%d)",
					tr.trackID, i+1, samples[i].Dur, samples[i].Flags, samples[i].CompositionTimeOffset)
			}
			if want := uint64(i) * uint64(sp.dur); samples[i].DecodeTime != want {
				t.Fatalf("track %d sample %d: DTS %d, want %d", tr.trackID, i+1, samples[i].DecodeTime, want)
			}
		}
	}
}

// cencRotationTracks builds the shared two-track cenc fixture: interleaved
// tracks, two truns with a group switch in the middle, 8/16-byte IV switching,
// and a clear sample sandwiched between encrypted ones.
func cencRotationTracks() []rotationTrack {
	kidA, kidB, kidC := testKID(0x31), testKID(0x42), testKID(0x53)
	keyA, keyB, keyC := testKey(0x11), testKey(0x22), testKey(0x33)
	iv := func(b byte, n int) []byte {
		out := make([]byte, n)
		for i := range out {
			out[i] = b + byte(i)
		}
		return out
	}
	mk := func(tag byte, size int, protected bool, kid mp4.UUID, key []byte, ivSize byte, ivVal []byte) sampleSpec {
		sp := sampleSpec{
			plain:     distinctPlain(tag, size),
			protected: protected,
			kid:       kid,
			key:       key,
			ivSize:    ivSize,
			iv:        ivVal,
			flags:     mp4.SyncSampleFlags,
			dur:       1024,
		}
		return sp
	}
	t1 := []sampleSpec{
		mk(1, 40, true, kidA, keyA, 8, iv(0x50, 8)),
		mk(2, 33, true, kidB, keyB, 16, iv(0x60, 16)),
		mk(3, 50, true, kidB, keyB, 8, iv(0x70, 8)),
		mk(4, 24, false, kidA, nil, 0, nil), // clear sample between encrypted ones
		mk(5, 45, true, kidA, keyA, 8, iv(0x80, 8)),
		mk(6, 37, true, kidB, keyB, 16, iv(0x90, 16)),
	}
	t1[2].cto = 512
	t1[4].cto = -256
	t2 := []sampleSpec{
		mk(7, 28, true, kidC, keyC, 8, iv(0xa0, 8)),
		mk(8, 52, true, kidC, keyC, 16, iv(0xb0, 16)),
		mk(9, 17, false, kidC, nil, 0, nil),
		mk(10, 41, true, kidC, keyC, 8, iv(0xc0, 8)),
	}
	tenc1 := &mp4.TencBox{Version: 0, DefaultIsProtected: 1, DefaultPerSampleIVSize: 8, DefaultKID: kidA}
	tenc2 := &mp4.TencBox{Version: 0, DefaultIsProtected: 1, DefaultPerSampleIVSize: 8, DefaultKID: kidC}
	return []rotationTrack{
		{trackID: 1, tenc: tenc1, specs: t1, trunAt: []int{0, 3}},
		{trackID: 2, tenc: tenc2, specs: t2},
	}
}

func childTypes(traf *mp4.TrafBox) []string {
	types := make([]string, 0, len(traf.Children))
	for _, c := range traf.Children {
		types = append(types, c.Type())
	}
	return types
}

func TestSeigCencRotation(t *testing.T) {
	tracks := cencRotationTracks()
	tracks[0].keepRoll = true // a non-seig sample group must survive decryption
	wire := buildRotationFragment(t, 7, tracks, "cenc", false)

	// A per-sample parse of the mixed 8/16-byte IV senc re-encodes to the same
	// hand-written box bytes, including the clear sample that carries no IV.
	preFile, err := mp4.DecodeFile(bytes.NewReader(bytes.Clone(wire)))
	if err != nil {
		t.Fatal(err)
	}
	preTraf := preFile.Segments[0].Fragments[0].Moof.Trafs[0]
	ivSizes := make([]byte, len(tracks[0].specs))
	for i, sp := range tracks[0].specs {
		if sp.protected {
			ivSizes[i] = sp.ivSize
		}
	}
	if err := preTraf.Senc.ParseReadBoxPerSample(ivSizes); err != nil {
		t.Fatal(err)
	}
	var sencBuf bytes.Buffer
	if err := preTraf.Senc.Encode(&sencBuf); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(sencBuf.Bytes(), rawSenc(tracks[0].specs, false)) {
		t.Fatal("mixed-IV-width senc does not re-encode to its original box bytes")
	}

	f, err := mp4.DecodeFile(bytes.NewReader(wire))
	if err != nil {
		t.Fatal(err)
	}
	frag := f.Segments[0].Fragments[0]
	di := decryptInfoFor(tracks, "cenc")
	keys := keyDictionary(tracks)

	if err := mp4.DecryptFragmentWithKeys(frag, di, nil, keys, true); err != nil {
		t.Fatalf("DecryptFragmentWithKeys: %v", err)
	}
	checkPlaintext(t, frag, tracks)

	for _, tr := range tracks {
		traf := trafForTrack(t, frag, tr.trackID)
		if traf.Senc != nil || traf.Saiz != nil || traf.Saio != nil {
			t.Fatalf("track %d: encryption auxiliary boxes were not removed", tr.trackID)
		}
		var foundSeig, foundRoll bool
		for _, c := range traf.Children {
			switch b := c.(type) {
			case *mp4.SbgpBox:
				if b.GroupingType == "seig" {
					foundSeig = true
				}
				if b.GroupingType == "roll" {
					foundRoll = true
				}
			case *mp4.SgpdBox:
				if b.GroupingType == "seig" {
					foundSeig = true
				}
				if b.GroupingType == "roll" {
					foundRoll = true
				}
			}
		}
		if foundSeig {
			t.Fatalf("track %d: seig sample group was not removed", tr.trackID)
		}
		if tr.keepRoll && !foundRoll {
			t.Fatalf("track %d: non-seig roll sample group was removed", tr.trackID)
		}
		if len(traf.Truns) != len(tr.trunAt) && tr.trunAt != nil {
			t.Fatalf("track %d: got %d truns, want %d", tr.trackID, len(traf.Truns), len(tr.trunAt))
		}
	}

	// The re-encoded fragment decodes again with the same sample association.
	var re bytes.Buffer
	if err := frag.Encode(&re); err != nil {
		t.Fatal(err)
	}
	reFile, err := mp4.DecodeFile(bytes.NewReader(re.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	checkPlaintext(t, reFile.Segments[0].Fragments[0], tracks)
}

func TestSeigCbcsRotation(t *testing.T) {
	kidA, kidB := testKID(0x31), testKID(0x42)
	keyA, keyB := testKey(0x11), testKey(0x22)
	constIV1 := bytes.Repeat([]byte{0x61}, 16)
	constIV2 := bytes.Repeat([]byte{0x62}, 16)
	perIV := func(b byte) []byte {
		out := make([]byte, 16)
		for i := range out {
			out[i] = b + byte(i)
		}
		return out
	}
	mk := func(tag byte, size int, protected bool, kid mp4.UUID, key []byte, ivSize byte,
		ivVal []byte, crypt, skip byte) sampleSpec {
		return sampleSpec{
			plain:      distinctPlain(tag, size),
			protected:  protected,
			kid:        kid,
			key:        key,
			ivSize:     ivSize,
			iv:         ivVal,
			cryptBlock: crypt,
			skipBlock:  skip,
			flags:      mp4.SyncSampleFlags,
			dur:        1024,
		}
	}
	// Pattern and IV rule change from sample to sample.
	t1 := []sampleSpec{
		mk(1, 64, true, kidA, keyA, 0, constIV1, 1, 9),     // tenc default: constant IV, 1/9 pattern
		mk(2, 80, true, kidB, keyB, 16, perIV(0x70), 1, 9), // per-sample IV
		mk(3, 48, true, kidA, keyA, 0, constIV2, 0, 0),     // other constant IV, full-sample CBC
		mk(4, 96, true, kidB, keyB, 16, perIV(0x80), 2, 7), // per-sample IV with 2/7 pattern
		mk(5, 32, false, kidA, nil, 0, nil, 0, 0),          // clear sample
		mk(6, 64, true, kidA, keyA, 0, constIV1, 1, 9),     // back to the tenc defaults
	}
	tenc1 := &mp4.TencBox{
		Version: 1, DefaultCryptByteBlock: 1, DefaultSkipByteBlock: 9,
		DefaultIsProtected: 1, DefaultPerSampleIVSize: 0, DefaultKID: kidA,
		DefaultConstantIV: constIV1,
	}
	// A second track with constant IVs only: the auxiliary boxes are omitted.
	t2 := []sampleSpec{
		mk(7, 48, true, kidA, keyA, 0, constIV1, 0, 0),
		mk(8, 32, false, kidA, nil, 0, nil, 0, 0),
		mk(9, 64, true, kidA, keyA, 0, constIV1, 0, 0),
	}
	tenc2 := &mp4.TencBox{
		Version: 1, DefaultIsProtected: 1, DefaultPerSampleIVSize: 0, DefaultKID: kidA,
		DefaultConstantIV: constIV1,
	}
	tracks := []rotationTrack{
		{trackID: 1, tenc: tenc1, specs: t1, trunAt: []int{0, 3}},
		{trackID: 2, tenc: tenc2, specs: t2, noAux: true},
	}
	wire := buildRotationFragment(t, 9, tracks, "cbcs", false)
	f, err := mp4.DecodeFile(bytes.NewReader(wire))
	if err != nil {
		t.Fatal(err)
	}
	frag := f.Segments[0].Fragments[0]
	if frag.Moof.Trafs[1].Senc != nil || frag.Moof.Trafs[1].Saiz != nil {
		t.Fatal("constant-IV track should carry no auxiliary boxes")
	}
	di := decryptInfoFor(tracks, "cbcs")
	if err := mp4.DecryptFragmentWithKeys(frag, di, nil, keyDictionary(tracks), true); err != nil {
		t.Fatalf("DecryptFragmentWithKeys: %v", err)
	}
	checkPlaintext(t, frag, tracks)

	var re bytes.Buffer
	if err := frag.Encode(&re); err != nil {
		t.Fatal(err)
	}
	reFile, err := mp4.DecodeFile(bytes.NewReader(re.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	checkPlaintext(t, reFile.Segments[0].Fragments[0], tracks)
}

func TestSeigDecodePaths(t *testing.T) {
	// Single-track cenc fixture whose track matches the protected AAC init segment.
	initFile, err := mp4.ReadMP4File("testdata/aac_init.mp4")
	if err != nil {
		t.Fatal(err)
	}
	keyA := testKey(0x11)
	kidA := testKID(0x31)
	kidB := testKID(0x42)
	keyB := testKey(0x22)
	iv8 := bytes.Repeat([]byte{0x53}, 8)
	if _, err := mp4.InitProtect(initFile.Init, keyA, iv8, "cenc", kidA, nil); err != nil {
		t.Fatal(err)
	}
	var initBuf bytes.Buffer
	if err := initFile.Encode(&initBuf); err != nil {
		t.Fatal(err)
	}
	decodedInit, err := mp4.DecodeFile(bytes.NewReader(initBuf.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	di, err := mp4.DecryptInit(decodedInit.Init)
	if err != nil {
		t.Fatal(err)
	}
	trackID := di.TrackInfos[0].TrackID
	tenc := di.TrackInfos[0].Sinf.Schi.Tenc

	specs := []sampleSpec{
		{plain: distinctPlain(1, 40), protected: true, kid: kidA, key: keyA, ivSize: 8,
			iv: bytes.Repeat([]byte{0x50}, 8), flags: mp4.SyncSampleFlags, dur: 1024},
		{plain: distinctPlain(2, 33), protected: true, kid: kidB, key: keyB, ivSize: 16,
			iv: bytes.Repeat([]byte{0x60}, 16), flags: mp4.SyncSampleFlags, dur: 1024},
		{plain: distinctPlain(3, 24), protected: false, kid: kidA, flags: mp4.SyncSampleFlags, dur: 1024},
		{plain: distinctPlain(4, 45), protected: true, kid: kidA, key: keyA, ivSize: 8,
			iv: bytes.Repeat([]byte{0x80}, 8), flags: mp4.SyncSampleFlags, dur: 1024},
	}
	tracks := []rotationTrack{{trackID: trackID, tenc: tenc, specs: specs}}
	wire := buildRotationFragment(t, 3, tracks, "cenc", false)
	keys := keyDictionary(tracks)

	// Media-only decode: without the init segment the tenc IV width is unknown,
	// so the senc stays deferred until ParseSenc supplies the init segment.
	mediaOnly, err := mp4.DecodeFile(bytes.NewReader(wire))
	if err != nil {
		t.Fatal(err)
	}
	mediaFrag := mediaOnly.Segments[0].Fragments[0]
	if senc := mediaFrag.Moof.Traf.Senc; senc == nil || !senc.ReadButNotParsed() {
		t.Fatal("senc should stay deferred while the init segment is missing")
	}
	if err := mediaOnly.Segments[0].ParseSenc(decodedInit.Init); err != nil {
		t.Fatal(err)
	}
	if err := mp4.DecryptFragmentWithKeys(mediaFrag, di, nil, keys, true); err != nil {
		t.Fatalf("decrypt after ParseSenc: %v", err)
	}
	checkPlaintext(t, mediaFrag, tracks)

	decodeAndDecrypt := func(name string, decode func(data []byte) (*mp4.Fragment, error)) {
		t.Helper()
		// DecodeFileSR and FragmentDecoder expose the mdat payload as a view
		// into the input slice, and decryption edits it in place, so every path
		// gets its own copy of the wire bytes.
		frag, err := decode(bytes.Clone(wire))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if err := mp4.DecryptFragmentWithKeys(frag, di, nil, keys, true); err != nil {
			t.Fatalf("%s: decrypt: %v", name, err)
		}
		checkPlaintext(t, frag, tracks)
	}
	// io.Reader path.
	decodeAndDecrypt("DecodeFile", func(data []byte) (*mp4.Fragment, error) {
		f, err := mp4.DecodeFile(bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		return f.Segments[0].Fragments[0], nil
	})
	// SliceReader path.
	decodeAndDecrypt("DecodeFileSR", func(data []byte) (*mp4.Fragment, error) {
		f, err := mp4.DecodeFileSR(bits.NewFixedSliceReader(data))
		if err != nil {
			return nil, err
		}
		return f.Segments[0].Fragments[0], nil
	})
	// FragmentDecoder path.
	decodeAndDecrypt("FragmentDecoder", func(data []byte) (*mp4.Fragment, error) {
		var d mp4.FragmentDecoder
		var got *mp4.Fragment
		decodeAllFragments(t, &d, data, func(_ int, f *mp4.Fragment) { got = f })
		if got == nil {
			return nil, fmt.Errorf("no fragment decoded")
		}
		return got, nil
	})
	// Encode the parsed fragment again and read it back before decrypting.
	decodeAndDecrypt("re-encode then decode", func(data []byte) (*mp4.Fragment, error) {
		f, err := mp4.DecodeFileSR(bits.NewFixedSliceReader(data))
		if err != nil {
			return nil, err
		}
		var b bytes.Buffer
		if err := f.Segments[0].Fragments[0].Encode(&b); err != nil {
			return nil, err
		}
		f2, err := mp4.DecodeFile(bytes.NewReader(b.Bytes()))
		if err != nil {
			return nil, err
		}
		return f2.Segments[0].Fragments[0], nil
	})
}

func TestSeigAtomicFailure(t *testing.T) {
	tracks := cencRotationTracks()
	di := decryptInfoFor(tracks, "cenc")
	fullKeys := keyDictionary(tracks)

	// A later track is missing its key in strict mode: nothing may change.
	t.Run("missing key leaves fragment untouched and retries", func(t *testing.T) {
		wire := buildRotationFragment(t, 11, tracks, "cenc", false)
		f, err := mp4.DecodeFile(bytes.NewReader(wire))
		if err != nil {
			t.Fatal(err)
		}
		frag := f.Segments[0].Fragments[0]
		incomplete := map[string][]byte{}
		for k, v := range fullKeys {
			incomplete[k] = v
		}
		kidCHex := hex.EncodeToString(testKID(0x53))
		delete(incomplete, kidCHex)
		keysBefore := len(incomplete)
		err = mp4.DecryptFragmentWithKeys(frag, di, nil, incomplete, true)
		if err == nil {
			t.Fatal("expected error for missing key")
		}
		msg := err.Error()
		if !strings.Contains(msg, "trackID=2") {
			t.Fatalf("error %q must name the failing track", msg)
		}
		if strings.Contains(msg, hex.EncodeToString(testKey(0x33))) {
			t.Fatal("error message must not contain key material")
		}
		if len(incomplete) != keysBefore {
			t.Fatal("the key dictionary was modified by the failed call")
		}
		// The media payload, box list, offsets and deferred senc state are intact.
		if payload := frag.Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire)) {
			t.Fatal("mdat payload was modified by the failed decryption")
		}
		for _, tr := range tracks {
			traf := trafForTrack(t, frag, tr.trackID)
			if traf.Senc == nil || traf.Saiz == nil || traf.Saio == nil {
				t.Fatalf("track %d: auxiliary boxes removed by failed call", tr.trackID)
			}
			if !traf.Senc.ReadButNotParsed() {
				t.Fatalf("track %d: senc parse state was not restored", tr.trackID)
			}
		}
		// Retry on the same object after supplying the missing key.
		incomplete[kidCHex] = fullKeys[kidCHex]
		if err := mp4.DecryptFragmentWithKeys(frag, di, nil, incomplete, true); err != nil {
			t.Fatalf("retry after adding key: %v", err)
		}
		checkPlaintext(t, frag, tracks)
	})

	// Strict mode with an empty dictionary must not silently use a legacy key.
	t.Run("empty dictionary in strict mode fails", func(t *testing.T) {
		wire := buildRotationFragment(t, 15, tracks, "cenc", false)
		f, err := mp4.DecodeFile(bytes.NewReader(wire))
		if err != nil {
			t.Fatal(err)
		}
		legacyKey := testKey(0x99)
		err = mp4.DecryptFragmentWithKeys(f.Segments[0].Fragments[0], di, legacyKey, nil, true)
		if err == nil {
			t.Fatal("strict mode with an empty dictionary must fail, not fall back to the legacy key")
		}
		if payload := f.Segments[0].Fragments[0].Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire)) {
			t.Fatal("mdat payload was modified by the failed decryption")
		}
		// The same call succeeds outside strict mode using the legacy key when
		// the dictionary stays empty, matching the old fallback behavior.
		f2, err := mp4.DecodeFile(bytes.NewReader(bytes.Clone(wire)))
		if err != nil {
			t.Fatal(err)
		}
		if err := mp4.DecryptFragmentWithKeys(f2.Segments[0].Fragments[0], di, testKey(0x11), nil, false); err != nil {
			t.Fatalf("non-strict empty dictionary should use the legacy key: %v", err)
		}
	})

	// Subsample failures occur in a later sample and leave earlier samples intact.
	t.Run("subsample violations are rejected", func(t *testing.T) {
		// A subsample-flagged senc must protect every sample. Build a fixture
		// where one sample is clear in the seig group while the senc carries
		// the subsample flag for all samples.
		t.Run("clear sample with subsample flag", func(t *testing.T) {
			bad := cloneTracks(tracks)
			bad[0].specs[3] = sampleSpec{
				plain: distinctPlain(4, 24), protected: false, kid: testKID(0x31),
				flags: mp4.SyncSampleFlags, dur: 1024,
			}
			wire := buildRotationFragment(t, 12, bad, "cenc", true)
			f, err := mp4.DecodeFile(bytes.NewReader(wire))
			if err != nil {
				t.Fatal(err)
			}
			err = mp4.DecryptFragmentWithKeys(f.Segments[0].Fragments[0], di, nil, fullKeys, true)
			if err == nil || !strings.Contains(err.Error(), "subsample") {
				t.Fatalf("got %v, want an error about subsample-flagged clear samples", err)
			}
		})
		t.Run("subsample range beyond sample", func(t *testing.T) {
			bad := cloneTracks(tracks)
			// A subsample-flagged senc protects every sample, so turn the
			// fixture's clear samples into protected default-KID samples first.
			for ti := range bad {
				for si := range bad[ti].specs {
					sp := &bad[ti].specs[si]
					if !sp.protected {
						sp.protected = true
						sp.key = testKey(0x11)
						sp.kid = testKID(0x31)
						sp.ivSize = 8
						sp.iv = bytes.Repeat([]byte{0x55 + byte(si)}, 8)
					}
				}
			}
			bad[1].specs[3].subsamples = []mp4.SubSamplePattern{
				{BytesOfClearData: 2, BytesOfProtectedData: 1000},
			}
			wire := buildRotationFragment(t, 13, bad, "cenc", true)
			f, err := mp4.DecodeFile(bytes.NewReader(wire))
			if err != nil {
				t.Fatal(err)
			}
			frag := f.Segments[0].Fragments[0]
			err = mp4.DecryptFragmentWithKeys(frag, di, nil, fullKeys, true)
			if err == nil {
				t.Fatal("expected error for subsample range outside sample")
			}
			if !strings.Contains(err.Error(), "trackID=2") || !strings.Contains(err.Error(), "sample 4") {
				t.Fatalf("error %q must name track and sample", err.Error())
			}
			if payload := frag.Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire)) {
				t.Fatal("mdat payload was modified by the failed decryption")
			}
			if !frag.Moof.Trafs[0].Senc.ReadButNotParsed() {
				t.Fatal("senc parse state was not restored after failure")
			}
		})
	})

	// Malformed seig structures: all rejected before media is touched.
	t.Run("malformed seig groups", func(t *testing.T) {
		cases := []struct {
			name    string
			mutate  func(traf *mp4.TrafBox)
			wantErr string
		}{
			{
				name: "zero sample count",
				mutate: func(traf *mp4.TrafBox) {
					traf.Sbgp.SampleCounts[0] = 0
				},
				wantErr: "zero sample count",
			},
			{
				name: "track-level description index",
				mutate: func(traf *mp4.TrafBox) {
					traf.Sbgp.GroupDescriptionIndices[0] = 1
				},
				wantErr: "in-fragment index",
			},
			{
				name: "out of range description index",
				mutate: func(traf *mp4.TrafBox) {
					for i := range traf.Sbgp.GroupDescriptionIndices {
						traf.Sbgp.GroupDescriptionIndices[i] = 65536 + 1000
					}
				},
				wantErr: "missing entry",
			},
			{
				name: "unsupported sbgp version",
				mutate: func(traf *mp4.TrafBox) {
					traf.Sbgp.Version = 1
				},
				wantErr: "sbgp version 1 not supported",
			},
			{
				name: "unsupported sgpd version",
				mutate: func(traf *mp4.TrafBox) {
					traf.Sgpd.Version = 2
				},
				wantErr: "sgpd version 2 not supported",
			},
			{
				name: "duplicate seig sgpd",
				mutate: func(traf *mp4.TrafBox) {
					dup := *traf.Sgpd
					_ = traf.AddChild(&dup)
				},
				wantErr: "duplicate seig sgpd",
			},
		}
		for _, c := range cases {
			t.Run(c.name, func(t *testing.T) {
				wire := buildRotationFragment(t, 14, tracks, "cenc", false)
				f, err := mp4.DecodeFile(bytes.NewReader(wire))
				if err != nil {
					t.Fatal(err)
				}
				frag := f.Segments[0].Fragments[0]
				c.mutate(frag.Moof.Trafs[0])
				err = mp4.DecryptFragmentWithKeys(frag, di, nil, fullKeys, true)
				if err == nil || !strings.Contains(err.Error(), c.wantErr) {
					t.Fatalf("got %v, want error containing %q", err, c.wantErr)
				}
				if payload := frag.Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire)) {
					t.Fatal("mdat payload was modified by the failed decryption")
				}
			})
		}
	})

	// More group and auxiliary boundary errors, all rejected before media changes.
	t.Run("group and auxiliary boundary errors", func(t *testing.T) {
		cases := []struct {
			name    string
			mutate  func(frag *mp4.Fragment)
			wantErr string
		}{
			{
				name: "duplicate seig sbgp",
				mutate: func(frag *mp4.Fragment) {
					dup := *frag.Moof.Trafs[0].Sbgp
					_ = frag.Moof.Trafs[0].AddChild(&dup)
				},
				wantErr: "duplicate seig sbgp",
			},
			{
				name: "illegal IV size in seig entry",
				mutate: func(frag *mp4.Fragment) {
					entry := frag.Moof.Trafs[0].Sgpd.SampleGroupEntries[0].(*mp4.SeigSampleGroupEntry)
					entry.PerSampleIVSize = 7
				},
				wantErr: "illegal per-sample IV size 7",
			},
			{
				name: "sbgp count beyond sample count",
				mutate: func(frag *mp4.Fragment) {
					sbgp := frag.Moof.Trafs[0].Sbgp
					sbgp.SampleCounts[len(sbgp.SampleCounts)-1] += 100
				},
				wantErr: "but the traf only has",
			},
			{
				name: "saio offset not pointing at senc",
				mutate: func(frag *mp4.Fragment) {
					frag.Moof.Trafs[0].Saio.Offset[0] += 1
				},
				wantErr: "saio offset",
			},
			{
				name: "truncated senc via oversized sample count",
				mutate: func(frag *mp4.Fragment) {
					frag.Moof.Trafs[0].Senc.SampleCount += 2
				},
				wantErr: "senc sample_count",
			},
			{
				name: "saiz size does not match consumption",
				mutate: func(frag *mp4.Fragment) {
					saiz := frag.Moof.Trafs[0].Saiz
					saiz.DefaultSampleInfoSize = 0
					if len(saiz.SampleInfo) > 0 {
						saiz.SampleInfo[0] += 1
					}
				},
				wantErr: "saiz auxiliary size",
			},
		}
		for _, c := range cases {
			t.Run(c.name, func(t *testing.T) {
				wire := buildRotationFragment(t, 16, tracks, "cenc", false)
				f, err := mp4.DecodeFile(bytes.NewReader(wire))
				if err != nil {
					t.Fatal(err)
				}
				frag := f.Segments[0].Fragments[0]
				c.mutate(frag)
				err = mp4.DecryptFragmentWithKeys(frag, di, nil, fullKeys, true)
				if err == nil || !strings.Contains(err.Error(), c.wantErr) {
					t.Fatalf("got %v, want error containing %q", err, c.wantErr)
				}
				if payload := frag.Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire)) {
					t.Fatal("mdat payload was modified by the failed decryption")
				}
			})
		}

		// A hand-written senc box with one surplus trailing byte is rejected.
		t.Run("surplus auxiliary bytes", func(t *testing.T) {
			wire := buildRotationFragment(t, 17, tracks, "cenc", false)
			f, err := mp4.DecodeFile(bytes.NewReader(wire))
			if err != nil {
				t.Fatal(err)
			}
			frag := f.Segments[0].Fragments[0]
			traf := frag.Moof.Trafs[0]
			badRaw := rawSenc(tracks[0].specs, false)
			binary.BigEndian.PutUint32(badRaw[:4], uint32(len(badRaw)+1))
			badRaw = append(badRaw, 0xab)
			badSenc := decodeRawBox(t, badRaw).(*mp4.SencBox)
			for i, c := range traf.Children {
				if c.Type() == "senc" {
					traf.Children[i] = badSenc
				}
			}
			traf.Senc = badSenc
			// The extra byte shifts every later box by one, including the mdat
			// payload that the trun data offset points at.
			for _, trun := range traf.Truns {
				trun.DataOffset++
			}
			var re bytes.Buffer
			if err := frag.Encode(&re); err != nil {
				t.Fatal(err)
			}
			f2, err := mp4.DecodeFile(bytes.NewReader(re.Bytes()))
			if err != nil {
				t.Fatal(err)
			}
			err = mp4.DecryptFragmentWithKeys(f2.Segments[0].Fragments[0], di, nil, fullKeys, true)
			if err == nil || !strings.Contains(err.Error(), "auxiliary bytes left") {
				t.Fatalf("got %v, want an error about surplus auxiliary bytes", err)
			}
		})
	})

	// Segment boundary: a completed first fragment is not rolled back when the
	// second fragment fails, and the segment can then be retried successfully.
	t.Run("segment retry keeps completed fragment", func(t *testing.T) {
		// The first fragment only uses keys that the incomplete dictionary has,
		// so it completes. The second fragment introduces the missing kidC.
		tracksFirst := cloneTracks(tracks)
		kidA := testKID(0x31)
		keyA := testKey(0x11)
		tracksFirst[1].tenc.DefaultKID = kidA
		for si := range tracksFirst[1].specs {
			if tracksFirst[1].specs[si].protected {
				tracksFirst[1].specs[si].kid = kidA
				tracksFirst[1].specs[si].key = keyA
			}
		}
		wire1 := buildRotationFragment(t, 21, tracksFirst, "cenc", false)
		wire2 := buildRotationFragment(t, 22, tracks, "cenc", false)
		f, err := mp4.DecodeFile(bytes.NewReader(append(append([]byte{}, wire1...), wire2...)))
		if err != nil {
			t.Fatal(err)
		}
		seg := f.Segments[0]
		if len(seg.Fragments) != 2 {
			t.Fatalf("got %d fragments, want 2", len(seg.Fragments))
		}
		incomplete := map[string][]byte{}
		for k, v := range fullKeys {
			incomplete[k] = v
		}
		kidCHex := hex.EncodeToString(testKID(0x53))
		delete(incomplete, kidCHex)
		err = mp4.DecryptSegmentWithKeys(seg, di, nil, incomplete, true)
		if err == nil {
			t.Fatal("expected segment decryption to fail on the second fragment")
		}
		checkPlaintext(t, seg.Fragments[0], tracksFirst) // first fragment completed
		if payload := seg.Fragments[1].Mdat.Payload(); !bytes.Equal(payload, payloadOf(wire2)) {
			t.Fatal("second fragment mdat was modified by the failed call")
		}
		// The completed first fragment is not retried; supply the key and
		// decrypt only the still-encrypted second fragment on the same object.
		incomplete[kidCHex] = fullKeys[kidCHex]
		if err := mp4.DecryptFragmentWithKeys(seg.Fragments[1], di, nil, incomplete, true); err != nil {
			t.Fatalf("fragment retry: %v", err)
		}
		checkPlaintext(t, seg.Fragments[0], tracksFirst)
		checkPlaintext(t, seg.Fragments[1], tracks)
	})
}

// cloneTracks deep-copies the rotation tracks so a mutation does not affect
// other subtests sharing the fixture.
func cloneTracks(tracks []rotationTrack) []rotationTrack {
	out := make([]rotationTrack, len(tracks))
	for i, tr := range tracks {
		out[i] = tr
		out[i].trunAt = append([]int(nil), tr.trunAt...)
		specs := make([]sampleSpec, len(tr.specs))
		for j, sp := range tr.specs {
			sp.plain = bytes.Clone(sp.plain)
			sp.kid = bytes.Clone(sp.kid)
			sp.key = bytes.Clone(sp.key)
			sp.iv = bytes.Clone(sp.iv)
			if sp.subsamples != nil {
				sp.subsamples = append([]mp4.SubSamplePattern(nil), sp.subsamples...)
			}
			specs[j] = sp
		}
		out[i].specs = specs
	}
	return out
}

// payloadOf extracts the mdat payload of a one-fragment wire bytes.
func payloadOf(wire []byte) []byte {
	f, err := mp4.DecodeFile(bytes.NewReader(wire))
	if err != nil {
		panic(err)
	}
	return f.Segments[0].Fragments[0].Mdat.Payload()
}
