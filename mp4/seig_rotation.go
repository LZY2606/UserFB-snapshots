package mp4

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"fmt"

	"github.com/Eyevinn/mp4ff/bits"
)

// In-fragment sample group description indices start at sbgpInsideOffset+1
// (65537), so an index of 65536+n refers to the nth description of the sgpd
// box inside the same traf. Indices 1..65536 refer to track-level descriptions,
// which are not supported.

// sampleProtection carries the encryption parameters that effectively apply to
// one sample, resolved from the track's tenc defaults and the traf's seig
// sample group. The fields are copies, so changing a tenc or seig box after
// resolution does not change them.
type sampleProtection struct {
	isProtected     bool
	kid             UUID
	perSampleIVSize byte
	constantIV      []byte
	cryptByteBlock  byte
	skipByteBlock   byte
}

// seigRun is one run of consecutive samples, in decode order, covered by one
// seig sample group description.
type seigRun struct {
	startSample int // zero-based, inclusive
	endSample   int // zero-based, exclusive
	entry       *SeigSampleGroupEntry
}

// findSeigBoxes returns the seig sbgp and sgpd boxes among a traf's children.
// A traf may carry other sample groups alongside seig, so the single Sbgp and
// Sgpd pointers of the traf are not enough. More than one box of either kind is
// an error.
func findSeigBoxes(traf *TrafBox) (sbgpBox *SbgpBox, sgpdBox *SgpdBox, err error) {
	for _, c := range traf.Children {
		switch b := c.(type) {
		case *SbgpBox:
			if b.GroupingType != "seig" {
				continue
			}
			if sbgpBox != nil {
				return nil, nil, fmt.Errorf("duplicate seig sbgp box")
			}
			sbgpBox = b
		case *SgpdBox:
			if b.GroupingType != "seig" {
				continue
			}
			if sgpdBox != nil {
				return nil, nil, fmt.Errorf("duplicate seig sgpd box")
			}
			sgpdBox = b
		}
	}
	return sbgpBox, sgpdBox, nil
}

// trafSampleCount returns the total number of samples of a traf, counted over
// all its trun boxes in decode order.
func trafSampleCount(traf *TrafBox) int {
	nrSamples := 0
	for _, trun := range traf.Truns {
		nrSamples += int(trun.SampleCount())
	}
	return nrSamples
}

// resolveSeigRuns maps the seig sbgp/sgpd boxes of a traf to sample runs. The
// sbgp sample counts run continuously over all truns of the traf and are not
// reset between truns. hasSeig is false when the traf has no seig group.
// Only in-fragment descriptions (indices 65537 and above) are supported.
func resolveSeigRuns(traf *TrafBox, nrSamples int) (runs []seigRun, hasSeig bool, err error) {
	sbgpBox, sgpdBox, err := findSeigBoxes(traf)
	if err != nil {
		return nil, false, err
	}
	if sbgpBox == nil && sgpdBox == nil {
		return nil, false, nil
	}
	trackID := traf.Tfhd.TrackID
	if sbgpBox == nil || sgpdBox == nil {
		return nil, false, fmt.Errorf("trackID=%d: seig sample group needs both an sbgp and an sgpd box", trackID)
	}
	if sbgpBox.Version != 0 {
		return nil, false, fmt.Errorf("trackID=%d: sbgp version %d not supported, only version 0",
			trackID, sbgpBox.Version)
	}
	if sgpdBox.Version != 1 {
		return nil, false, fmt.Errorf("trackID=%d: sgpd version %d not supported, only version 1",
			trackID, sgpdBox.Version)
	}
	if len(sbgpBox.SampleCounts) != len(sbgpBox.GroupDescriptionIndices) {
		return nil, false, fmt.Errorf("trackID=%d: seig sbgp has %d sample counts but %d description indices",
			trackID, len(sbgpBox.SampleCounts), len(sbgpBox.GroupDescriptionIndices))
	}
	nrEntries := len(sgpdBox.SampleGroupEntries)
	sampleIdx := 0
	for entryNr, count := range sbgpBox.SampleCounts {
		if count == 0 {
			return nil, false, fmt.Errorf("trackID=%d: seig sbgp entry %d has a zero sample count",
				trackID, entryNr+1)
		}
		end := sampleIdx + int(count)
		if end > nrSamples {
			return nil, false, fmt.Errorf("trackID=%d: seig sbgp covers %d samples, but the traf only has %d samples",
				trackID, end, nrSamples)
		}
		gdi := sbgpBox.GroupDescriptionIndices[entryNr]
		switch {
		case gdi == 0:
			// Index 0 means "no group": these samples keep the tenc defaults.
		case gdi < seigFirstInsideIndex:
			return nil, false, fmt.Errorf("trackID=%d sample %d: seig description index %d is not an in-fragment index",
				trackID, sampleIdx+1, gdi)
		default:
			entryIdx := int(gdi - sbgpInsideOffset - 1)
			if entryIdx >= nrEntries {
				return nil, false, fmt.Errorf("trackID=%d sample %d: seig description index %d references missing entry %d of %d",
					trackID, sampleIdx+1, gdi, entryIdx+1, nrEntries)
			}
			entry, ok := sgpdBox.SampleGroupEntries[entryIdx].(*SeigSampleGroupEntry)
			if !ok {
				return nil, false, fmt.Errorf("trackID=%d sample %d: sgpd entry %d is not a seig entry",
					trackID, sampleIdx+1, entryIdx+1)
			}
			runs = append(runs, seigRun{startSample: sampleIdx, endSample: end, entry: entry})
		}
		sampleIdx = end
	}
	return runs, true, nil
}

