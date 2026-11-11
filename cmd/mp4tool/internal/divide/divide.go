package divide

import (
	"fmt"
	"io"
	"math"
	"os"
	"path"
	"sort"

	"github.com/abema/go-mp4"
)

const (
	videoDirName     = "video"
	audioDirName     = "audio"
	initMP4FileName  = "init.mp4"
	playlistFileName = "playlist.m3u8"
)

func segmentFileName(i int) string {
	return fmt.Sprintf("%d.mp4", i)
}

func Main(args []string) int {
	if len(args) < 2 {
		println("USAGE: mp4tool divide INPUT.mp4 OUTPUT_DIR")
		return 1
	}

	if err := divide(args[0], args[1]); err != nil {
		fmt.Println("Error:", err)
		return 1
	}
	return 0
}

type childInfo map[uint32]uint64

type segment struct {
	duration float64
}

type trackType int

const (
	trackVideo trackType = iota
	trackAudio
)

type track struct {
	id         uint32
	trackType  trackType
	timescale  uint32
	bandwidth  uint64
	height     uint16
	width      uint16
	avcProfile uint8
	avcCompat  uint8
	avcLevel   uint8
	segments   []segment
	outputDir  string
	initFile   *os.File

	// segmentFile is the file receiving the current (most recent) fragment.
	segmentFile *os.File
}

// dirName returns the output directory name. When the input contains more
// than one video track, video directories are disambiguated by trackID so
// that two tracks never open the same file.
func (t *track) dirName(multiVideo bool) string {
	if t.trackType == trackVideo && multiVideo {
		return fmt.Sprintf("%s_%d", videoDirName, t.id)
	}
	if t.trackType == trackAudio {
		return audioDirName
	}
	return videoDirName
}

func (t *track) codecString() string {
	if t.trackType == trackAudio {
		// AAC-LC, the only audio codec supported by this command.
		return "mp4a.40.2"
	}
	return fmt.Sprintf("avc1.%02X%02X%02X", t.avcProfile, t.avcCompat, t.avcLevel)
}

