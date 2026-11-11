package flac_test

import (
	"bytes"
	"crypto/md5"
	"fmt"
	"io"
	"log"

	"github.com/mewkiz/flac"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/flac/meta"
)

func ExampleParseFile() {
	// Parse metadata of love.flac
	stream, err := flac.ParseFile("testdata/love.flac")
	if err != nil {
		log.Fatal(err)
	}
	defer stream.Close()

	fmt.Printf("unencoded audio md5sum: %032x\n", stream.Info.MD5sum[:])
	for i, block := range stream.Blocks {
		fmt.Printf("block %d: %v\n", i, block.Type)
	}
	// Output:
	// unencoded audio md5sum: bdf6f7d31f77cb696a02b2192d192a89
	// block 0: seek table
	// block 1: vorbis comment
	// block 2: padding
}

func ExampleOpen() {
	// Open love.flac for audio streaming without parsing metadata.
	stream, err := flac.Open("testdata/love.flac")
	if err != nil {
		log.Fatal(err)
	}
	defer stream.Close()

	// Parse audio samples and verify the MD5 signature of the decoded audio
	// samples.
	md5sum := md5.New()
	for {
		// Parse one frame of audio samples at the time, each frame containing one
		// subframe per audio channel.
		frame, err := stream.ParseNext()
		if err != nil {
			if err == io.EOF {
				break
			}
			log.Fatal(err)
		}
		frame.Hash(md5sum)

		// Print first three samples from each channel of the first five frames.
		if frame.Num < 5 {
			fmt.Printf("frame %d\n", frame.Num)
			for i, subframe := range frame.Subframes {
				fmt.Printf("  subframe %d\n", i)
				for j, sample := range subframe.Samples {
					if j >= 3 {
						break
					}
					fmt.Printf("    sample %d: %v\n", j, sample)
				}
			}
		}
	}
	fmt.Println()

	got, want := md5sum.Sum(nil), stream.Info.MD5sum[:]
	fmt.Println("decoded audio md5sum valid:", bytes.Equal(got, want))
	// Output:
	// frame 0
	//   subframe 0
	//     sample 0: 126
	//     sample 1: 126
	//     sample 2: 126
	//   subframe 1
	//     sample 0: 126
	//     sample 1: 126
	//     sample 2: 126
	// frame 1
	//   subframe 0
	//     sample 0: 126
	//     sample 1: 126
	//     sample 2: 126
	//   subframe 1
	//     sample 0: 126
	//     sample 1: 126
	//     sample 2: 126
	// frame 2
	//   subframe 0
	//     sample 0: 121
	//     sample 1: 130
	//     sample 2: 137
	//   subframe 1
	//     sample 0: 121
	//     sample 1: 130
	//     sample 2: 137
	// frame 3
	//   subframe 0
	//     sample 0: -9501
	//     sample 1: -6912
	//     sample 2: -3916
	//   subframe 1
	//     sample 0: -9501
	//     sample 1: -6912
	//     sample 2: -3916
	// frame 4
	//   subframe 0
	//     sample 0: 513
	//     sample 1: 206
	//     sample 2: 152
	//   subframe 1
	//     sample 0: 513
	//     sample 1: 206
	//     sample 2: 152
	//
	// decoded audio md5sum valid: true
}

func ExampleEncoder_EnableAutoCompression() {
	// Two strongly correlated channels (here: identical ramps) compress far
	// better as inter-channel decorrelated subframes than as verbatim PCM.
	const n = 256
	pcm := make([]int32, n)
	for i := range pcm {
		pcm[i] = int32(i*17 - 2000)
	}
	input := &frame.Frame{
		Header: frame.Header{
			HasFixedBlockSize: true,
			BlockSize:         n,
			SampleRate:        48000,
			Channels:          frame.ChannelsLR,
			BitsPerSample:     16,
		},
	}
	for c := 0; c < 2; c++ {
		input.Subframes = append(input.Subframes, &frame.Subframe{
			SubHeader: frame.SubHeader{Pred: frame.PredVerbatim},
			NSamples:  n,
			Samples:   append([]int32(nil), pcm...),
		})
	}

	buf := new(bytes.Buffer)
	info := &meta.StreamInfo{
		BlockSizeMin: n, BlockSizeMax: n,
		SampleRate: 48000, NChannels: 2, BitsPerSample: 16,
	}
	enc, err := flac.NewEncoder(buf, info)
	if err != nil {
		log.Fatal(err)
	}
	// Automatic compression is disabled by default; opt in per encoder.
	enc.EnableAutoCompression(true)
	if err := enc.WriteFrame(input); err != nil {
		log.Fatal(err)
	}
	if err := enc.Close(); err != nil {
		log.Fatal(err)
	}

	// Decode the file back and verify the original PCM is preserved.
	stream, err := flac.Parse(bytes.NewReader(buf.Bytes()))
	if err != nil {
		log.Fatal(err)
	}
	got, err := stream.ParseNext()
	if err != nil {
		log.Fatal(err)
	}
	roundtrip := true
	for _, subframe := range got.Subframes {
		for i, s := range subframe.Samples {
			if s != pcm[i] {
				roundtrip = false
			}
		}
	}
	fmt.Println("roundtrip ok:", roundtrip)
	// The encoder selected an inter-channel representation instead of plain
	// left/right, and the caller's frame was left untouched.
	fmt.Println("decorrelated:", got.Channels != frame.ChannelsLR)
	fmt.Println("input unchanged:", input.Channels == frame.ChannelsLR &&
		input.Subframes[0].Pred == frame.PredVerbatim)
	// Output:
	// roundtrip ok: true
	// decorrelated: true
	// input unchanged: true
}