// seigFirstInsideIndex is the smallest in-fragment sample group description
// index; 65537 refers to the first description of the traf's sgpd box.
const seigFirstInsideIndex = sbgpInsideOffset + 1

// tencProtection returns the default per-sample parameters that a tenc box gives.
func tencProtection(tenc *TencBox) sampleProtection {
	return sampleProtection{
		isProtected:     tenc.DefaultIsProtected != 0,
		kid:             bytes.Clone(tenc.DefaultKID),
		perSampleIVSize: tenc.DefaultPerSampleIVSize,
		constantIV:      bytes.Clone(tenc.DefaultConstantIV),
		cryptByteBlock:  tenc.DefaultCryptByteBlock,
		skipByteBlock:   tenc.DefaultSkipByteBlock,
	}
}

// resolveSampleProtection resolves the effective encryption parameters of every
// sample of a traf. Sample index 0 and any trailing samples that no sbgp entry
// covers keep the tenc defaults. The state is derived anew and never written
// back into the tenc or seig boxes.
func resolveSampleProtection(traf *TrafBox, tenc *TencBox) ([]sampleProtection, error) {
	nrSamples := trafSampleCount(traf)
	params := make([]sampleProtection, nrSamples)
	def := tencProtection(tenc)
	for i := range params {
		params[i] = def
	}
	runs, _, err := resolveSeigRuns(traf, nrSamples)
	if err != nil {
		return nil, err
	}
	for _, run := range runs {
		entry := run.entry
		p := sampleProtection{
			isProtected:     entry.IsProtected != 0,
			kid:             bytes.Clone(entry.KID),
			perSampleIVSize: entry.PerSampleIVSize,
			constantIV:      bytes.Clone(entry.ConstantIV),
			cryptByteBlock:  entry.CryptByteBlock,
			skipByteBlock:   entry.SkipByteBlock,
		}
		for i := run.startSample; i < run.endSample; i++ {
			params[i] = p
		}
	}
	for i, p := range params {
		if len(p.kid) != 16 {
			return nil, fmt.Errorf("trackID=%d sample %d: effective KID has length %d, want 16",
				traf.Tfhd.TrackID, i+1, len(p.kid))
		}
	}
	return params, nil
}

// seigIVSizes returns the effective per-sample IV size of every sample, using
// defaultIVSize for the samples that tenc defaults cover. A clear sample
// (IsProtected=0) never carries an IV, whatever its description's IV size says.
func seigIVSizes(traf *TrafBox, defaultIVSize byte) ([]byte, error) {
	nrSamples := trafSampleCount(traf)
	sizes := make([]byte, nrSamples)
	for i := range sizes {
		sizes[i] = defaultIVSize
	}
	runs, _, err := resolveSeigRuns(traf, nrSamples)
	if err != nil {
		return nil, err
	}
	for _, run := range runs {
		for i := run.startSample; i < run.endSample; i++ {
			if run.entry.IsProtected != 0 {
				sizes[i] = run.entry.PerSampleIVSize
			}
		}
	}
	return sizes, nil
}