func divide(inputFilePath, outputDir string) error {
	inputFile, err := os.Open(inputFilePath)
	if err != nil {
		return err
	}
	defer inputFile.Close()

	// Build the track map from the initialization boxes. Every later decision
	// (which file a fragment is written to, which playlist it belongs to) is
	// keyed by the trackID declared inside tkhd/tfhd, never by trak or moof
	// ordering.
	tracks, err := discoverTracks(inputFile)
	if err != nil {
		return err
	}
	if len(tracks) == 0 {
		return fmt.Errorf("no supported track found: only unencrypted avc1 video and a single mp4a audio track are supported")
	}

	multiVideo := countTrackType(tracks, trackVideo) > 1

	// Validation that must not create or overwrite any output has to run
	// before the first MkdirAll/Create below.
	if err := checkDuplicateTrackID(tracks); err != nil {
		return err
	}
	if n := countTrackType(tracks, trackAudio); n > 1 {
		return fmt.Errorf("only a single mp4a audio track is supported: audio tracks=%d", n)
	}

	for _, t := range tracks {
		t.outputDir = path.Join(outputDir, t.dirName(multiVideo))
	}

	// Everything below mutates the output directory; validation that must not
	// touch existing outputs has to happen before this point.
	for _, t := range tracks {
		if err := os.MkdirAll(t.outputDir, 0777); err != nil {
			return err
		}
		if t.initFile, err = os.Create(path.Join(t.outputDir, initMP4FileName)); err != nil {
			return err
		}
		if t.segmentFile, err = os.Create(path.Join(t.outputDir, segmentFileName(0))); err != nil {
			return err
		}
	}
	defer func() {
		for _, t := range tracks {
			if t.initFile != nil {
				t.initFile.Close()
			}
			if t.segmentFile != nil {
				t.segmentFile.Close()
			}
		}
	}()

	trackByID := make(map[uint32]*track, len(tracks))
	for _, t := range tracks {
		trackByID[t.id] = t
	}

	// pendingTrackID identifies the moof that must be immediately followed by
	// an mdat. pendingSkip is set when the moof belongs to an unsupported
	// track whose following mdat must be consumed but discarded.
	var pendingTrackID uint32
	var pending, pendingSkip bool

	if _, err = mp4.ReadBoxStructure(inputFile, func(h *mp4.ReadHandle) (interface{}, error) {
		// The mvex box is shared but contains one trex per track. Expand it
		// and keep only each track's own trex, instead of copying the whole
		// mvex payload (which would include foreign trex boxes) verbatim.
		if h.BoxInfo.Type == mp4.BoxTypeMvex() {
			return writeFilteredContainer(h, tracks)
		}

		// initialization segment
		if h.BoxInfo.Type == mp4.BoxTypeMoov() ||
			h.BoxInfo.Type == mp4.BoxTypeFtyp() ||
			h.BoxInfo.Type == mp4.BoxTypePssh() ||
			h.BoxInfo.Type == mp4.BoxTypeMvhd() ||
			h.BoxInfo.Type == mp4.BoxTypeMehd() ||
			h.BoxInfo.Type == mp4.BoxTypeTrak() ||
			h.BoxInfo.Type == mp4.BoxTypeUdta() ||
			h.BoxInfo.Type == mp4.BoxTypeSidx() ||
			h.BoxInfo.Type == mp4.BoxTypeTrex() ||
			h.BoxInfo.Type == mp4.BoxTypeTrep() {

			var writeAll bool
			var trackID uint32
			switch h.BoxInfo.Type {
			case mp4.BoxTypeTrak():
				// get trackID from Tkhd box
				bs, err := mp4.ExtractBoxWithPayload(inputFile, &h.BoxInfo, mp4.BoxPath{mp4.BoxTypeTkhd()})
				if err != nil {
					return nil, err
				} else if len(bs) != 1 {
					return nil, fmt.Errorf("trak box must have one tkhd box: tkhd=%d", len(bs))
				}
				tkhd := bs[0].Payload.(*mp4.Tkhd)
				trackID = tkhd.TrackID
			case mp4.BoxTypeTrex():
				// reached while expanding mvex; keep only the owning track
				trexID, err := readTrexTrackID(inputFile, &h.BoxInfo)
				if err != nil {
					return nil, err
				}
				trackID = trexID
			case mp4.BoxTypeTrep():
				trepID, err := readTrepTrackID(inputFile, &h.BoxInfo)
				if err != nil {
					return nil, err
				}
				trackID = trepID
			default:
				writeAll = true
			}

			offsetMap := make(map[uint32]int64, len(tracks))
			biMap := make(map[uint32]*mp4.BoxInfo, len(tracks))
			for _, t := range tracks {
				if writeAll || t.id == trackID {
					if offsetMap[t.id], err = t.initFile.Seek(0, io.SeekEnd); err != nil {
						return nil, err
					}
					if biMap[t.id], err = mp4.WriteBoxInfo(t.initFile, &h.BoxInfo); err != nil {
						return nil, err
					}
					biMap[t.id].Size = biMap[t.id].HeaderSize
				}
			}

			if h.BoxInfo.Type == mp4.BoxTypeMoov() {
				vals, err := h.Expand()
				if err != nil {
					return nil, err
				}
				for _, val := range vals {
					ci := val.(childInfo)
					for _, t := range tracks {
						// already writeAll is true in Moov box
						biMap[t.id].Size += ci[t.id]
					}
				}
			} else {
				// copy all data of payload
				for _, t := range tracks {
					if writeAll || t.id == trackID {
						n, err := h.ReadData(t.initFile)
						if err != nil {
							return nil, err
						}
						biMap[t.id].Size += uint64(n)
					}
				}
			}

			// rewrite headers
			for _, t := range tracks {
				if writeAll || t.id == trackID {
					if _, err = t.initFile.Seek(offsetMap[t.id], io.SeekStart); err != nil {
						return nil, err
					}
					if biMap[t.id], err = mp4.WriteBoxInfo(t.initFile, biMap[t.id]); err != nil {
						return nil, err
					}
				}
			}

			ci := make(childInfo, len(biMap))
			for id, bi := range biMap {
				ci[id] = bi.Size
			}
			return ci, nil
		}

		// media segment: every moof must contain exactly one traf and be
		// immediately followed by one mdat.
		if h.BoxInfo.Type == mp4.BoxTypeMoof() {
			if pending || pendingSkip {
				return nil, fmt.Errorf("moof box must be immediately followed by mdat box")
			}

			trafs, err := mp4.ExtractBox(inputFile, &h.BoxInfo, mp4.BoxPath{mp4.BoxTypeTraf()})
			if err != nil {
				return nil, err
			}
			if len(trafs) != 1 {
				return nil, fmt.Errorf("each moof box must contain exactly one traf box: traf=%d", len(trafs))
			}

			bs, err := mp4.ExtractBoxWithPayload(inputFile, trafs[0], mp4.BoxPath{mp4.BoxTypeTfhd()})
			if err != nil {
				return nil, err
			} else if len(bs) != 1 {
				return nil, fmt.Errorf("traf box must have one tfhd box: tfhd=%d", len(bs))
			}
			tfhd := bs[0].Payload.(*mp4.Tfhd)
			currTrackID := tfhd.TrackID

			t, supported := trackByID[currTrackID]
			if !supported {
				// consume and discard this fragment and its trailing mdat
				pendingSkip = true
				return nil, nil
			}

			var defaultSampleDuration uint32
			if tfhd.CheckFlag(0x000008) {
				defaultSampleDuration = tfhd.DefaultSampleDuration
			}

			// extract Trun-box
			bs, err = mp4.ExtractBoxWithPayload(inputFile, trafs[0], mp4.BoxPath{mp4.BoxTypeTrun()})
			if err != nil {
				return nil, err
			}
			if len(bs) == 0 {
				return nil, fmt.Errorf("traf box must have one trun box: trun=0")
			}
			trun := bs[0].Payload.(*mp4.Trun)

			var duration uint32
			for i := range trun.Entries {
				if trun.CheckFlag(0x000100) {
					duration += trun.Entries[i].SampleDuration
				} else {
					duration += defaultSampleDuration
				}
			}

			// close last segment file and create next. Segment numbers are
			// per-track and contiguous from zero.
			if err := t.segmentFile.Close(); err != nil {
				return nil, err
			}
			if t.segmentFile, err = os.Create(path.Join(t.outputDir, segmentFileName(len(t.segments)))); err != nil {
				return nil, err
			}
			t.segments = append(t.segments, segment{
				duration: float64(duration) / float64(t.timescale),
			})

			if err := copyBoxToFile(h, t.segmentFile); err != nil {
				return nil, err
			}

			pendingTrackID = currTrackID
			pending = true
			return nil, nil
		}

		if h.BoxInfo.Type == mp4.BoxTypeMdat() {
			if !pending && !pendingSkip {
				return nil, fmt.Errorf("mdat box must be immediately preceded by moof box")
			}
			if pendingSkip {
				pendingSkip = false
				return nil, nil
			}

			t := trackByID[pendingTrackID]
			// Bandwidth is derived only from this track's own fragment size
			// and duration.
			last := t.segments[len(t.segments)-1]
			if last.duration > 0 {
				bandwidth := uint64(float64(h.BoxInfo.Size) * 8 / last.duration)
				if bandwidth > t.bandwidth {
					t.bandwidth = bandwidth
				}
			}
			if err := copyBoxToFile(h, t.segmentFile); err != nil {
				return nil, err
			}

			pending = false
			pendingTrackID = 0
			return nil, nil
		}

		// skip the movie fragment random access box
		if h.BoxInfo.Type == mp4.BoxTypeMfra() {
			return nil, nil
		}

		return nil, fmt.Errorf("unexpected box type: %s", h.BoxInfo.Type.String())
	}); err != nil {
		return err
	}

	if pending || pendingSkip {
		return fmt.Errorf("moof box must be immediately followed by mdat box")
	}

	if err := outputMasterPlaylist(path.Join(outputDir, playlistFileName), tracks); err != nil {
		return err
	}

	for _, t := range tracks {
		if err := outputMediaPlaylist(path.Join(t.outputDir, playlistFileName), t.segments); err != nil {
			return err
		}
	}

	return nil
}

