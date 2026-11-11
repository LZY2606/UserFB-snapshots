package beep

// Take returns a Streamer which streams at most num samples from s.
//
// The returned Streamer propagates s's errors throught Err.
func Take(num int, s Streamer) Streamer {
	return &take{
		s:       s,
		remains: num,
	}
}

type take struct {
	s       Streamer
	remains int
}

func (t *take) Stream(samples [][2]float64) (n int, ok bool) {
	if t.remains <= 0 {
		return 0, false
	}
	toStream := t.remains
	if len(samples) < toStream {
		toStream = len(samples)
	}
	n, ok = t.s.Stream(samples[:toStream])
	t.remains -= n
	return n, ok
}

func (t *take) Err() error {
	return t.s.Err()
}

// Loop takes a StreamSeeker and plays it count times. If count is negative, s is looped infinitely.
//
// The returned Streamer propagates s's errors.
func Loop(count int, s StreamSeeker) Streamer {
	return &loop{
		s:       s,
		remains: count,
	}
}

type loop struct {
	s       StreamSeeker
	remains int
}

func (l *loop) Stream(samples [][2]float64) (n int, ok bool) {
	if l.remains == 0 || l.s.Err() != nil {
		return 0, false
	}
	for len(samples) > 0 {
		sn, sok := l.s.Stream(samples)
		if !sok {
			if l.remains == 0 {
				break
			}
			err := l.s.Seek(0)
			if err != nil {
				return n, true
			}
			if l.remains > 0 {
				l.remains--
			}
			continue
		}
		samples = samples[sn:]
		n += sn
	}
	return n, true
}

func (l *loop) Err() error {
	return l.s.Err()
}

// Seq takes zero or more Streamers and returns a Streamer which streams them one by one without pauses.
//
// Seq does not propagate errors from the Streamers.
func Seq(s ...Streamer) Streamer {
	i := 0
	return StreamerFunc(func(samples [][2]float64) (n int, ok bool) {
		for i < len(s) && len(samples) > 0 {
			sn, sok := s[i].Stream(samples)
			samples = samples[sn:]
			n, ok = n+sn, ok || sok
			if !sok {
				i++
			}
		}
		return n, ok
	})
}

// Mix takes zero or more Streamers and returns a Streamer which streames them mixed together.
//
// Mix does not propagate errors from the Streamers.
func Mix(s ...Streamer) Streamer {
	return StreamerFunc(func(samples [][2]float64) (n int, ok bool) {
		var tmp [512][2]float64

		for len(samples) > 0 {
			toStream := len(tmp)
			if toStream > len(samples) {
				toStream = len(samples)
			}

			// clear the samples
			for i := range samples[:toStream] {
				samples[i] = [2]float64{}
			}

			snMax := 0 // max number of streamed samples in this iteration
			for _, st := range s {
				// mix the stream
				sn, sok := st.Stream(tmp[:toStream])
				if sn > snMax {
					snMax = sn
				}
				ok = ok || sok

				for i := range tmp[:sn] {
					samples[i][0] += tmp[i][0]
					samples[i][1] += tmp[i][1]
				}
			}

			n += snMax
			if snMax < len(tmp) {
				break
			}
			samples = samples[snMax:]
		}

		return n, ok
	})
}

// Dup returns two Streamers which both stream the same data as the original s. The two Streamers
// can't be used concurrently without synchronization.
//
// The two returned Streamers may stream at different paces. Samples streamed by one of them are
// buffered for the other. Once s is drained, each of the returned Streamers keeps streaming its
// buffered samples and only reports drainage after its buffer is empty. An error of s is
// propagated through a returned Streamer's Err only after that Streamer itself has become
// drained.
func Dup(s Streamer) (t, u Streamer) {
	shared := &dupShared{s: s}
	var tBuf, uBuf [][2]float64
	return &dup{shared: shared, myBuf: &tBuf, itsBuf: &uBuf},
		&dup{shared: shared, myBuf: &uBuf, itsBuf: &tBuf}
}

// dupShared holds the state shared by both Streamers returned by Dup: the source itself and
// whether it has been drained. The source is streamed from at most once per Stream call of
// either branch, and once it reported drainage it is never streamed from again, no matter how
// often either branch is asked for more samples.
type dupShared struct {
	s         Streamer
	exhausted bool  // s returned 0, false and must not be called again
	err       error // s's error captured at exhaustion, nil for a clean EOF
}

// dup is one branch of a Dup pair. Its drained flag tracks whether this branch has already
// reported drainage to its caller; this is deliberately separate from the shared exhaustion
// state, because a branch may still hold buffered samples (and must keep Err nil) after the
// shared source is already exhausted.
type dup struct {
	shared        *dupShared
	myBuf, itsBuf *[][2]float64
	drained       bool // this branch returned 0, false to its caller
}

func (d *dup) Stream(samples [][2]float64) (n int, ok bool) {
	if d.drained {
		return 0, false
	}
	if len(samples) == 0 {
		// A zero-length request neither pulls from the source nor consumes the
		// buffer, and must not surface a pending termination early.
		return 0, true
	}

	buf := *d.myBuf
	n = copy(samples, buf)
	*d.myBuf = buf[n:]
	samples = samples[n:]

	if len(samples) > 0 && !d.shared.exhausted {
		sn, sok := d.shared.s.Stream(samples)
		if sn > 0 {
			// Append copies the samples, so the buffer never aliases the
			// caller's memory.
			*d.itsBuf = append(*d.itsBuf, samples[:sn]...)
		}
		n += sn
		if !sok {
			d.shared.exhausted = true
			d.shared.err = d.shared.s.Err()
		}
	}

	if n > 0 {
		// Deliver the samples obtained so far even if the source just failed;
		// the failure is reported on this branch's next non-empty request.
		return n, true
	}
	if d.shared.exhausted {
		d.drained = true
		return 0, false
	}
	return 0, true
}

func (d *dup) Err() error {
	if !d.drained {
		// Until this branch itself reported drainage, a failure of the shared
		// source is not this branch's error yet: it may still stream buffered
		// samples.
		return nil
	}
	return d.shared.err
}