// effectiveIVSize is the IV size a sample actually consumes: zero for clear
// samples, and the protection parameters' size otherwise.
func effectiveIVSize(p sampleProtection) byte {
	if !p.isProtected {
		return 0
	}
	return p.perSampleIVSize
}

// tencDefaultIVSize is the per-sample IV size that a tenc box gives to its
// default samples. A tenc whose default samples are not protected contributes
// no IV width.
func tencDefaultIVSize(tenc *TencBox) byte {
	if tenc == nil || tenc.DefaultIsProtected == 0 {
		return 0
	}
	return tenc.DefaultPerSampleIVSize
}

// sencBoxOf returns the senc box of a traf, whether it is a standalone box or a
// PIFF UUID box, together with whether one was present.
func sencBoxOf(traf *TrafBox) (*SencBox, bool) {
	switch {
	case traf.Senc != nil:
		return traf.Senc, true
	case traf.UUIDSenc != nil:
		return traf.UUIDSenc.Senc, true
	default:
		return nil, false
	}
}

// sencParseState snapshots everything that per-sample parsing changes, so a
// later failure restores the box to its pre-decryption state.
type sencParseState struct {
	senc             *SencBox
	readButNotParsed bool
	isParsedByGuess  bool
	perSampleIVSize  byte
	perSampleIVSizes []byte
	ivs              []InitializationVector
	subSamples       [][]SubSamplePattern
}

func snapshotSencState(senc *SencBox) sencParseState {
	return sencParseState{
		senc:             senc,
		readButNotParsed: senc.readButNotParsed,
		isParsedByGuess:  senc.isParsedByGuess,
		perSampleIVSize:  senc.perSampleIVSize,
		perSampleIVSizes: senc.perSampleIVSizes,
		ivs:              senc.IVs,
		subSamples:       senc.SubSamples,
	}
}

func (s sencParseState) restore() {
	s.senc.readButNotParsed = s.readButNotParsed
	s.senc.isParsedByGuess = s.isParsedByGuess
	s.senc.perSampleIVSize = s.perSampleIVSize
	s.senc.perSampleIVSizes = s.perSampleIVSizes
	s.senc.IVs = s.ivs
	s.senc.SubSamples = s.subSamples
}

// ParseReadBoxPerSample parses a deferred senc box where every sample may have
// its own IV size, as seig sample groups allow. A cenc box may therefore carry
// 8-byte and 16-byte IVs interleaved. Samples with an effective size of 0
// consume no IV. When the subsample flag is set, every sample record carries a
// subsample count and its entries; a count of 0 means full-sample handling.
func (s *SencBox) ParseReadBoxPerSample(ivSizes []byte) error {
	if !s.readButNotParsed && !s.isParsedByGuess {
		return fmt.Errorf("senc box already parsed")
	}
	if int(s.SampleCount) != len(ivSizes) {
		return fmt.Errorf("senc sample_count %d differs from %d traf samples", s.SampleCount, len(ivSizes))
	}
	sr := bits.NewFixedSliceReader(s.rawData)
	useSubSamples := s.Flags&UseSubSampleEncryption != 0
	ivs := make([]InitializationVector, 0, s.SampleCount)
	var subSamples [][]SubSamplePattern
	if useSubSamples {
		subSamples = make([][]SubSamplePattern, s.SampleCount)
	}
	uniformSize := byte(0)
	if s.SampleCount > 0 {
		uniformSize = ivSizes[0]
	}
	for i := uint32(0); i < s.SampleCount; i++ {
		ivSize := ivSizes[i]
		switch ivSize {
		case 0, 8, 16:
		default:
			return fmt.Errorf("sample %d: illegal per-sample IV size %d", i+1, ivSize)
		}
		if ivSize != uniformSize {
			uniformSize = 0 // mixed IV widths
		}
		if ivSize > 0 {
			if sr.NrRemainingBytes() < int(ivSize) {
				return fmt.Errorf("sample %d: truncated auxiliary information, want %d IV bytes", i+1, ivSize)
			}
			ivs = append(ivs, sr.ReadBytes(int(ivSize)))
		}
		if useSubSamples {
			if sr.NrRemainingBytes() < 2 {
				return fmt.Errorf("sample %d: truncated subsample count", i+1)
			}
			subsampleCount := int(sr.ReadUint16())
			if sr.NrRemainingBytes() < subsampleCount*6 {
				return fmt.Errorf("sample %d: truncated subsample entries", i+1)
			}
			entries := make([]SubSamplePattern, subsampleCount)
			for j := range entries {
				entries[j] = SubSamplePattern{
					BytesOfClearData:     sr.ReadUint16(),
					BytesOfProtectedData: sr.ReadUint32(),
				}
			}
			subSamples[i] = entries
		}
	}
	if sr.NrRemainingBytes() != 0 {
		return fmt.Errorf("%d auxiliary bytes left after all %d samples", sr.NrRemainingBytes(), s.SampleCount)
	}
	s.IVs = ivs
	s.SubSamples = subSamples
	s.perSampleIVSize = uniformSize
	// A per-sample table is needed only while at least two different IV widths
	// (including 0 for samples without an IV) occur. Otherwise the box behaves
	// exactly like one parsed with the uniform ParseReadBox path.
	mixedWidths := false
	for i := 1; i < len(ivSizes); i++ {
		if ivSizes[i] != ivSizes[0] {
			mixedWidths = true
			break
		}
	}
	if mixedWidths {
		s.perSampleIVSizes = bytes.Clone(ivSizes)
	} else {
		s.perSampleIVSizes = nil
	}
	s.readButNotParsed = false
	s.isParsedByGuess = false
	return nil
}