func copyBoxToFile(h *mp4.ReadHandle, w io.WriteSeeker) error {
	if _, err := mp4.WriteBoxInfo(w, &h.BoxInfo); err != nil {
		return err
	}
	if _, err := h.ReadData(w); err != nil {
		return err
	}
	return nil
}

// writeFilteredContainer writes the current container box (used for mvex) to
// every track's init file while its children are filtered by trackID through
// the normal handler. The container size is rewritten to account for the
// children that were actually kept, and a per-track childInfo is returned so
// the enclosing moov size stays correct.
func writeFilteredContainer(h *mp4.ReadHandle, tracks []*track) (interface{}, error) {
	offsetMap := make(map[uint32]int64, len(tracks))
	biMap := make(map[uint32]*mp4.BoxInfo, len(tracks))
	for _, t := range tracks {
		var err error
		if offsetMap[t.id], err = t.initFile.Seek(0, io.SeekEnd); err != nil {
			return nil, err
		}
		if biMap[t.id], err = mp4.WriteBoxInfo(t.initFile, &h.BoxInfo); err != nil {
			return nil, err
		}
		biMap[t.id].Size = biMap[t.id].HeaderSize
	}

	vals, err := h.Expand()
	if err != nil {
		return nil, err
	}
	for _, val := range vals {
		ci := val.(childInfo)
		for _, t := range tracks {
			biMap[t.id].Size += ci[t.id]
		}
	}

	for _, t := range tracks {
		if _, err := t.initFile.Seek(offsetMap[t.id], io.SeekStart); err != nil {
			return nil, err
		}
		var err error
		if biMap[t.id], err = mp4.WriteBoxInfo(t.initFile, biMap[t.id]); err != nil {
			return nil, err
		}
	}

	ci := make(childInfo, len(biMap))
	for id, bi := range biMap {
		ci[id] = bi.Size
	}
	return ci, nil
}

