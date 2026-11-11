package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
)

// CaptureSummary is one row of the capture list.
type CaptureSummary struct {
	ID           int64  `json:"id"`
	Source       string `json:"source"`
	ImportedAtUS int64  `json:"imported_at_us"`
	PacketCount  int    `json:"packet_count"`
	TimeMinUS    int64  `json:"time_min_us"`
	TimeMaxUS    int64  `json:"time_max_us"`
}

// ListCaptures returns every imported capture, newest first.
func (s *Store) ListCaptures(ctx context.Context) ([]CaptureSummary, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, source, imported_at_us, packet_count, time_min_us, time_max_us
		 FROM captures ORDER BY id DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []CaptureSummary
	for rows.Next() {
		var c CaptureSummary
		if err := rows.Scan(&c.ID, &c.Source, &c.ImportedAtUS, &c.PacketCount,
			&c.TimeMinUS, &c.TimeMaxUS); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// TrackDTO describes a track to the UI.
type TrackDTO struct {
	ID          int    `json:"id"`
	MID         string `json:"mid"`
	RID         string `json:"rid"`
	PayloadType int    `json:"payload_type"`
	Synthetic   bool   `json:"synthetic"`
}

// GenerationDTO describes one SSRC generation.
type GenerationDTO struct {
	ID           int    `json:"id"`
	TrackID      int    `json:"track_id"`
	SSRC         uint32 `json:"ssrc"`
	GenerationNo int    `json:"generation_no"`
	StartIndex   int    `json:"start_index"`
	EndIndex     int    `json:"end_index"`
	StartUS      int64  `json:"start_us"`
	EndUS        int64  `json:"end_us"`
	OpenReason   string `json:"open_reason"`
	CloseReason  string `json:"close_reason"`
	PayloadType  int    `json:"payload_type"`
}

// EpochDTO describes one transport epoch.
type EpochDTO struct {
	ID         int    `json:"id"`
	StartIndex int    `json:"start_index"`
	EndIndex   int    `json:"end_index"`
	StartUS    int64  `json:"start_us"`
	EndUS      int64  `json:"end_us"`
	Reason     string `json:"reason"`
}

// Overview is the top-level view of one capture.
type Overview struct {
	Capture        CaptureSummary  `json:"capture"`
	Tracks         []TrackDTO      `json:"tracks"`
	Generations    []GenerationDTO `json:"generations"`
	Epochs         []EpochDTO      `json:"transport_epochs"`
	FeedbackCounts map[string]int  `json:"feedback_counts"`
	StatusCounts   map[string]int  `json:"status_counts"`
	FreezeCount    int             `json:"freeze_count"`
}

// Overview loads the structural summary of one capture.
func (s *Store) Overview(ctx context.Context, capID int64) (*Overview, error) {
	o := &Overview{
		FeedbackCounts: map[string]int{},
		StatusCounts:   map[string]int{},
	}
	row := s.db.QueryRowContext(ctx,
		`SELECT id, source, imported_at_us, packet_count, time_min_us, time_max_us
		 FROM captures WHERE id = ?`, capID)
	if err := row.Scan(&o.Capture.ID, &o.Capture.Source, &o.Capture.ImportedAtUS,
		&o.Capture.PacketCount, &o.Capture.TimeMinUS, &o.Capture.TimeMaxUS); err != nil {
		if err == sql.ErrNoRows {
			return nil, ErrNotFound
		}
		return nil, err
	}

	tr, err := s.db.QueryContext(ctx,
		`SELECT id, mid, rid, payload_type, synthetic FROM tracks
		 WHERE capture_id = ? ORDER BY id`, capID)
	if err != nil {
		return nil, err
	}
	for tr.Next() {
		var t TrackDTO
		var syn int
		if err := tr.Scan(&t.ID, &t.MID, &t.RID, &t.PayloadType, &syn); err != nil {
			tr.Close()
			return nil, err
		}
		t.Synthetic = syn == 1
		o.Tracks = append(o.Tracks, t)
	}
	tr.Close()

	gr, err := s.db.QueryContext(ctx,
		`SELECT id, track_id, ssrc, generation_no, start_idx, end_idx, start_us,
		   end_us, open_reason, close_reason, payload_type
		 FROM generations WHERE capture_id = ? ORDER BY id`, capID)
	if err != nil {
		return nil, err
	}
	for gr.Next() {
		var g GenerationDTO
		var ssrc int64
		if err := gr.Scan(&g.ID, &g.TrackID, &ssrc, &g.GenerationNo, &g.StartIndex,
			&g.EndIndex, &g.StartUS, &g.EndUS, &g.OpenReason, &g.CloseReason,
			&g.PayloadType); err != nil {
			gr.Close()
			return nil, err
		}
		g.SSRC = uint32(ssrc)
		o.Generations = append(o.Generations, g)
	}
	gr.Close()

	er, err := s.db.QueryContext(ctx,
		`SELECT id, start_idx, end_idx, start_us, end_us, reason
		 FROM transport_epochs WHERE capture_id = ? ORDER BY id`, capID)
	if err != nil {
		return nil, err
	}
	for er.Next() {
		var ep EpochDTO
		if err := er.Scan(&ep.ID, &ep.StartIndex, &ep.EndIndex, &ep.StartUS,
			&ep.EndUS, &ep.Reason); err != nil {
			er.Close()
			return nil, err
		}
		o.Epochs = append(o.Epochs, ep)
	}
	er.Close()

	fr, err := s.db.QueryContext(ctx,
		`SELECT kind, status, COUNT(*) FROM feedback_events
		 WHERE capture_id = ? GROUP BY kind, status`, capID)
	if err != nil {
		return nil, err
	}
	for fr.Next() {
		var kind, status string
		var n int
		if err := fr.Scan(&kind, &status, &n); err != nil {
			fr.Close()
			return nil, err
		}
		o.FeedbackCounts[kind] += n
		o.StatusCounts[status] += n
	}
	fr.Close()

	if err := s.db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM freezes WHERE capture_id = ?`, capID).Scan(&o.FreezeCount); err != nil {
		return nil, err
	}
	return o, nil
}

// RTPPacketRow is one packet in the packet browser.
type RTPPacketRow struct {
	PacketIndex  int     `json:"packet_index"`
	TimeUS       int64   `json:"time_us"`
	Direction    string  `json:"direction"`
	SSRC         uint32  `json:"ssrc"`
	PayloadType  int     `json:"payload_type"`
	Seq          uint16  `json:"seq"`
	ExtSeq       int64   `json:"ext_seq"`
	RTPTimestamp uint32  `json:"rtp_timestamp"`
	Marker       bool    `json:"marker"`
	MID          string  `json:"mid"`
	RID          string  `json:"rid"`
	TransportSeq *uint16 `json:"transport_seq"`
	TrackID      int     `json:"track_id"`
	GenID        int     `json:"gen_id"`
	EpochID      int     `json:"epoch_id"`
}

// RTPFilter narrows a packet query.
type RTPFilter struct {
	GenID   int // -1 = no filter
	TrackID int // -1 = no filter
	Limit   int
}

// RTPPackets returns RTP packets in capture (original index) order.
func (s *Store) RTPPackets(ctx context.Context, capID int64, f RTPFilter) ([]RTPPacketRow, error) {
	q := `SELECT packet_idx, time_us, ssrc, payload_type, seq, ext_seq,
		rtp_timestamp, marker, mid, rid, transport_seq, track_id, gen_id, epoch_id
		FROM rtp_packets WHERE capture_id = ?`
	args := []any{capID}
	if f.GenID >= 0 {
		q += ` AND gen_id = ?`
		args = append(args, f.GenID)
	}
	if f.TrackID >= 0 {
		q += ` AND track_id = ?`
		args = append(args, f.TrackID)
	}
	q += ` ORDER BY packet_idx`
	if f.Limit > 0 {
		q += ` LIMIT ?`
		args = append(args, f.Limit)
	}
	rows, err := s.db.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []RTPPacketRow
	for rows.Next() {
		var r RTPPacketRow
		var ssrc, seq int64
		var marker int
		var tseq sql.NullInt64
		if err := rows.Scan(&r.PacketIndex, &r.TimeUS, &ssrc, &r.PayloadType,
			&seq, &r.ExtSeq, &r.RTPTimestamp, &marker, &r.MID, &r.RID,
			&tseq, &r.TrackID, &r.GenID, &r.EpochID); err != nil {
			return nil, err
		}
		r.SSRC, r.Seq = uint32(ssrc), uint16(seq)
		r.Marker = marker == 1
		if tseq.Valid {
			v := uint16(tseq.Int64)
			r.TransportSeq = &v
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// CandidateDTO is one stored causality candidate.
type CandidateDTO struct {
	GenID        int    `json:"gen_id"`
	PacketIndex  int    `json:"packet_index"`
	TrackID      int    `json:"track_id"`
	EpochID      int    `json:"epoch_id"`
	RawSeq       uint16 `json:"raw_seq"`
	ExtSeq       int64  `json:"ext_seq"`
	TransportSeq uint16 `json:"transport_seq"`
	CrossEpoch   bool   `json:"cross_epoch"`
	Note         string `json:"note"`
}

// FeedbackDTO is one feedback event with its candidates.
type FeedbackDTO struct {
	Position    int            `json:"position"`
	PacketIndex int            `json:"packet_index"`
	MsgIndex    int            `json:"msg_index"`
	Kind        string         `json:"kind"`
	SenderSSRC  uint32         `json:"sender_ssrc"`
	MediaSSRC   uint32         `json:"media_ssrc"`
	EpochID     int            `json:"epoch_id"`
	Status      string         `json:"status"`
	Note        string         `json:"note"`
	NACKSeqs    []uint16       `json:"nack_seqs"`
	TWCCSeqs    []uint16       `json:"twcc_seqs"`
	RTTUS       []int64        `json:"rtt_us"`
	RTTNotes    []string       `json:"rtt_notes"`
	Candidates  []CandidateDTO `json:"candidates"`
}

// FeedbackFilter narrows feedback queries.
type FeedbackFilter struct {
	Kind   string
	Status string
	GenID  int // match candidates on this generation
	Limit  int
}

// Feedback lists feedback events with candidates.
func (s *Store) Feedback(ctx context.Context, capID int64, f FeedbackFilter) ([]FeedbackDTO, error) {
	q := `SELECT id, position, packet_idx, msg_idx, kind, sender_ssrc, media_ssrc,
		epoch_id, status, note, nack_seqs, twcc_seqs, rtt_us, rtt_notes
		FROM feedback_events WHERE capture_id = ?`
	args := []any{capID}
	if f.Kind != "" {
		q += ` AND kind = ?`
		args = append(args, f.Kind)
	}
	if f.Status != "" {
		q += ` AND status = ?`
		args = append(args, f.Status)
	}
	if f.GenID >= 0 {
		q += ` AND id IN (SELECT feedback_rowid FROM feedback_candidates WHERE gen_id = ?)`
		args = append(args, f.GenID)
	}
	q += ` ORDER BY position`
	if f.Limit > 0 {
		q += ` LIMIT ?`
		args = append(args, f.Limit)
	}
	rows, err := s.db.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, err
	}
	type rawFB struct {
		dto                    FeedbackDTO
		rowID                  int64
		nack, twcc, rtt, notes string
	}
	var raws []rawFB
	for rows.Next() {
		var r rawFB
		var sender, media int64
		if err := rows.Scan(&r.rowID, &r.dto.Position, &r.dto.PacketIndex, &r.dto.MsgIndex,
			&r.dto.Kind, &sender, &media, &r.dto.EpochID, &r.dto.Status, &r.dto.Note,
			&r.nack, &r.twcc, &r.rtt, &r.notes); err != nil {
			rows.Close()
			return nil, err
		}
		r.dto.SenderSSRC, r.dto.MediaSSRC = uint32(sender), uint32(media)
		raws = append(raws, r)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()

	out := make([]FeedbackDTO, 0, len(raws))
	for _, r := range raws {
		dto := r.dto
		dto.NACKSeqs = decodeUint16s(r.nack)
		dto.TWCCSeqs = decodeUint16s(r.twcc)
		_ = json.Unmarshal([]byte(r.rtt), &dto.RTTUS)
		_ = json.Unmarshal([]byte(r.notes), &dto.RTTNotes)
		// Child-table query happens only after the outer cursor is closed, so
		// this is safe even with a single-connection pool.
		cands, err := s.candidates(ctx, r.rowID)
		if err != nil {
			return nil, err
		}
		dto.Candidates = cands
		out = append(out, dto)
	}
	return out, nil
}

func (s *Store) candidates(ctx context.Context, fbRowID int64) ([]CandidateDTO, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT gen_id, packet_idx, track_id, epoch_id, raw_seq, ext_seq,
		   transport_seq, cross_epoch, note
		 FROM feedback_candidates WHERE feedback_rowid = ? ORDER BY id`, fbRowID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []CandidateDTO
	for rows.Next() {
		var c CandidateDTO
		var raw, tseq int64
		var cross int
		if err := rows.Scan(&c.GenID, &c.PacketIndex, &c.TrackID, &c.EpochID,
			&raw, &c.ExtSeq, &tseq, &cross, &c.Note); err != nil {
			return nil, err
		}
		c.RawSeq, c.TransportSeq = uint16(raw), uint16(tseq)
		c.CrossEpoch = cross == 1
		out = append(out, c)
	}
	return out, rows.Err()
}

// FreezeDTO is one freeze with linked feedback positions.
type FreezeDTO struct {
	ID              int    `json:"id"`
	TrackID         int    `json:"track_id"`
	GenID           int    `json:"gen_id"`
	StartIndex      int    `json:"start_index"`
	EndIndex        int    `json:"end_index"`
	StartUS         int64  `json:"start_us"`
	EndUS           int64  `json:"end_us"`
	DurationUS      int64  `json:"duration_us"`
	Kind            string `json:"kind"`
	FirstMissingExt int64  `json:"first_missing_ext"`
	LastMissingExt  int64  `json:"last_missing_ext"`
	MissingCount    int    `json:"missing_count"`
	Feedback        []int  `json:"feedback_positions"`
}

// Freezes lists freeze intervals for one capture.
func (s *Store) Freezes(ctx context.Context, capID int64) ([]FreezeDTO, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, track_id, gen_id, start_idx, end_idx, start_us, end_us,
		   duration_us, kind, first_missing_ext, last_missing_ext, missing_count
		 FROM freezes WHERE capture_id = ? ORDER BY start_us, id`, capID)
	if err != nil {
		return nil, err
	}
	var out []FreezeDTO
	type rowIDHolder struct {
		dto   FreezeDTO
		rowID int
	}
	var holders []rowIDHolder
	for rows.Next() {
		var f FreezeDTO
		var rowID int
		if err := rows.Scan(&rowID, &f.TrackID, &f.GenID, &f.StartIndex, &f.EndIndex,
			&f.StartUS, &f.EndUS, &f.DurationUS, &f.Kind, &f.FirstMissingExt,
			&f.LastMissingExt, &f.MissingCount); err != nil {
			rows.Close()
			return nil, err
		}
		holders = append(holders, rowIDHolder{dto: f, rowID: rowID})
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	for _, h := range holders {
		h.dto.ID = h.rowID
		links, err := s.freezeLinks(ctx, h.rowID)
		if err != nil {
			return nil, err
		}
		h.dto.Feedback = links
		out = append(out, h.dto)
	}
	return out, nil
}

func (s *Store) freezeLinks(ctx context.Context, freezeRowID int) ([]int, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT feedback_pos FROM freeze_feedback WHERE freeze_rowid = ?
		 ORDER BY position_order`, freezeRowID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int
	for rows.Next() {
		var pos int
		if err := rows.Scan(&pos); err != nil {
			return nil, err
		}
		out = append(out, pos)
	}
	return out, rows.Err()
}

// SRDTO is one stored sender report mapping.
type SRDTO struct {
	PacketIndex  int    `json:"packet_index"`
	MsgIndex     int    `json:"msg_index"`
	GenID        int    `json:"gen_id"`
	SSRC         uint32 `json:"ssrc"`
	NTPMiddle32  uint32 `json:"ntp_middle32"`
	NTPUS        int64  `json:"ntp_us"`
	RTPTimestamp uint32 `json:"rtp_timestamp"`
	PacketCount  uint32 `json:"packet_count"`
	OctetCount   uint32 `json:"octet_count"`
}

// SenderReports lists SR mappings for one capture.
func (s *Store) SenderReports(ctx context.Context, capID int64) ([]SRDTO, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT packet_idx, msg_idx, gen_id, ssrc, ntp_middle32, ntp_us,
		   rtp_timestamp, packet_count, octet_count
		 FROM sender_reports WHERE capture_id = ? ORDER BY packet_idx`, capID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SRDTO
	for rows.Next() {
		var sr SRDTO
		var ssrc int64
		if err := rows.Scan(&sr.PacketIndex, &sr.MsgIndex, &sr.GenID, &ssrc,
			&sr.NTPMiddle32, &sr.NTPUS, &sr.RTPTimestamp, &sr.PacketCount,
			&sr.OctetCount); err != nil {
			return nil, err
		}
		sr.SSRC = uint32(ssrc)
		out = append(out, sr)
	}
	return out, rows.Err()
}

// RawPacket returns the original JSON record for one packet index, preserving
// the exact imported headers.
func (s *Store) RawPacket(ctx context.Context, capID int64, idx int) (string, error) {
	var raw string
	err := s.db.QueryRowContext(ctx,
		`SELECT raw_json FROM packets WHERE capture_id = ? AND idx = ?`,
		capID, idx).Scan(&raw)
	if err == sql.ErrNoRows {
		return "", ErrNotFound
	}
	return raw, err
}

func decodeUint16s(s string) []uint16 {
	var ints []int
	if err := json.Unmarshal([]byte(s), &ints); err != nil {
		return nil
	}
	out := make([]uint16, len(ints))
	for i, v := range ints {
		out[i] = uint16(v)
	}
	return out
}

// ErrNotFound signals a missing capture or packet.
var ErrNotFound = fmt.Errorf("not found")
