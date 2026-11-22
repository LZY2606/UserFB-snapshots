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

// memFile is a minimal in-memory io.WriteSeeker built only from the standard
// library, sufficient to demonstrate stream-relative STREAMINFO back-filling.
type memFile struct {
	data []byte
	pos  int64
}

func (m *memFile) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.data)) {
		m.data = append(m.data, make([]byte, end-int64(len(m.data)))...)
	}
	copy(m.data[m.pos:end], p)
	m.pos = end
	return len(p), nil
}

func (m *memFile) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekCurrent:
		offset += m.pos
	case io.SeekEnd:
		offset += int64(len(m.data))
	}
	if offset < 0 {
		return m.pos, fmt.Errorf("negative position")
	}
	m.pos = offset
	return offset, nil
}

// newExampleFrame builds a deterministic 16-bit mono frame.
func newExampleFrame(blockSize uint16) *frame.Frame {
	samples := make([]int32, blockSize)
	for i := range samples {
		samples[i] = int32(i*257) - 2048
	}
	return &frame.Frame{
		Header: frame.Header{
			HasFixedBlockSize: true,
			BlockSize:         blockSize,
			SampleRate:        44100,
			Channels:          frame.ChannelsMono,
			BitsPerSample:     16,
		},
		Subframes: []*frame.Subframe{{
			SubHeader: frame.SubHeader{Pred: frame.PredVerbatim},
			NSamples:  int(blockSize),
			Samples:   samples,
		}},
	}
}

// ExampleNewEncoder encodes a frame into a plain io.Writer. Because a
// bytes.Buffer cannot seek, the STREAMINFO fields that are only known once the
// stream is finished (total sample count, MD5 and frame sizes) are left
// untouched, but the audio frames themselves are fully written and decodable.
func ExampleNewEncoder() {
	info := &meta.StreamInfo{
		BlockSizeMin:  16,
		BlockSizeMax:  16,
		SampleRate:    44100,
		NChannels:     1,
		BitsPerSample: 16,
	}
	out := new(bytes.Buffer)
	enc, err := flac.NewEncoder(out, info)
	if err != nil {
		log.Fatal(err)
	}
	enc.EnablePredictionAnalysis(false)
	if err := enc.WriteFrame(newExampleFrame(16)); err != nil {
		log.Fatal(err)
	}
	if err := enc.Close(); err != nil {
		log.Fatal(err)
	}

	stream, err := flac.Parse(bytes.NewReader(out.Bytes()))
	if err != nil {
		log.Fatal(err)
	}
	f, err := stream.ParseNext()
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("channels=%d blocksize=%d first=%d\n", stream.Info.NChannels, f.BlockSize, f.Subframes[0].Samples[0])
	// Output:
	// channels=1 blocksize=16 first=-2048
}

// ExampleEncoder_Close demonstrates appending an independent FLAC stream to a
// writer that already holds unrelated bytes. The encoder starts at the current
// position, writes its own "fLaC" signature and metadata there, and on Close
// back-fills only this stream's STREAMINFO before restoring the cursor to the
// end of the new stream's audio. The prefix is never modified.
func ExampleEncoder_Close() {
	prefix := []byte("PREVIOUS-DATA")
	f := &memFile{data: append([]byte(nil), prefix...)}
	f.pos = int64(len(prefix))

	info := &meta.StreamInfo{
		BlockSizeMin:  16,
		BlockSizeMax:  16,
		SampleRate:    44100,
		NChannels:     1,
		BitsPerSample: 16,
	}
	enc, err := flac.NewEncoder(f, info)
	if err != nil {
		log.Fatal(err)
	}
	enc.EnablePredictionAnalysis(false)
	if err := enc.WriteFrame(newExampleFrame(16)); err != nil {
		log.Fatal(err)
	}
	audioEnd := f.pos
	if err := enc.Close(); err != nil {
		log.Fatal(err)
	}

	fmt.Printf("prefix kept: %t\n", bytes.Equal(f.data[:len(prefix)], prefix))
	fmt.Printf("cursor restored: %t\n", f.pos == audioEnd)

	stream, err := flac.Parse(bytes.NewReader(f.data[len(prefix):]))
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("back-filled nsamples=%d md5-set=%t\n", stream.Info.NSamples, stream.Info.MD5sum != [16]byte{})
	// Output:
	// prefix kept: true
	// cursor restored: true
	// back-filled nsamples=16 md5-set=true
}