func readTrexTrackID(r io.ReadSeeker, bi *mp4.BoxInfo) (uint32, error) {
	if _, err := bi.SeekToPayload(r); err != nil {
		return 0, err
	}
	trex := &mp4.Trex{}
	if _, err := mp4.Unmarshal(r, bi.Size-bi.HeaderSize, trex, bi.Context); err != nil {
		return 0, err
	}
	return trex.TrackID, nil
}

func readTrepTrackID(r io.ReadSeeker, bi *mp4.BoxInfo) (uint32, error) {
	if _, err := bi.SeekToPayload(r); err != nil {
		return 0, err
	}
	trep := &mp4.Trep{}
	if _, err := mp4.Unmarshal(r, bi.Size-bi.HeaderSize, trep, bi.Context); err != nil {
		return 0, err
	}
	return trep.TrackID, nil
}

func discoverTracks(inputFile *os.File) ([]*track, error) {
	bis, err := mp4.ExtractBox(inputFile, nil, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeTrak()})
	if err != nil {
		return nil, err
	}

	tracks := make([]*track, 0, len(bis))
	for _, bi := range bis {
		t := new(track)

		// get trackID from Tkhd box
		bs, err := mp4.ExtractBoxWithPayload(inputFile, bi, mp4.BoxPath{mp4.BoxTypeTkhd()})
		if err != nil {
			return nil, err
		} else if len(bs) != 1 {
			return nil, fmt.Errorf("trak box must have one tkhd box: tkhd=%d", len(bs))
		}
		tkhd := bs[0].Payload.(*mp4.Tkhd)
		t.id = tkhd.TrackID

		// get timescale from Mdhd box
		bs, err = mp4.ExtractBoxWithPayload(inputFile, bi, mp4.BoxPath{mp4.BoxTypeMdia(), mp4.BoxTypeMdhd()})
		if err != nil {
			return nil, err
		} else if len(bs) != 1 {
			return nil, fmt.Errorf("trak box must have one mdhd box: mdhd=%d", len(bs))
		}
		mdhd := bs[0].Payload.(*mp4.Mdhd)
		t.timescale = mdhd.Timescale

		bs, err = mp4.ExtractBoxWithPayload(inputFile, bi, mp4.BoxPath{
			mp4.BoxTypeMdia(), mp4.BoxTypeMinf(), mp4.BoxTypeStbl(),
			mp4.BoxTypeStsd(), mp4.BoxTypeAvc1(),
		})
		if err != nil {
			return nil, err
		}
		if len(bs) != 0 {
			avc1 := bs[0].Payload.(*mp4.VisualSampleEntry)
			t.trackType = trackVideo
			t.height = avc1.Height
			t.width = avc1.Width

			avcBs, err := mp4.ExtractBoxWithPayload(inputFile, bi, mp4.BoxPath{
				mp4.BoxTypeMdia(), mp4.BoxTypeMinf(), mp4.BoxTypeStbl(),
				mp4.BoxTypeStsd(), mp4.BoxTypeAvc1(), mp4.BoxTypeAvcC(),
			})
			if err != nil {
				return nil, err
			}
			if len(avcBs) == 0 {
				return nil, fmt.Errorf("avc1 track must contain an avcC box: trackID=%d", t.id)
			}
			avcC := avcBs[0].Payload.(*mp4.AVCDecoderConfiguration)
			t.avcProfile = avcC.Profile
			t.avcCompat = avcC.ProfileCompatibility
			t.avcLevel = avcC.Level

			tracks = append(tracks, t)
			continue
		}

		audioBis, err := mp4.ExtractBox(inputFile, bi, mp4.BoxPath{
			mp4.BoxTypeMdia(), mp4.BoxTypeMinf(), mp4.BoxTypeStbl(),
			mp4.BoxTypeStsd(), mp4.BoxTypeMp4a(),
		})
		if err != nil {
			return nil, err
		}
		if len(audioBis) != 0 {
			t.trackType = trackAudio
			tracks = append(tracks, t)
			continue
		}

		fmt.Printf("WARN: unsupported track type: trackID=%d\n", t.id)
	}

	sort.Slice(tracks, func(i, j int) bool {
		// audio after video, then ascending trackID within a type
		if tracks[i].trackType != tracks[j].trackType {
			return tracks[i].trackType == trackVideo
		}
		return tracks[i].id < tracks[j].id
	})

	return tracks, nil
}