// sampleIV returns the IV of sample i of a parsed senc box. The IVs of samples
// without one (constant-IV or clear samples) are not stored, so the stored IVs
// are indexed by their own order, not by the sample index.
func (s *SencBox) sampleIV(i int) ([]byte, error) {
	if s.perSampleIVSizes == nil {
		// Uniform box: every sample either has an IV or none has one.
		if i >= len(s.IVs) {
			return nil, nil
		}
		return s.IVs[i], nil
	}
	ivIdx := 0
	for j := 0; j < i; j++ {
		if s.perSampleIVSizes[j] > 0 {
			ivIdx++
		}
	}
	if s.perSampleIVSizes[i] == 0 {
		return nil, nil
	}
	if ivIdx >= len(s.IVs) {
		return nil, fmt.Errorf("sample %d: senc IV missing", i+1)
	}
	return s.IVs[ivIdx], nil
}

// parsedSencView gives the IV and subsamples of one sample from an already
// parsed senc box, checking them against the sample's effective IV size.
func parsedSencView(senc *SencBox, i int, p sampleProtection) (iv []byte, subSamples []SubSamplePattern, err error) {
	if i < 0 || i >= int(senc.SampleCount) {
		return nil, nil, fmt.Errorf("sample %d outside senc sample_count %d", i+1, senc.SampleCount)
	}
	if effectiveIVSize(p) > 0 {
		iv, err = senc.sampleIV(i)
		if err != nil {
			return nil, nil, err
		}
		if p.isProtected && len(iv) != int(p.perSampleIVSize) {
			return nil, nil, fmt.Errorf("sample %d: senc IV length %d does not match effective IV size %d",
				i+1, len(iv), p.perSampleIVSize)
		}
	}
	if senc.Flags&UseSubSampleEncryption != 0 {
		if i >= len(senc.SubSamples) {
			return nil, nil, fmt.Errorf("sample %d: senc subsample entry missing", i+1)
		}
		subSamples = senc.SubSamples[i]
	}
	return iv, subSamples, nil
}

