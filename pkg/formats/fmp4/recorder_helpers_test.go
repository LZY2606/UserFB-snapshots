package fmp4

import (
	"io"

	"github.com/bluenviron/mediacommon/v2/pkg/codecs/mpeg4audio"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
)

// recordingMemFile is a minimal in-package RecordingFile.
type recordingMemFile struct {
	data []byte
	pos  int64
}

func (f *recordingMemFile) Read(p []byte) (int, error) {
	if f.pos >= int64(len(f.data)) {
		return 0, io.EOF
	}
	n := copy(p, f.data[f.pos:])
	f.pos += int64(n)
	return n, nil
}

func (f *recordingMemFile) Write(p []byte) (int, error) {
	end := f.pos + int64(len(p))
	if end > int64(len(f.data)) {
		f.data = append(f.data, make([]byte, end-int64(len(f.data)))...)
	}
	copy(f.data[f.pos:end], p)
	f.pos = end
	return len(p), nil
}

func (f *recordingMemFile) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		f.pos = offset
	case io.SeekCurrent:
		f.pos += offset
	case io.SeekEnd:
		f.pos = int64(len(f.data)) + offset
	}
	return f.pos, nil
}

func (f *recordingMemFile) Truncate(size int64) error {
	f.data = f.data[:size]
	if f.pos > size {
		f.pos = size
	}
	return nil
}

func (f *recordingMemFile) Sync() error { return nil }

func audioOnlyCodec() *codecs.MPEG4Audio {
	return &codecs.MPEG4Audio{
		Config: mpeg4audio.AudioSpecificConfig{
			Type:          2,
			SampleRate:    44100,
			ChannelConfig: 2,
		},
	}
}