func checkDuplicateTrackID(tracks []*track) error {
	seen := make(map[uint32]struct{}, len(tracks))
	for _, t := range tracks {
		if _, exists := seen[t.id]; exists {
			return fmt.Errorf("duplicated trackID: %d", t.id)
		}
		seen[t.id] = struct{}{}
	}
	return nil
}

func countTrackType(tracks []*track, tt trackType) int {
	n := 0
	for _, t := range tracks {
		if t.trackType == tt {
			n++
		}
	}
	return n
}

func outputMasterPlaylist(filePath string, tracks []*track) error {
	file, err := os.Create(filePath)
	if err != nil {
		return err
	}
	defer file.Close()

	var audio *track
	videos := make([]*track, 0, len(tracks))
	for _, t := range tracks {
		switch t.trackType {
		case trackAudio:
			audio = t
		case trackVideo:
			videos = append(videos, t)
		}
	}
	// videos are already ordered by ascending trackID
	sort.Slice(videos, func(i, j int) bool { return videos[i].id < videos[j].id })

	if _, err := file.WriteString("#EXTM3U\n"); err != nil {
		return err
	}
	if audio != nil {
		if _, err := file.WriteString("#EXT-X-MEDIA:TYPE=AUDIO,URI=\"" + audioDirName + "/" + playlistFileName + "\",GROUP-ID=\"audio\",NAME=\"audio\",AUTOSELECT=YES,CHANNELS=\"2\"\n"); err != nil {
			return err
		}
	}

	multiVideo := len(videos) > 1
	for _, vt := range videos {
		vdir := vt.dirName(multiVideo)
		codecs := vt.codecString()
		if audio != nil {
			codecs += "," + audio.codecString()
		}
		if _, err := fmt.Fprintf(file,
			"#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"%s\",RESOLUTION=%dx%d",
			vt.bandwidth, codecs, vt.width, vt.height); err != nil {
			return err
		}
		if audio != nil {
			if _, err := file.WriteString(",AUDIO=\"audio\""); err != nil {
				return err
			}
		}
		if _, err := file.WriteString("\n" + vdir + "/" + playlistFileName + "\n"); err != nil {
			return err
		}
	}
	return nil
}

func outputMediaPlaylist(filePath string, segments []segment) error {
	file, err := os.Create(filePath)
	if err != nil {
		return err
	}
	defer file.Close()

	var maxDur float64
	for i := range segments {
		if segments[i].duration > maxDur {
			maxDur = segments[i].duration
		}
	}

	file.WriteString("#EXTM3U\n")
	file.WriteString("#EXT-X-VERSION:7\n")
	if _, err := fmt.Fprintf(file, "#EXT-X-TARGETDURATION:%d\n", int(math.Ceil(maxDur))); err != nil {
		return err
	}
	file.WriteString("#EXT-X-PLAYLIST-TYPE:VOD\n")
	file.WriteString("#EXT-X-MAP:URI=\"" + initMP4FileName + "\"\n")
	for i := range segments {
		if _, err := fmt.Fprintf(file, "#EXTINF:%f,\n", segments[i].duration); err != nil {
			return err
		}
		if _, err := fmt.Fprintf(file, "%s\n", segmentFileName(i)); err != nil {
			return err
		}
	}
	file.WriteString("#EXT-X-ENDLIST\n")
	return nil
}
