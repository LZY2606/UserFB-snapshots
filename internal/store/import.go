package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"time"

	"rtpfreeze/internal/analyze"
	"rtpfreeze/internal/identity"
	"rtpfreeze/internal/rtproto"
)

// ImportResult summarizes an import.
type ImportResult struct {
	CaptureID       int64 `json:"capture_id"`
	PacketCount     int   `json:"packet_count"`
	Tracks          int   `json:"tracks"`
	Generations     int   `json:"generations"`
	TransportEpochs int   `json:"transport_epochs"`
	FeedbackEvents  int   `json:"feedback_events"`
	SenderReports   int   `json:"sender_reports"`
	Freezes         int   `json:"freezes"`
}

// Import parses a JSON Lines capture from r, partitions identities, runs the
// analysis engine and persists everything under one transaction.
func (s *Store) Import(ctx context.Context, source string, r io.Reader) (*ImportResult, error) {
	packets, err := rtproto.DecodeCapture(r)
	if err != nil {
		return nil, fmt.Errorf("decode capture: %w", err)
	}
	if len(packets) == 0 {
		return nil, fmt.Errorf("capture is empty")
	}
	events := extractEvents(packets)
	id := identity.Build(packets, events)
	engine := analyze.NewEngine(packets, id, analyze.DefaultFreezeParams())
	res := engine.Run()

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	capID, err := insertCapture(tx, source, packets)
	if err != nil {
		return nil, err
	}
	if err := insertPackets(tx, capID, packets); err != nil {
		return nil, err
	}
	if err := insertIdentity(tx, capID, id); err != nil {
		return nil, err
	}
	if err := insertRTP(tx, capID, packets, id); err != nil {
		return nil, err
	}
	if err := insertRTCP(tx, capID, packets); err != nil {
		return nil, err
	}
	if err := insertAnalysis(tx, capID, res); err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return &ImportResult{
		CaptureID: capID, PacketCount: len(packets),
		Tracks: len(id.Tracks), Generations: len(id.Generations),
		TransportEpochs: len(id.TransportEpochs),
		FeedbackEvents:  len(res.Feedback), SenderReports: len(res.SRs),
		Freezes: len(res.Freezes),
	}, nil
}

// extractEvents pulls out-of-band markers (e.g. ICE restart) out of APP
// RTCP packets, so the identity builder does not have to understand RTCP.
func extractEvents(packets []rtproto.Packet) []identity.Event {
	var events []identity.Event
	for _, p := range packets {
		if p.Kind != rtproto.KindRTCP {
			continue
		}
		for _, m := range p.RTCP {
			if m.PacketType == rtproto.PTAPP && m.Reason == "ice_restart" {
				events = append(events, identity.Event{
					Index: p.Index, TimeUS: p.TimeUS, Type: "ice_restart",
				})
			}
		}
	}
	return events
}