// validateSaiz checks that every saiz entry, when the box is present, equals
// the auxiliary bytes that the sample actually consumes. A uniform box carries
// one size for every sample; otherwise the per-sample sizes line up with the
// samples, where zero sizes, as SaizBox.AddSampleInfo writes them, have no
// entry and line up with the samples that consume no bytes.
func validateSaiz(saiz *SaizBox, nrSamples int, consumed []uint32) error {
	if saiz == nil {
		return nil
	}
	if saiz.DefaultSampleInfoSize != 0 {
		if int(saiz.SampleCount) != nrSamples {
			return fmt.Errorf("saiz sample_count %d differs from %d traf samples", saiz.SampleCount, nrSamples)
		}
		for i := 0; i < nrSamples; i++ {
			if consumed[i] != saiz.DefaultSampleInfoSize {
				return fmt.Errorf("sample %d: saiz auxiliary size %d does not match %d consumed bytes",
					i+1, saiz.DefaultSampleInfoSize, consumed[i])
			}
		}
		return nil
	}
	entryIdx := 0
	for i := 0; i < nrSamples; i++ {
		if consumed[i] == 0 {
			continue // zero sizes carry no saiz entry, as AddSampleInfo writes them
		}
		if entryIdx >= len(saiz.SampleInfo) {
			return fmt.Errorf("saiz has no size for sample %d of %d", i+1, nrSamples)
		}
		if saiz.SampleInfo[entryIdx] != consumed[i] {
			return fmt.Errorf("sample %d: saiz auxiliary size %d does not match %d consumed bytes",
				i+1, saiz.SampleInfo[entryIdx], consumed[i])
		}
		entryIdx++
	}
	if entryIdx != len(saiz.SampleInfo) {
		return fmt.Errorf("saiz has %d extra auxiliary size entries", len(saiz.SampleInfo)-entryIdx)
	}
	return nil
}

// validateSaio checks that the single saio offset points at the inline senc
// auxiliary payload. External auxiliary information is not supported.
func validateSaio(saio *SaioBox, senc *SencBox, moofStartPos uint64) error {
	if saio == nil {
		return nil
	}
	if len(saio.Offset) != 1 {
		return fmt.Errorf("saio has %d offsets, only one inline offset is supported", len(saio.Offset))
	}
	posFromSaio := saio.Offset[0] + int64(moofStartPos)
	if uint64(posFromSaio) != senc.StartPos+16 {
		return fmt.Errorf("saio offset %d does not point at inline senc payload at %d",
			posFromSaio, senc.StartPos+16)
	}
	return nil
}

// sampleDecryptEntry is everything one sample needs to be decrypted, resolved
// before any media byte is touched.
type sampleDecryptEntry struct {
	sampleIdx  int
	protected  bool
	block      cipher.Block
	iv         []byte // 16-byte CTR counter or CBC IV, nil for clear samples
	subSamples []SubSamplePattern
	params     sampleProtection
}

// trafDecryptPlan is the fully checked decryption work of one traf.
type trafDecryptPlan struct {
	traf      *TrafBox
	scheme    string
	samples   []FullSample
	entries   []sampleDecryptEntry
	senc      *SencBox
	snapshots []sencParseState
}

// getSampleKey resolves the AES key of one sample from its effective KID. In
// strict mode every protected sample must have a dictionary entry, so an empty
// dictionary never falls back to the legacy key. Outside strict mode the
// legacy-key fallback is kept, including for an empty dictionary.
func getSampleKey(kid UUID, key []byte, keysByKID map[string][]byte, strictKIDMode bool) ([]byte, error) {
	if len(kid) != 16 {
		return nil, fmt.Errorf("effective KID has length %d, want 16", len(kid))
	}
	kidHex := hex.EncodeToString(kid)
	if len(keysByKID) == 0 {
		if strictKIDMode {
			return nil, fmt.Errorf("empty key dictionary in strict mode for kid=%s", kidHex)
		}
		return key, nil
	}
	if mappedKey, ok := keysByKID[kidHex]; ok {
		return mappedKey, nil
	}
	if strictKIDMode {
		return nil, fmt.Errorf("requested key was not found for kid=%s", kidHex)
	}
	return key, nil
}

