package analyze

// indexFreezes scans each generation's sent packets for two kinds of freeze:
//
//   - loss_gap: a forward jump in extended sequence space, i.e. packets that
//     never arrived at the capture point;
//   - silence: a long time gap between consecutive observed packets even when
//     the sequence numbers are continuous (the encoder simply stopped).
//
// Detection never crosses a generation boundary: the first and last packet of
// separate generations belong to different flows, so the gap between them is
// not a freeze of either one.
func (e *Engine) indexFreezes(out *Analysis) {
	nextID := 0
	for genID, list := range e.byGen {
		if len(list) == 0 {
			continue
		}
		for n := 1; n < len(list); n++ {
			prev := e.sent[list[n-1]]
			cur := e.sent[list[n]]

			// Extended sequence jump => missing packets.
			if gap := cur.extSeq - prev.extSeq; gap > 1 {
				missing := int(gap) - 1
				if missing >= e.params.MinMissing {
					out.Freezes = append(out.Freezes, FreezeInterval{
						ID: nextID, TrackID: cur.trackID, GenID: genID,
						StartIndex: prev.idx, EndIndex: cur.idx,
						StartUS: prev.timeUS, EndUS: cur.timeUS,
						DurationUS:      cur.timeUS - prev.timeUS,
						Kind:            "loss_gap",
						FirstMissingExt: prev.extSeq + 1,
						LastMissingExt:  cur.extSeq - 1,
						MissingCount:    missing,
					})
					nextID++
					continue
				}
			}

			// Continuous sequence but the pipeline went quiet.
			if dt := cur.timeUS - prev.timeUS; dt >= e.params.MinSilenceUS {
				out.Freezes = append(out.Freezes, FreezeInterval{
					ID: nextID, TrackID: cur.trackID, GenID: genID,
					StartIndex: prev.idx, EndIndex: cur.idx,
					StartUS: prev.timeUS, EndUS: cur.timeUS,
					DurationUS: dt, Kind: "silence",
					FirstMissingExt: prev.extSeq, LastMissingExt: prev.extSeq,
				})
				nextID++
			}
		}

		// Tail silence: the generation ended and no further packet closed the
		// interval within the capture. Reported as an open-ended freeze only
		// when the last packet is far enough from capture end.
		last := e.sent[list[len(list)-1]]
		lastTime := e.lastCaptureTime()
		if lastTime-last.timeUS >= e.params.MinSilenceUS && e.genStillOpen(genID) {
			out.Freezes = append(out.Freezes, FreezeInterval{
				ID: nextID, TrackID: last.trackID, GenID: genID,
				StartIndex: last.idx, EndIndex: -1,
				StartUS: last.timeUS, EndUS: lastTime,
				DurationUS: lastTime - last.timeUS, Kind: "silence",
				FirstMissingExt: last.extSeq, LastMissingExt: last.extSeq,
			})
			nextID++
		}
	}
	sortFreezes(out.Freezes)
}

func (e *Engine) genStillOpen(genID int) bool {
	return !e.id.Generations[genID].Closed()
}

func (e *Engine) lastCaptureTime() int64 {
	for i := len(e.packets) - 1; i >= 0; i-- {
		if e.packets[i].TimeUS > 0 {
			return e.packets[i].TimeUS
		}
	}
	return 0
}

func sortFreezes(fs []FreezeInterval) {
	// Insertion sort keeps the deterministic capture-order tiebreak without
	// pulling in another sort closure.
	for i := 1; i < len(fs); i++ {
		for j := i; j > 0 && freezeLess(fs[j], fs[j-1]); j-- {
			fs[j], fs[j-1] = fs[j-1], fs[j]
		}
	}
}

func freezeLess(a, b FreezeInterval) bool {
	if a.StartUS != b.StartUS {
		return a.StartUS < b.StartUS
	}
	return a.ID < b.ID
}

// linkFreezeFeedback attaches feedback events that target the freeze's
// generation (by SSRC) and fall inside the freeze window or the feedback that
// precedes it. Ambiguous and cross-epoch events are linked just like resolved
// ones; the UI shows why they cannot be settled.
func (e *Engine) linkFreezeFeedback(out *Analysis) {
	genSSRC := map[int]uint32{}
	for _, g := range e.id.Generations {
		genSSRC[g.ID] = g.SSRC
	}
	for fi := range out.Freezes {
		f := &out.Freezes[fi]
		ssrc := genSSRC[f.GenID]
		g := e.id.Generations[f.GenID]
		for ei, ev := range out.Feedback {
			if !feedbackMentionsGeneration(ev, f.GenID, ssrc) {
				continue
			}
			// Relevant when it arrives at or after the last packet before the
			// freeze and no later than the end of the generation that owns the
			// freeze. Feedback often lands only after the gap recovers, so the
			// freeze's own end index is deliberately NOT the upper bound.
			if ev.TimeUS < f.StartUS {
				continue
			}
			if g.Closed() && ev.TimeUS > g.EndUS {
				continue
			}
			f.RelatedFeedback = append(f.RelatedFeedback, ei)
		}
	}
}

// feedbackMentionsGeneration reports whether a feedback event could refer to
// the given generation: either its media SSRC equals the generation's SSRC
// (which keeps ambiguous/cross-epoch events linked), or one of its resolved
// candidates is a packet of that generation.
func feedbackMentionsGeneration(ev FeedbackEvent, genID int, ssrc uint32) bool {
	if ev.MediaSSRC == ssrc {
		return true
	}
	for _, c := range ev.Candidates {
		if c.GenID == genID {
			return true
		}
	}
	return false
}
