"""Built-in sample capture.

Two exporters, three generations of template id 256 on exporter A,
template withdrawal, all-templates withdrawal, a reconnect with id
reuse, a sequence gap, a late template (data before definition) and a
truncated variable-length field. All bytes are synthetic; nothing
touches the network.
"""

from __future__ import annotations

import struct

EXPORTER_A = "10.0.0.1:4739"
EXPORTER_B = "10.0.0.2:4739"


def _msg(export_time: int, seq: int, domain: int, *sets: bytes) -> bytes:
    body = b"".join(sets)
    return struct.pack("!HHIII", 10, 16 + len(body), export_time, seq, domain) + body


def _set(set_id: int, payload: bytes) -> bytes:
    return struct.pack("!HH", set_id, 4 + len(payload)) + payload


def _field(eid: int, length: int, enterprise: int | None = None) -> bytes:
    if enterprise is not None:
        return struct.pack("!HHI", eid | 0x8000, length, enterprise)
    return struct.pack("!HH", eid, length)


def _tpl(tid: int, fields: list[bytes]) -> bytes:
    return struct.pack("!HH", tid, len(fields)) + b"".join(fields)


def _opts_tpl(tid: int, scope: int, fields: list[bytes]) -> bytes:
    return struct.pack("!HHH", tid, len(fields), scope) + b"".join(fields)


def _withdraw(tid: int) -> bytes:
    return struct.pack("!HH", tid, 0)


GEN1_FIELDS = [_field(8, 4), _field(1, 8)]                      # ipv4 src, octets
GEN2_FIELDS = [_field(8, 4), _field(1, 8), _field(95, 0xFFFF),  # + app name (var)
               _field(1000, 4, enterprise=9)]                   # + enterprise field
GEN3_FIELDS = [_field(8, 4)]                                    # id reuse: minimal


def build_sample() -> list[tuple[int, str, bytes]]:
    """Return (arrival, exporter, raw_message) tuples in arrival order."""
    msgs: list[tuple[int, str, bytes]] = []

    def add(arrival, exporter, raw):
        msgs.append((arrival, exporter, raw))

    # --- exporter A, domain 100, session 1 (arrivals 0..6) ---
    add(0, EXPORTER_A, _msg(1000, 0, 100,
        _set(2, _tpl(256, GEN1_FIELDS)),
        _set(3, _opts_tpl(257, 1, [_field(8, 4), _field(95, 0xFFFF)])),
    ))
    rec1 = bytes([192, 0, 2, 1]) + struct.pack("!Q", 500)
    rec2 = bytes([192, 0, 2, 2]) + struct.pack("!Q", 600)
    add(1, EXPORTER_A, _msg(1001, 0, 100, _set(256, rec1 + rec2 + b"\x00\x00")))
    add(2, EXPORTER_A, _msg(1002, 2, 100, _set(2, _tpl(256, GEN2_FIELDS))))
    # variable-length field declares 20 bytes but the set ends early
    trunc = bytes([198, 51, 100, 7]) + struct.pack("!Q", 42) + bytes([20]) + b"abc" + struct.pack("!I", 77)
    add(3, EXPORTER_A, _msg(1003, 2, 100, _set(256, trunc)))
    add(4, EXPORTER_A, _msg(1004, 3, 100, _set(2, _withdraw(256))))
    add(5, EXPORTER_A, _msg(1005, 3, 100, _set(2, _tpl(256, GEN3_FIELDS))))
    add(6, EXPORTER_A, _msg(1006, 3, 100, _set(256, bytes([203, 0, 113, 9]))))
    # --- exporter A, session 2 after reconnect (arrivals 7..9) ---
    add(7, EXPORTER_A, _msg(2000, 10, 100, _set(256, bytes([203, 0, 113, 10]))))  # gap: 4 -> 10
    add(8, EXPORTER_A, _msg(2001, 11, 100, _set(2, b"")))  # withdraw all templates
    add(9, EXPORTER_A, _msg(2002, 11, 100, _set(256, bytes([203, 0, 113, 11]))))  # no live template
    # --- exporter B, domain 100: late template ---
    add(10, EXPORTER_B, _msg(3000, 0, 100, _set(300, struct.pack("!II", 1, 2))))
    add(11, EXPORTER_B, _msg(3001, 0, 100, _set(2, _tpl(300, [_field(8, 4), _field(4, 4)]))))
    add(12, EXPORTER_B, _msg(3002, 0, 100, _set(300, struct.pack("!II", 3, 4))))
    # --- exporter B, domain 200: separate template space, same id 256 ---
    add(13, EXPORTER_B, _msg(3003, 0, 200, _set(2, _tpl(256, [_field(7, 2)]))))
    add(14, EXPORTER_B, _msg(3004, 0, 200, _set(256, struct.pack("!H", 443))))
    return msgs


# session boundaries as (exporter, start_arrival, end_arrival, note)
SAMPLE_SESSIONS = [
    (EXPORTER_A, 0, 6, "initial session"),
    (EXPORTER_A, 7, 9, "after reconnect"),
    (EXPORTER_B, 10, 14, "single session"),
]


def load_sample(store) -> int:
    capture_id = store.create_capture("built-in sample")
    for arrival, exporter, raw in build_sample():
        store.add_message(capture_id, arrival, exporter, raw)
    for exporter, start, end, note in SAMPLE_SESSIONS:
        store.add_session(capture_id, exporter, 1, start, end, note)
    store.run_replay(capture_id)
    return capture_id