// buildTrafDecryptPlan resolves and checks everything for one traf without
// changing media data or the box tree: sample groups, senc consumption,
// auxiliary boundaries, required keys and subsample ranges. On error it still
// returns the partial plan, whose snapshots restore any senc box that parsing
// already changed, so the caller can roll back through plans of failed trafs.
func buildTrafDecryptPlan(frag *Fragment, traf *TrafBox, ti DecryptTrackInfo, scheme string,
	key []byte, keysByKID map[string][]byte, strictKIDMode bool) (plan *trafDecryptPlan, err error) {
	tenc := ti.Sinf.Schi.Tenc
	trackID := traf.Tfhd.TrackID
	plan = &trafDecryptPlan{traf: traf, scheme: scheme}
	params, err := resolveSampleProtection(traf, tenc)
	if err != nil {
		return plan, err
	}
	samples, err := frag.GetFullSamples(ti.Trex)
	if err != nil {
		return plan, err
	}
	if len(samples) != len(params) {
		return plan, fmt.Errorf("trackID=%d: %d trun samples but %d protection entries",
			trackID, len(samples), len(params))
	}
	plan.samples = samples

	senc, hasSenc := sencBoxOf(traf)
	plan.senc = senc
	useSubSamples := false
	if hasSenc {
		if int(senc.SampleCount) != len(samples) {
			return plan, fmt.Errorf("trackID=%d: senc sample_count %d differs from %d traf samples",
				trackID, senc.SampleCount, len(samples))
		}
		useSubSamples = senc.Flags&UseSubSampleEncryption != 0
		if useSubSamples {
			for i, p := range params {
				if !p.isProtected {
					return plan, fmt.Errorf("trackID=%d sample %d: senc carries subsample information, so every sample must be protected",
						trackID, i+1)
				}
			}
		}
		if senc.readButNotParsed || senc.isParsedByGuess {
			plan.snapshots = append(plan.snapshots, snapshotSencState(senc))
			ivSizes := make([]byte, len(params))
			for i, p := range params {
				ivSizes[i] = effectiveIVSize(p)
			}
			if err := senc.ParseReadBoxPerSample(ivSizes); err != nil {
				return plan, fmt.Errorf("trackID=%d: parse senc: %w", trackID, err)
			}
		}
	} else {
		if traf.Saiz != nil || traf.Saio != nil {
			return plan, fmt.Errorf("trackID=%d: saiz/saio present without a senc box", trackID)
		}
		// A missing senc is only acceptable while every protected sample uses
		// a constant IV and there is no subsample information.
		for i, p := range params {
			if p.isProtected && p.perSampleIVSize != 0 {
				return plan, fmt.Errorf("trackID=%d sample %d: no senc box for per-sample IV", trackID, i+1)
			}
		}
	}

	consumed := make([]uint32, len(samples))
	entries := make([]sampleDecryptEntry, len(samples))
	blocks := make(map[string]cipher.Block)
	for i, p := range params {
		if hasSenc {
			consumed[i] = uint32(effectiveIVSize(p))
		}
		if !p.isProtected {
			if hasSenc && useSubSamples {
				return plan, fmt.Errorf("trackID=%d sample %d: clear sample in subsample-encrypted senc", trackID, i+1)
			}
			continue
		}
		var sampleIV []byte
		var subSamples []SubSamplePattern
		if hasSenc {
			sampleIV, subSamples, err = parsedSencView(senc, i, p)
			if err != nil {
				return plan, fmt.Errorf("trackID=%d: %w", trackID, err)
			}
			if useSubSamples {
				consumed[i] += 2 + uint32(len(subSamples))*6
			}
		}
		var iv16 []byte
		switch scheme {
		case "cenc":
			switch p.perSampleIVSize {
			case 8:
				if len(sampleIV) != 8 {
					return plan, fmt.Errorf("trackID=%d sample %d: want 8-byte cenc IV, got %d bytes",
						trackID, i+1, len(sampleIV))
				}
				iv16 = make([]byte, 16) // The 8-byte IV is the high half of the counter
				copy(iv16, sampleIV)
			case 16:
				if len(sampleIV) != 16 {
					return plan, fmt.Errorf("trackID=%d sample %d: want 16-byte cenc IV, got %d bytes",
						trackID, i+1, len(sampleIV))
				}
				iv16 = bytes.Clone(sampleIV)
			default:
				return plan, fmt.Errorf("trackID=%d sample %d: illegal cenc IV size %d",
					trackID, i+1, p.perSampleIVSize)
			}
		case "cbcs":
			switch {
			case p.perSampleIVSize == 16:
				if len(sampleIV) != 16 {
					return plan, fmt.Errorf("trackID=%d sample %d: want 16-byte cbcs IV, got %d bytes",
						trackID, i+1, len(sampleIV))
				}
				iv16 = bytes.Clone(sampleIV)
			case p.perSampleIVSize == 0:
				if len(p.constantIV) != 16 {
					return plan, fmt.Errorf("trackID=%d sample %d: cbcs constant IV has length %d, want 16",
						trackID, i+1, len(p.constantIV))
				}
				iv16 = bytes.Clone(p.constantIV)
			default:
				return plan, fmt.Errorf("trackID=%d sample %d: illegal cbcs IV size %d",
					trackID, i+1, p.perSampleIVSize)
			}
		}
		sampleKey, err := getSampleKey(p.kid, key, keysByKID, strictKIDMode)
		if err != nil {
			return plan, fmt.Errorf("trackID=%d: %w", trackID, err)
		}
		if len(sampleKey) != 16 {
			return plan, fmt.Errorf("trackID=%d sample %d: AES key has length %d, want 16",
				trackID, i+1, len(sampleKey))
		}
		block, ok := blocks[string(sampleKey)]
		if !ok {
			block, err = aes.NewCipher(sampleKey)
			if err != nil {
				return plan, fmt.Errorf("trackID=%d sample %d: %w", trackID, i+1, err)
			}
			blocks[string(sampleKey)] = block
		}
		// Check the subsample ranges against the actual sample now, so no
		// media byte is touched before every check has passed.
		var pos uint32
		for j, ss := range subSamples {
			_, end, err := subSampleRange(len(samples[i].Data), pos, j, ss)
			if err != nil {
				return plan, fmt.Errorf("trackID=%d sample %d: %w", trackID, i+1, err)
			}
			pos = end
		}
		entries[i] = sampleDecryptEntry{
			sampleIdx:  i,
			protected:  true,
			block:      block,
			iv:         iv16,
			subSamples: subSamples,
			params:     p,
		}
	}
	if hasSenc {
		if err := validateSaiz(traf.Saiz, len(samples), consumed); err != nil {
			return plan, fmt.Errorf("trackID=%d: %w", trackID, err)
		}
		if err := validateSaio(traf.Saio, senc, frag.Moof.StartPos); err != nil {
			return plan, fmt.Errorf("trackID=%d: %w", trackID, err)
		}
	}
	plan.entries = entries
	return plan, nil
}

