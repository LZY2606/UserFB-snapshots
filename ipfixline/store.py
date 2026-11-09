"""SQLite persistence layer.

Everything is derived from the messages table, so the whole database
can be rebuilt from an empty file by re-importing captures. Wall-clock
time is never used: a logical tick counter orders revisions so replays
are fully deterministic.
"""

from __future__ import annotations

import json
import sqlite3
from dataclasses import asdict

from .parser import parse_message
from .replay import ReplayResult, replay

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER);
CREATE TABLE IF NOT EXISTS captures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES captures(id),
    arrival INTEGER NOT NULL,
    exporter TEXT NOT NULL,
    domain_id INTEGER NOT NULL,
    export_time INTEGER NOT NULL,
    sequence INTEGER NOT NULL,
    set_first INTEGER,
    set_last INTEGER,
    digest TEXT NOT NULL,
    payload BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES captures(id),
    exporter TEXT NOT NULL,
    version INTEGER NOT NULL,
    start_arrival INTEGER NOT NULL,
    end_arrival INTEGER NOT NULL,
    note TEXT DEFAULT '',
    supersedes INTEGER
);
CREATE TABLE IF NOT EXISTS revisions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES captures(id),
    revision INTEGER NOT NULL,
    tick INTEGER NOT NULL,
    reason TEXT DEFAULT ''
);
CREATE TABLE IF NOT EXISTS resolutions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capture_id INTEGER NOT NULL REFERENCES captures(id),
    revision INTEGER NOT NULL,
    kind TEXT NOT NULL,
    arrival INTEGER NOT NULL,
    detail TEXT NOT NULL
);
"""


class Store:
    def __init__(self, path: str):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.execute("INSERT OR IGNORE INTO meta(key, value) VALUES ('tick', 0)")
        self.db.commit()

    def close(self):
        self.db.close()

    def _tick(self) -> int:
        row = self.db.execute("SELECT value FROM meta WHERE key='tick'").fetchone()
        tick = row["value"] + 1
        self.db.execute("UPDATE meta SET value=? WHERE key='tick'", (tick,))
        return tick

    def create_capture(self, name: str) -> int:
        cur = self.db.execute("INSERT INTO captures(name) VALUES (?)", (name,))
        self.db.commit()
        return cur.lastrowid

    def add_message(self, capture_id, arrival, exporter, raw) -> int:
        msg = parse_message(raw)
        set_ids = [s.set_id for s in msg.sets]
        cur = self.db.execute(
            "INSERT INTO messages (capture_id, arrival, exporter, domain_id,"
            " export_time, sequence, set_first, set_last, digest, payload)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)",
            (
                capture_id, arrival, exporter, msg.domain_id, msg.export_time,
                msg.sequence,
                min(set_ids) if set_ids else None,
                max(set_ids) if set_ids else None,
                msg.digest, raw,
            ),
        )
        self.db.commit()
        return cur.lastrowid

    def add_session(self, capture_id, exporter, version, start, end,
                    note="", supersedes=None) -> int:
        cur = self.db.execute(
            "INSERT INTO sessions (capture_id, exporter, version,"
            " start_arrival, end_arrival, note, supersedes)"
            " VALUES (?,?,?,?,?,?,?)",
            (capture_id, exporter, version, start, end, note, supersedes),
        )
        self.db.commit()
        return cur.lastrowid

    def current_sessions(self, capture_id):
        return self.db.execute(
            "SELECT s.* FROM sessions s"
            " JOIN (SELECT exporter, MAX(version) v FROM sessions"
            "       WHERE capture_id=? GROUP BY exporter) latest"
            "   ON latest.exporter=s.exporter AND latest.v=s.version"
            " WHERE s.capture_id=? ORDER BY s.exporter, s.start_arrival",
            (capture_id, capture_id),
        ).fetchall()

    def revise_session(self, capture_id, session_id, start, end, note="") -> int:
        old = self.db.execute(
            "SELECT * FROM sessions WHERE id=?", (session_id,)
        ).fetchone()
        if old is None:
            raise KeyError("session not found")
        new_id = self.add_session(
            capture_id, old["exporter"], old["version"] + 1, start, end,
            note or old["note"], supersedes=old["id"],
        )
        revision = self.new_revision(capture_id, "session revised")
        self.run_replay(capture_id, revision)
        return new_id

    def new_revision(self, capture_id, reason) -> int:
        row = self.db.execute(
            "SELECT MAX(revision) r FROM revisions WHERE capture_id=?",
            (capture_id,),
        ).fetchone()
        revision = (row["r"] or 0) + 1
        self.db.execute(
            "INSERT INTO revisions(capture_id, revision, tick, reason)"
            " VALUES (?,?,?,?)",
            (capture_id, revision, self._tick(), reason),
        )
        self.db.commit()
        return revision

    def latest_revision(self, capture_id):
        row = self.db.execute(
            "SELECT MAX(revision) r FROM revisions WHERE capture_id=?",
            (capture_id,),
        ).fetchone()
        return row["r"]

    def _session_messages(self, capture_id):
        sessions = self.current_sessions(capture_id)
        if not sessions:
            rows = self.db.execute(
                "SELECT arrival, exporter, payload FROM messages"
                " WHERE capture_id=? ORDER BY arrival",
                (capture_id,),
            ).fetchall()
            return [(r["arrival"], r["exporter"], r["payload"]) for r in rows]
        out = []
        for s in sessions:
            rows = self.db.execute(
                "SELECT arrival, exporter, payload FROM messages"
                " WHERE capture_id=? AND exporter=?"
                " AND arrival>=? AND arrival<=? ORDER BY arrival",
                (capture_id, s["exporter"], s["start_arrival"], s["end_arrival"]),
            ).fetchall()
            out.extend((r["arrival"], r["exporter"], r["payload"]) for r in rows)
        out.sort(key=lambda t: t[0])
        return out

    def run_replay(self, capture_id, revision=None) -> ReplayResult:
        if revision is None:
            revision = self.new_revision(capture_id, "replay")
        result = replay(self._session_messages(capture_id))
        rows = []
        for e in result.epochs:
            rows.append((capture_id, revision, "epoch", e.defined_at, json.dumps({
                "exporter": e.exporter, "domain_id": e.domain_id,
                "template_id": e.template_id, "kind": e.kind,
                "generation": e.generation, "defined_at": e.defined_at,
                "withdrawn_at": e.withdrawn_at,
                "fields": [f.describe() for f in e.record.fields],
            })))
        for r in result.records:
            rows.append((capture_id, revision, "record", r.arrival, json.dumps({
                "arrival": r.arrival,
                "set_index": r.set_index, "record_index": r.record_index,
                "template_id": r.template_id, "status": r.status,
                "generation": r.generation,
                "fields": [asdict(f) for f in r.fields],
                "padding": r.padding.hex(), "tail": r.tail.hex(),
                "candidates": r.candidates, "note": r.note,
            })))
        for s in result.sequence_events:
            rows.append((capture_id, revision, "sequence", s.arrival, json.dumps({
                "arrival": s.arrival,
                "exporter": s.exporter, "domain_id": s.domain_id,
                "expected": s.expected, "observed": s.observed, "kind": s.kind,
            })))
        self.db.executemany(
            "INSERT INTO resolutions(capture_id, revision, kind, arrival, detail)"
            " VALUES (?,?,?,?,?)",
            rows,
        )
        self.db.commit()
        return result

    def resolutions(self, capture_id, revision, kind):
        rows = self.db.execute(
            "SELECT arrival, detail FROM resolutions"
            " WHERE capture_id=? AND revision=? AND kind=? ORDER BY arrival, id",
            (capture_id, revision, kind),
        ).fetchall()
        return [json.loads(r["detail"]) for r in rows]

    def list_captures(self):
        return self.db.execute("SELECT * FROM captures ORDER BY id").fetchall()

    def list_messages(self, capture_id):
        return self.db.execute(
            "SELECT * FROM messages WHERE capture_id=? ORDER BY arrival",
            (capture_id,),
        ).fetchall()
