// Example aligned cuts a video and an audio HLS media playlist at the same
// absolute program-time boundaries and rebuilds both as closed VOD
// highlight reels with a continuous program clock. Nothing is downloaded
// or re-encoded: only complete source segments are selected.
package main

import (
	"fmt"
	"log"
	"strings"
	"time"

	m3u8 "github.com/grafov/m3u8"
)

// Both hand-written playlists describe the same program clock starting at
// programStart. The video track uses 2-second segments carried as byte
// ranges of one file with two different initialization sections and a
// discontinuity; the audio track uses eight 1-second segments.
const videoPlaylist = `#EXTM3U
#EXT-X-VERSION:6
#EXT-X-TARGETDURATION:2
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-MAP:URI="init-a.mp4",BYTERANGE=100@0
#EXT-X-PROGRAM-DATE-TIME:2026-04-01T00:00:00Z
#EXTINF:2.000,
#EXT-X-BYTERANGE:20@0
file.mp4
#EXTINF:2.000,
#EXT-X-BYTERANGE:20
file.mp4
#EXT-X-MAP:URI="init-b.mp4"
#EXT-X-DISCONTINUITY
#EXT-X-PROGRAM-DATE-TIME:2026-04-01T00:00:04Z
#EXTINF:2.000,
#EXT-X-BYTERANGE:20
file.mp4
#EXTINF:2.000,
#EXT-X-BYTERANGE:20
file.mp4
#EXT-X-ENDLIST
`

const audioPlaylist = `#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:1
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-PROGRAM-DATE-TIME:2026-04-01T00:00:00Z
#EXTINF:1.000,
a0.aac
#EXTINF:1.000,
a1.aac
#EXTINF:1.000,
a2.aac
#EXTINF:1.000,
a3.aac
#EXTINF:1.000,
a4.aac
#EXTINF:1.000,
a5.aac
#EXTINF:1.000,
a6.aac
#EXTINF:1.000,
a7.aac
#EXT-X-ENDLIST
`

func mustDecode(name, raw string) *m3u8.MediaPlaylist {
	p, err := m3u8.NewMediaPlaylist(0, 16)
	if err != nil {
		log.Fatalf("%s: create playlist: %v", name, err)
	}
	if err := p.DecodeFrom(strings.NewReader(raw), true); err != nil {
		log.Fatalf("%s: decode playlist: %v", name, err)
	}
	return p
}

func main() {
	video := mustDecode("video", videoPlaylist)
	audio := mustDecode("audio", audioPlaylist)

	programStart := time.Date(2026, time.April, 1, 0, 0, 0, 0, time.UTC)
	// Two non-adjacent intervals of program time: [2s,4s) and [6s,8s).
	cuts := []m3u8.WallRange{
		{Start: programStart.Add(2 * time.Second), End: programStart.Add(4 * time.Second)},
		{Start: programStart.Add(6 * time.Second), End: programStart.Add(8 * time.Second)},
	}
	// The rebuilt reels run on a fresh, continuous clock from this origin.
	origin := time.Date(2030, time.January, 1, 0, 0, 0, 0, time.UTC)

	reels, err := m3u8.AssembleAligned(
		map[string]*m3u8.MediaPlaylist{"video": video, "audio": audio},
		cuts, origin)
	if err != nil {
		log.Fatalf("aligned assembly failed: %v", err)
	}

	for _, name := range []string{"video", "audio"} {
		reel := reels[name]
		fmt.Printf("=== %s reel: %d segment(s), closed VOD, sequence starts at %d ===\n",
			name, reel.Count(), reel.SeqNo)
		// Encode really emits the rewritten playlist, including the rebuilt
		// PROGRAM-DATE-TIME values and any MAP/byte-range switches.
		fmt.Println(reel.Encode().String())
	}
}