// restoreSencParsing puts every senc box that plan building parsed back into
// its pre-call state.
func restoreSencParsing(plans []*trafDecryptPlan) {
	for _, p := range plans {
		for _, snap := range p.snapshots {
			snap.restore()
		}
	}
}

// restoreMdatPayload copies a payload snapshot back into the mdat data parts in
// place, preserving the DataParts/Data structure. Decryption never changes the
// payload length, so backup has exactly the current payload size.
func restoreMdatPayload(mdat *MdatBox, backup []byte) {
	if mdat == nil {
		return
	}
	pos := 0
	for _, part := range mdat.DataParts {
		copy(part, backup[pos:pos+len(part)])
		pos += len(part)
	}
	if len(mdat.Data) > 0 {
		copy(mdat.Data, backup[pos:pos+len(mdat.Data)])
	}
}

// cryptEntry decrypts (dirDec) or re-encrypts (dirEnc) one sample in place.
func cryptEntry(scheme string, dir cryptoDir, entry *sampleDecryptEntry, data []byte) error {
	if !entry.protected {
		return nil
	}
	switch scheme {
	case "cenc":
		return cryptSampleCencBlock(entry.block, data, entry.iv, entry.subSamples)
	case "cbcs":
		tenc := &TencBox{
			DefaultCryptByteBlock: entry.params.cryptByteBlock,
			DefaultSkipByteBlock:  entry.params.skipByteBlock,
		}
		return cryptSampleCbcs(dir, data, entry.block, entry.iv, entry.subSamples, tenc)
	default:
		return fmt.Errorf("scheme type %s not supported", scheme)
	}
}

// applyTrafPlan decrypts the protected samples of one traf in place. The
// caller snapshots the whole mdat payload first and restores it if this returns
// an error, so any partially decrypted samples are rolled back there.
func applyTrafPlan(plan *trafDecryptPlan) error {
	for i := range plan.entries {
		entry := &plan.entries[i]
		if !entry.protected {
			continue
		}
		if err := cryptEntry(plan.scheme, dirDec, entry, plan.samples[entry.sampleIdx].Data); err != nil {
			return err
		}
	}
	return nil
}
