// Copyright 2019 The ebml-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mkvcore

// BlockWriter is a Matroska block writer interface.
type BlockWriter interface {
	// Write a block to the connected Matroska writer.
	// timestamp is in millisecond.
	Write(keyframe bool, timestamp int64, b []byte) (int, error)
}

// BlockReader is a Matroska block reader interface.
type BlockReader interface {
	// Read a block from the connected Matroska reader.
	Read() (b []byte, keyframe bool, timestamp int64, err error)
}

// BlockCloser is a Matroska closer interface.
type BlockCloser interface {
	// Close the stream frame writer.
	// Output Matroska will be closed after closing all FrameWriter.
	Close() error
}

// BlockWriteCloser groups Writer and Closer.
type BlockWriteCloser interface {
	BlockWriter
	BlockCloser
}

// BlockReadCloser groups Reader and Closer.
type BlockReadCloser interface {
	BlockReader
	BlockCloser
}

// TrackEntryGetter is a interface to get TrackEntry.
type TrackEntryGetter interface {
	TrackEntry() TrackEntry
}

// BlockReadCloserWithTrackEntry groups BlockReadCloser and TrackEntryGetter.
type BlockReadCloserWithTrackEntry interface {
	BlockReadCloser
	TrackEntryGetter
}

// TimedFrame is a compressed frame with its presentation timing in nanoseconds.
//
// TimestampNS is the original presentation start of the compressed frame,
// derived from the cluster and block timecodes, the segment TimecodeScale and
// the track CodecDelay. It may be negative.
//
// DurationNS is the nominal frame duration in nanoseconds. A nil pointer means
// that the container does not carry enough information to determine it; the
// duration is never reported as zero to stand for an unknown value.
//
// PresentStartNS and PresentDurationNS describe the presentation interval left
// after applying BlockGroup DiscardPadding. PresentDurationNS is nil when the
// group duration is unknown. A frame fully removed by padding is still
// delivered with a zero-length present interval.
//
// Data always holds the original compressed frame and is owned by the caller;
// it never aliases an internal parse buffer that is reused later.
type TimedFrame struct {
	Data              []byte
	Keyframe          bool
	TimestampNS       int64
	DurationNS        *int64
	PresentStartNS    int64
	PresentDurationNS *int64
}

// TimedBlockReader reads timed compressed frames of a Matroska track.
type TimedBlockReader interface {
	// ReadFrame reads the next timed frame. io.EOF is returned at the end of
	// the track, or after a fatal parse error has been reported once.
	ReadFrame() (*TimedFrame, error)
}

// TimedBlockCloser closes a TimedBlockReader. Closing one track never affects
// the other tracks of the same reader set.
type TimedBlockCloser interface {
	Close() error
}

// TimedBlockReadCloser groups TimedBlockReader, TimedBlockCloser and
// TrackEntryGetter.
type TimedBlockReadCloser interface {
	TimedBlockReader
	TimedBlockCloser
	TrackEntryGetter
}
