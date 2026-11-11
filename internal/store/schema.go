// Package store persists imported captures and their analysis in SQLite.
//
// The schema keeps the original packet index everywhere: every derived row
// (RTP assignment, RTCP message, feedback candidate, freeze link) carries the
// index of the datagram it came from so a review can always return to the raw
// evidence.
package store

import (
	"database/sql"
	"fmt"

	_ "modernc.org/sqlite"
)

// Store wraps an open SQLite database.
type Store struct {
	db *sql.DB
}

// schema is applied idempotently on open.
const schema = `
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;

CREATE TABLE IF NOT EXISTS captures (
	id            INTEGER PRIMARY KEY AUTOINCREMENT,
	source        TEXT NOT NULL,
	imported_at_us INTEGER NOT NULL,
	packet_count  INTEGER NOT NULL,
	time_min_us   INTEGER NOT NULL,
	time_max_us   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS packets (
	capture_id  INTEGER NOT NULL REFERENCES captures(id),
	idx         INTEGER NOT NULL,
	time_us     INTEGER NOT NULL,
	direction   TEXT NOT NULL,
	kind        TEXT NOT NULL,
	raw_json    TEXT NOT NULL,
	PRIMARY KEY (capture_id, idx)
);

CREATE TABLE IF NOT EXISTS tracks (
	capture_id   INTEGER NOT NULL REFERENCES captures(id),
	id           INTEGER NOT NULL,
	mid          TEXT NOT NULL,
	rid          TEXT NOT NULL,
	payload_type INTEGER NOT NULL,
	synthetic    INTEGER NOT NULL,
	PRIMARY KEY (capture_id, id)
);

CREATE TABLE IF NOT EXISTS generations (
	capture_id    INTEGER NOT NULL REFERENCES captures(id),
	id            INTEGER NOT NULL,
	track_id      INTEGER NOT NULL,
	ssrc          INTEGER NOT NULL,
	generation_no INTEGER NOT NULL,
	start_idx     INTEGER NOT NULL,
	end_idx       INTEGER NOT NULL,
	start_us      INTEGER NOT NULL,
	end_us        INTEGER NOT NULL,
	open_reason   TEXT NOT NULL,
	close_reason  TEXT NOT NULL,
	payload_type  INTEGER NOT NULL,
	PRIMARY KEY (capture_id, id)
);

CREATE TABLE IF NOT EXISTS transport_epochs (
	capture_id INTEGER NOT NULL REFERENCES captures(id),
	id         INTEGER NOT NULL,
	start_idx  INTEGER NOT NULL,
	end_idx    INTEGER NOT NULL,
	start_us   INTEGER NOT NULL,
	end_us     INTEGER NOT NULL,
	reason     TEXT NOT NULL,
	PRIMARY KEY (capture_id, id)
);

CREATE TABLE IF NOT EXISTS rtp_packets (
	capture_id    INTEGER NOT NULL REFERENCES captures(id),
	packet_idx    INTEGER NOT NULL,
	time_us       INTEGER NOT NULL,
	ssrc          INTEGER NOT NULL,
	payload_type  INTEGER NOT NULL,
	seq           INTEGER NOT NULL,
	ext_seq       INTEGER NOT NULL,
	rtp_timestamp INTEGER NOT NULL,
	marker        INTEGER NOT NULL,
	mid           TEXT NOT NULL,
	rid           TEXT NOT NULL,
	transport_seq INTEGER,
	track_id      INTEGER NOT NULL,
	gen_id        INTEGER NOT NULL,
	epoch_id      INTEGER NOT NULL,
	PRIMARY KEY (capture_id, packet_idx)
);

CREATE TABLE IF NOT EXISTS rtcp_messages (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	capture_id  INTEGER NOT NULL REFERENCES captures(id),
	packet_idx  INTEGER NOT NULL,
	msg_idx     INTEGER NOT NULL,
	fmt         INTEGER NOT NULL,
	packet_type INTEGER NOT NULL,
	sender_ssrc INTEGER NOT NULL,
	media_ssrc  INTEGER NOT NULL,
	kind        TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS rtcp_nacks (
	id        INTEGER PRIMARY KEY AUTOINCREMENT,
	msg_rowid INTEGER NOT NULL REFERENCES rtcp_messages(id),
	start_seq INTEGER NOT NULL,
	end_seq   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS rtcp_report_blocks (
	id              INTEGER PRIMARY KEY AUTOINCREMENT,
	msg_rowid       INTEGER NOT NULL REFERENCES rtcp_messages(id),
	ssrc            INTEGER NOT NULL,
	fraction_lost   INTEGER NOT NULL,
	cumulative_lost INTEGER NOT NULL,
	highest_seq     INTEGER NOT NULL,
	jitter          INTEGER NOT NULL,
	lsr             INTEGER NOT NULL,
	dlsr            INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS rtcp_sender_info (
	msg_rowid     INTEGER PRIMARY KEY REFERENCES rtcp_messages(id),
	ntp_msw       INTEGER NOT NULL,
	ntp_lsw       INTEGER NOT NULL,
	rtp_timestamp INTEGER NOT NULL,
	packet_count  INTEGER NOT NULL,
	octet_count   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS feedback_events (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	capture_id  INTEGER NOT NULL REFERENCES captures(id),
	position    INTEGER NOT NULL,
	packet_idx  INTEGER NOT NULL,
	msg_idx     INTEGER NOT NULL,
	kind        TEXT NOT NULL,
	sender_ssrc INTEGER NOT NULL,
	media_ssrc  INTEGER NOT NULL,
	epoch_id    INTEGER NOT NULL,
	status      TEXT NOT NULL,
	note        TEXT NOT NULL,
	nack_seqs   TEXT NOT NULL,
	twcc_seqs   TEXT NOT NULL,
	rtt_us      TEXT NOT NULL,
	rtt_notes   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS feedback_candidates (
	id            INTEGER PRIMARY KEY AUTOINCREMENT,
	feedback_rowid INTEGER NOT NULL REFERENCES feedback_events(id),
	gen_id        INTEGER NOT NULL,
	packet_idx    INTEGER NOT NULL,
	track_id      INTEGER NOT NULL,
	epoch_id      INTEGER NOT NULL,
	raw_seq       INTEGER NOT NULL,
	ext_seq       INTEGER NOT NULL,
	transport_seq INTEGER NOT NULL,
	cross_epoch   INTEGER NOT NULL,
	note          TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sender_reports (
	id            INTEGER PRIMARY KEY AUTOINCREMENT,
	capture_id    INTEGER NOT NULL REFERENCES captures(id),
	packet_idx    INTEGER NOT NULL,
	msg_idx       INTEGER NOT NULL,
	gen_id        INTEGER NOT NULL,
	ssrc          INTEGER NOT NULL,
	ntp_msw       INTEGER NOT NULL,
	ntp_lsw       INTEGER NOT NULL,
	ntp_middle32  INTEGER NOT NULL,
	ntp_us        INTEGER NOT NULL,
	rtp_timestamp INTEGER NOT NULL,
	packet_count  INTEGER NOT NULL,
	octet_count   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS freezes (
	id               INTEGER PRIMARY KEY AUTOINCREMENT,
	capture_id       INTEGER NOT NULL REFERENCES captures(id),
	track_id         INTEGER NOT NULL,
	gen_id           INTEGER NOT NULL,
	start_idx        INTEGER NOT NULL,
	end_idx          INTEGER NOT NULL,
	start_us         INTEGER NOT NULL,
	end_us           INTEGER NOT NULL,
	duration_us      INTEGER NOT NULL,
	kind             TEXT NOT NULL,
	first_missing_ext INTEGER NOT NULL,
	last_missing_ext INTEGER NOT NULL,
	missing_count    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS freeze_feedback (
	freeze_rowid   INTEGER NOT NULL REFERENCES freezes(id),
	feedback_pos   INTEGER NOT NULL,
	position_order INTEGER NOT NULL,
	PRIMARY KEY (freeze_rowid, feedback_pos)
);

CREATE INDEX IF NOT EXISTS idx_rtp_gen ON rtp_packets(capture_id, gen_id);
CREATE INDEX IF NOT EXISTS idx_rtp_ssrc ON rtp_packets(capture_id, ssrc);
CREATE INDEX IF NOT EXISTS idx_rtp_tseq ON rtp_packets(capture_id, epoch_id, transport_seq);
CREATE INDEX IF NOT EXISTS idx_fb_status ON feedback_events(capture_id, status);
CREATE INDEX IF NOT EXISTS idx_fb_kind ON feedback_events(capture_id, kind);
CREATE INDEX IF NOT EXISTS idx_gen_ssrc ON generations(capture_id, ssrc);
`

// Open opens (creating if needed) the SQLite database at path and applies the
// schema. The database is created in a fully offline, local file mode.
func Open(path string) (*Store, error) {
	// _pragma in DSN keeps connection settings attached to every conn.
	dsn := fmt.Sprintf("file:%s?_pragma=busy_timeout(5000)&_pragma=foreign_keys(ON)", path)
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// WAL allows one writer plus concurrent readers. Keep a small pool rather
	// than serializing everything: nested read queries (an outer cursor plus
	// its child-table lookups) must not deadlock waiting for one connection.
	// busy_timeout (in the DSN and schema) makes writers wait their turn.
	db.SetMaxOpenConns(6)
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, err
	}
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, fmt.Errorf("apply schema: %w", err)
	}
	return &Store{db: db}, nil
}

// Close releases the database handle.
func (s *Store) Close() error { return s.db.Close() }
