package mp4_test

import (
	"bytes"
	"fmt"
	"os"

	"github.com/Eyevinn/mp4ff/mp4"
)

// ExampleTrakBox_PlanMovieSeek plans a seek two seconds into a progressive
// movie and reads the preroll interval, from the sync sample where decoding
// must start up to the sample presented at the requested movie time, directly
// from the file with the ranges of the plan. An empty edit would give an
// [mp4.EmptyMovieSeekPlan] instead, which has no ranges to read.
func ExampleTrakBox_PlanMovieSeek() {
	fd, err := os.Open("testdata/bbb_prog_10s.mp4")
	if err != nil {
		panic(err)
	}
	defer fd.Close()
	f, err := mp4.DecodeFile(fd)
	if err != nil {
		panic(err)
	}
	trak := f.Moov.Traks[0] // video track
	const movieTime uint64 = 2000
	plan, err := trak.PlanMovieSeek(movieTime, f.Moov.Mvhd.Timescale)
	if err != nil {
		panic(err)
	}
	switch p := plan.(type) {
	case *mp4.EmptyMovieSeekPlan:
		// An empty edit presents no media here, so there is nothing to read.
		fmt.Printf("empty edit %d at movie time %d\n", p.EditNr, movieTime)
	case *mp4.MediaMovieSeekPlan:
		// Read every range in decode order. The offsets are absolute file
		// positions and stay in decode order despite B-frame reordering.
		var preroll bytes.Buffer
		for _, r := range p.DataRanges {
			buf := make([]byte, r.Size)
			if _, err := fd.ReadAt(buf, int64(r.Offset)); err != nil {
				panic(err)
			}
			preroll.Write(buf)
		}
		fmt.Printf("edit %d: decode from sample %d to presented sample %d, %d ranges, %d bytes\n",
			p.EditNr, p.DecodeSampleNr, p.TargetSampleNr, len(p.DataRanges), preroll.Len())
	}
	// Output: edit 1: decode from sample 16 to presented sample 49, 34 ranges, 51759 bytes
}