func insertCapture(tx *sql.Tx, source string, packets []rtproto.Packet) (int64, error) {
	var tmin, tmax int64
	tmin = packets[0].TimeUS
	for _, p := range packets {
		if p.TimeUS < tmin {
			tmin = p.TimeUS
		}
		if p.TimeUS > tmax {
			tmax = p.TimeUS
		}
	}
	res, err := tx.Exec(
		`INSERT INTO captures(source, imported_at_us, packet_count, time_min_us, time_max_us)
		 VALUES (?, ?, ?, ?, ?)`,
		source, time.Now().UnixMicro(), len(packets), tmin, tmax,
	)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func insertPackets(tx *sql.Tx, capID int64, packets []rtproto.Packet) error {
	stmt, err := tx.Prepare(
		`INSERT INTO packets(capture_id, idx, time_us, direction, kind, raw_json)
		 VALUES (?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for i := range packets {
		p := &packets[i]
		raw, err := json.Marshal(p)
		if err != nil {
			return err
		}
		if _, err := stmt.Exec(capID, p.Index, p.TimeUS, string(p.Direction),
			string(p.Kind), string(raw)); err != nil {
			return fmt.Errorf("packet %d: %w", p.Index, err)
		}
	}
	return nil
}

func insertIdentity(tx *sql.Tx, capID int64, id *identity.Result) error {
	trackStmt, err := tx.Prepare(
		`INSERT INTO tracks(capture_id, id, mid, rid, payload_type, synthetic)
		 VALUES (?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer trackStmt.Close()
	for _, t := range id.Tracks {
		if _, err := trackStmt.Exec(capID, t.ID, t.MID, t.RID, t.PayloadType,
			boolInt(t.Synthetic)); err != nil {
			return err
		}
	}

	genStmt, err := tx.Prepare(
		`INSERT INTO generations(capture_id, id, track_id, ssrc, generation_no,
		   start_idx, end_idx, start_us, end_us, open_reason, close_reason, payload_type)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer genStmt.Close()
	for _, g := range id.Generations {
		if _, err := genStmt.Exec(capID, g.ID, g.TrackID, int64(g.SSRC),
			g.GenerationNo, g.StartIndex, g.EndIndex, g.StartUS, g.EndUS,
			g.OpenReason, g.CloseReason, g.PayloadType); err != nil {
			return err
		}
	}

	epStmt, err := tx.Prepare(
		`INSERT INTO transport_epochs(capture_id, id, start_idx, end_idx, start_us, end_us, reason)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer epStmt.Close()
	for _, ep := range id.TransportEpochs {
		if _, err := epStmt.Exec(capID, ep.ID, ep.StartIndex, ep.EndIndex,
			ep.StartUS, ep.EndUS, ep.Reason); err != nil {
			return err
		}
	}
	return nil
}

func insertRTP(tx *sql.Tx, capID int64, packets []rtproto.Packet, id *identity.Result) error {
	stmt, err := tx.Prepare(
		`INSERT INTO rtp_packets(capture_id, packet_idx, time_us, ssrc, payload_type, seq,
		   ext_seq, rtp_timestamp, marker, mid, rid, transport_seq, track_id, gen_id, epoch_id)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for i := range packets {
		p := &packets[i]
		if p.Kind != rtproto.KindRTP || p.RTP == nil {
			continue
		}
		a := id.RTP[p.Index]
		if a == nil {
			continue
		}
		var tseq interface{}
		if p.TransportSeq != nil {
			tseq = int64(*p.TransportSeq)
		}
		if _, err := stmt.Exec(capID, p.Index, p.TimeUS, int64(p.RTP.SSRC),
			p.RTP.PayloadType, int64(p.RTP.Sequence), a.ExtendedSeq,
			int64(p.RTP.Timestamp), boolInt(p.RTP.Marker), p.MID, p.RID,
			tseq, a.TrackID, a.GenerationID, a.TransportEpochID); err != nil {
			return err
		}
	}
	return nil
}

func insertRTCP(tx *sql.Tx, capID int64, packets []rtproto.Packet) error {
	msgStmt, err := tx.Prepare(
		`INSERT INTO rtcp_messages(capture_id, packet_idx, msg_idx, fmt, packet_type,
		   sender_ssrc, media_ssrc, kind) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer msgStmt.Close()
	nackStmt, err := tx.Prepare(
		`INSERT INTO rtcp_nacks(msg_rowid, start_seq, end_seq) VALUES (?, ?, ?)`)
	if err != nil {
		return err
	}
	defer nackStmt.Close()
	blockStmt, err := tx.Prepare(
		`INSERT INTO rtcp_report_blocks(msg_rowid, ssrc, fraction_lost, cumulative_lost,
		   highest_seq, jitter, lsr, dlsr) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer blockStmt.Close()
	siStmt, err := tx.Prepare(
		`INSERT INTO rtcp_sender_info(msg_rowid, ntp_msw, ntp_lsw, rtp_timestamp,
		   packet_count, octet_count) VALUES (?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer siStmt.Close()

	for i := range packets {
		p := &packets[i]
		if p.Kind != rtproto.KindRTCP {
			continue
		}
		for mi, m := range p.RTCP {
			res, err := msgStmt.Exec(capID, p.Index, mi, m.FMT, m.PacketType,
				int64(m.SSRC), int64(m.MediaSSRC), rtcpKind(m))
			if err != nil {
				return err
			}
			rowID, _ := res.LastInsertId()
			for _, n := range m.NACK {
				if _, err := nackStmt.Exec(rowID, int64(n.Start), int64(n.End)); err != nil {
					return err
				}
			}
			for _, b := range m.ReportBlocks {
				if _, err := blockStmt.Exec(rowID, int64(b.SSRC), b.FractionLost,
					b.CumulativeLost, int64(b.HighestSeq), int64(b.Jitter),
					int64(b.LSR), int64(b.DLSR)); err != nil {
					return err
				}
			}
			if m.SenderInfo != nil {
				if _, err := siStmt.Exec(rowID, int64(m.SenderInfo.NTPMSW),
					int64(m.SenderInfo.NTPLSW), int64(m.SenderInfo.RTPTimestamp),
					int64(m.SenderInfo.PacketCount), int64(m.SenderInfo.OctetCount)); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

func rtcpKind(m rtproto.RTCPMessage) string {
	switch {
	case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTNACK:
		return analyze.KindNACK
	case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTPLI:
		return analyze.KindPLI
	case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTFIR:
		return analyze.KindFIR
	case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTTWCC:
		return analyze.KindTWCC
	case m.PacketType == rtproto.PTRR:
		return analyze.KindRR
	case m.PacketType == rtproto.PTSR:
		return analyze.KindSR
	case m.PacketType == rtproto.PTBYE:
		return "bye"
	case m.PacketType == rtproto.PTAPP:
		return "app"
	default:
		return "other"
	}
}

func insertAnalysis(tx *sql.Tx, capID int64, res *analyze.Analysis) error {
	fbStmt, err := tx.Prepare(
		`INSERT INTO feedback_events(capture_id, position, packet_idx, msg_idx, kind,
		   sender_ssrc, media_ssrc, epoch_id, status, note, nack_seqs, twcc_seqs,
		   rtt_us, rtt_notes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer fbStmt.Close()
	candStmt, err := tx.Prepare(
		`INSERT INTO feedback_candidates(feedback_rowid, gen_id, packet_idx, track_id,
		   epoch_id, raw_seq, ext_seq, transport_seq, cross_epoch, note)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer candStmt.Close()

	for pos, ev := range res.Feedback {
		r, err := fbStmt.Exec(capID, pos, ev.PacketIndex, ev.MsgIndex, ev.Kind,
			int64(ev.SenderSSRC), int64(ev.MediaSSRC), ev.TransportEpochID,
			ev.Status, ev.Note, jsonInts(ev.NACKSeqs), jsonInts(ev.TWCCSeqs),
			jsonInt64s(ev.RTTUS), jsonStrings(ev.RTTNotes))
		if err != nil {
			return err
		}
		rowID, _ := r.LastInsertId()
		for _, c := range ev.Candidates {
			if _, err := candStmt.Exec(rowID, c.GenID, c.PacketIndex, c.TrackID,
				c.TransportEpochID, int64(c.RawSeq), c.ExtendedSeq,
				int64(c.TransportSeq), boolInt(c.CrossEpoch), c.Note); err != nil {
				return err
			}
		}
	}

	srStmt, err := tx.Prepare(
		`INSERT INTO sender_reports(capture_id, packet_idx, msg_idx, gen_id, ssrc,
		   ntp_msw, ntp_lsw, ntp_middle32, ntp_us, rtp_timestamp, packet_count, octet_count)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer srStmt.Close()
	for _, sr := range res.SRs {
		if _, err := srStmt.Exec(capID, sr.PacketIndex, sr.MsgIndex, sr.GenID,
			int64(sr.SSRC), int64(sr.NTPMSW), int64(sr.NTPLSW), int64(sr.NTPMiddle32),
			sr.NTPUS, int64(sr.RTPTimestamp), int64(sr.PacketCount),
			int64(sr.OctetCount)); err != nil {
			return err
		}
	}

	fzStmt, err := tx.Prepare(
		`INSERT INTO freezes(capture_id, track_id, gen_id, start_idx, end_idx,
		   start_us, end_us, duration_us, kind, first_missing_ext, last_missing_ext,
		   missing_count) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
	if err != nil {
		return err
	}
	defer fzStmt.Close()
	linkStmt, err := tx.Prepare(
		`INSERT INTO freeze_feedback(freeze_rowid, feedback_pos, position_order)
		 VALUES (?, ?, ?)`)
	if err != nil {
		return err
	}
	defer linkStmt.Close()
	for _, f := range res.Freezes {
		r, err := fzStmt.Exec(capID, f.TrackID, f.GenID, f.StartIndex, f.EndIndex,
			f.StartUS, f.EndUS, f.DurationUS, f.Kind, f.FirstMissingExt,
			f.LastMissingExt, f.MissingCount)
		if err != nil {
			return err
		}
		rowID, _ := r.LastInsertId()
		for order, pos := range f.RelatedFeedback {
			if _, err := linkStmt.Exec(rowID, pos, order); err != nil {
				return err
			}
		}
	}
	return nil
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func jsonInts(v []uint16) string {
	if len(v) == 0 {
		return "[]"
	}
	out := make([]int, len(v))
	for i, x := range v {
		out[i] = int(x)
	}
	b, _ := json.Marshal(out)
	return string(b)
}

func jsonInt64s(v []int64) string {
	if len(v) == 0 {
		return "[]"
	}
	b, _ := json.Marshal(v)
	return string(b)
}

func jsonStrings(v []string) string {
	if len(v) == 0 {
		return "[]"
	}
	b, _ := json.Marshal(v)
	return string(b)
}
