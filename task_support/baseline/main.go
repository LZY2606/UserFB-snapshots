package main

import (
	"bytes"
	"fmt"
	m "github.com/sinshu/go-meltysynth/meltysynth"
	"github.com/sinshu/go-meltysynth/task_support/fontfixture"
	"math"
	"os"
)

type measured struct {
	r                    *bytes.Reader
	pos, start, end, pcm int
}

func (r *measured) Read(p []byte) (int, error) {
	n, e := r.r.Read(p)
	lo, hi := r.pos, r.pos+n
	if lo < r.start {
		lo = r.start
	}
	if hi > r.end {
		hi = r.end
	}
	if hi > lo {
		r.pcm += hi - lo
	}
	r.pos += n
	return n, e
}
func run() error {
	data, start, size := fontfixture.Build()
	r := &measured{r: bytes.NewReader(data), start: start, end: start + size}
	sf, e := m.NewSoundFont(r)
	if e != nil {
		return e
	}
	if len(sf.Presets) != 1 || len(sf.WaveData) != 110 || r.pcm != 220 {
		return fmt.Errorf("unexpected load presets=%d samples=%d pcm_read=%d", len(sf.Presets), len(sf.WaveData), r.pcm)
	}
	settings := m.NewSynthesizerSettings(22050)
	settings.EnableReverbAndChorus = false
	a, e := m.NewSynthesizer(sf, settings)
	if e != nil {
		return e
	}
	b, e := m.NewSynthesizer(sf, settings)
	if e != nil {
		return e
	}
	a.NoteOn(0, 60, 100)
	b.NoteOn(0, 60, 100)
	left, right := make([]float32, 384), make([]float32, 384)
	a.Render(left, right)
	l2, r2 := make([]float32, 384), make([]float32, 384)
	at := 0
	for _, n := range []int{7, 61, 3, 128, 185} {
		b.Render(l2[at:at+n], r2[at:at+n])
		at += n
	}
	energy := 0.0
	for i, x := range left {
		if math.IsNaN(float64(x)) || math.IsInf(float64(x), 0) || x != l2[i] || right[i] != r2[i] {
			return fmt.Errorf("PCM mismatch at %d", i)
		}
		energy += float64(x*x + right[i]*right[i])
	}
	if energy < 0.001 {
		return fmt.Errorf("silent production path")
	}
	a.NoteOff(0, 60)
	a.Render(make([]float32, 256), make([]float32, 256))
	fmt.Printf("EAGER_FONT samples=%d smpl_read=%d\n", len(sf.WaveData), r.pcm)
	fmt.Printf("SYNTH_PCM frames=384 chunked=matched energy=%.8f\n", energy)
	return nil
}
func main() {
	if e := run(); e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
}
