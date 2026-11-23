package oggvorbis

import (
	"io"

	"github.com/jfreymuth/vorbis"
)

// Format contains information about the audio format of an ogg/vorbis file.
// For chained streams the format is taken from the first segment; only chains
// whose segments all share the same sample rate and channel count are
// supported.
type Format struct {
	SampleRate int
	Channels   int
	Bitrate    vorbis.Bitrate
}

// ReadAll decodes audio from in until an error or EOF. For chained ogg
// streams the returned slice is the concatenation of every segment's fully
// decoded PCM, in the same global frame coordinates as Reader.Position.
func ReadAll(in io.Reader) ([]float32, *Format, error) {
	r, err := NewReader(in)
	if err != nil {
		return nil, nil, err
	}
	format := &Format{
		SampleRate: r.SampleRate(),
		Channels:   r.Channels(),
		Bitrate:    r.Bitrate(),
	}
	if r.Length() > 0 {
		result := make([]float32, (r.Length()-r.Position())*int64(r.Channels()))
		read := 0

		for {
			n, err := r.Read(result[read:])
			read += n
			if err != nil || read == len(result) {
				if err == io.EOF {
					err = nil
				}
				return result[:read], format, err
			}
			if n == 0 {
				return result[:read], format, io.ErrNoProgress
			}
		}
	}
	buf := make([]float32, r.dec.BufferSize())
	var result []float32
	for {
		n, err := r.Read(buf)
		result = append(result, buf[:n]...)
		if err != nil {
			if err == io.EOF {
				err = nil
			}
			return result, format, err
		}
		if n == 0 {
			return result, format, io.ErrNoProgress
		}
	}
}

// GetFormat reads the first ogg page from in to get the audio format.
func GetFormat(in io.Reader) (*Format, error) {
	var dec vorbis.Decoder
	r := oggReader{source: in}
	p, err := r.NextPacket()
	if err != nil {
		return nil, err
	}
	err = dec.ReadHeader(p)
	if err != nil {
		return nil, err
	}
	return &Format{
		SampleRate: dec.SampleRate(),
		Channels:   dec.Channels(),
		Bitrate:    dec.Bitrate,
	}, nil
}

// GetCommentHeader returns a struct containing info from the comment header.
func GetCommentHeader(in io.Reader) (vorbis.CommentHeader, error) {
	var dec vorbis.Decoder
	r := oggReader{source: in}
	for i := 0; i < 2; i++ {
		p, err := r.NextPacket()
		if err != nil {
			return vorbis.CommentHeader{}, err
		}
		err = dec.ReadHeader(p)
		if err != nil {
			return vorbis.CommentHeader{}, err
		}
	}
	return dec.CommentHeader, nil
}

// GetLength returns the total length of the file in frames and the audio
// format. For chained streams the length is the sum of every segment's
// trimmed frame count.
func GetLength(in io.ReadSeeker) (int64, *Format, error) {
	idx, err := buildChainIndex(in)
	if err != nil {
		return 0, nil, err
	}
	var dec vorbis.Decoder
	if err := dec.ReadHeader(idx.segments[0].id); err != nil {
		return 0, nil, err
	}
	format := &Format{
		SampleRate: dec.SampleRate(),
		Channels:   dec.Channels(),
		Bitrate:    dec.Bitrate,
	}
	return idx.length, format, nil
}
