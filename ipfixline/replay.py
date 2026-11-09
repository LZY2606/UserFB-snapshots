"""Replay engine: template epoch resolution and data record decoding.

The engine walks messages in arrival order, builds template/options
template generations per (exporter, observation domain, template id),
and resolves every data record against the generation visible at that
point. Nothing is ever dropped: ambiguous, missing, truncated or
withdrawn situations are kept as candidates on the resolution.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .parser import (
    Message,
    SetBlock,
    TemplateRecord,
    parse_message,
)


@dataclass
class TemplateEpoch:
    exporter: str
    domain_id: int
    template_id: int
    kind: str
    generation: int
    defined_at: int  # arrival index of defining message
    withdrawn_at: int | None  # arrival index of withdrawal, if any
    record: TemplateRecord

    def visible_at(self, arrival: int) -> bool:
        if self.defined_at >= arrival:
            return False
        return self.withdrawn_at is None or self.withdrawn_at > arrival


@dataclass
class DecodedField:
    description: str
    length: int
    value_hex: str
    variable: bool = False
    enterprise: int | None = None
    truncated: bool = False


@dataclass
class RecordResolution:
    arrival: int
    set_index: int
    record_index: int
    template_id: int
    status: str  # resolved | no_template | ambiguous | truncated
    generation: int | None = None
    fields: list[DecodedField] = field(default_factory=list)
    padding: bytes = b""
    tail: bytes = b""
    candidates: list[int] = field(default_factory=list)  # generations kept
    note: str = ""


@dataclass
class SequenceEvent:
    arrival: int
    exporter: str
    domain_id: int
    expected: int
    observed: int
    kind: str  # "gap" | "ok"


@dataclass
class ReplayResult:
    epochs: list[TemplateEpoch]
    records: list[RecordResolution]
    sequence_events: list[SequenceEvent]
    record_counts: dict[tuple[str, int], int]  # (exporter, domain) -> records


def _decode_record(
    tpl: TemplateRecord, body: bytes, pos: int
) -> tuple[list[DecodedField], int, bool]:
    """Decode one record at pos. Returns (fields, consumed, truncated)."""
    out: list[DecodedField] = []
    start = pos
    truncated = False
    for spec in tpl.fields:
        if spec.variable:
            if pos >= len(body):
                truncated = True
                break
            first = body[pos]
            if first < 255:
                n, hdr = first, 1
            else:
                if pos + 3 > len(body):
                    truncated = True
                    break
                n = int.from_bytes(body[pos + 1 : pos + 3], "big")
                hdr = 3
            if pos + hdr + n > len(body):
                out.append(
                    DecodedField(
                        spec.describe(),
                        n,
                        body[pos:].hex(),
                        variable=True,
                        enterprise=spec.enterprise,
                        truncated=True,
                    )
                )
                truncated = True
                break
            out.append(
                DecodedField(
                    spec.describe(),
                    n,
                    body[pos + hdr : pos + hdr + n].hex(),
                    variable=True,
                    enterprise=spec.enterprise,
                )
            )
            pos += hdr + n
        else:
            if pos + spec.length > len(body):
                out.append(
                    DecodedField(
                        spec.describe(),
                        spec.length,
                        body[pos:].hex(),
                        enterprise=spec.enterprise,
                        truncated=True,
                    )
                )
                truncated = True
                break
            out.append(
                DecodedField(
                    spec.describe(),
                    spec.length,
                    body[pos : pos + spec.length].hex(),
                    enterprise=spec.enterprise,
                )
            )
            pos += spec.length
    return out, pos - start, truncated


def replay(messages: list[tuple[int, str, bytes]]) -> ReplayResult:
    """Replay (arrival, exporter, raw_bytes) tuples in arrival order."""
    epochs: list[TemplateEpoch] = []
    # live table: (exporter, domain, template_id, kind) -> current epoch
    live: dict[tuple[str, int, int, str], TemplateEpoch] = {}
    gen_counter: dict[tuple[str, int, int, str], int] = {}
    records: list[RecordResolution] = []
    seq_events: list[SequenceEvent] = []
    expected_seq: dict[tuple[str, int], int] = {}
    record_counts: dict[tuple[str, int], int] = {}

    def define(exporter, domain, arrival, tpl: TemplateRecord):
        key = (exporter, domain, tpl.template_id, tpl.kind)
        old = live.get(key)
        if old is not None and old.withdrawn_at is None:
            old.withdrawn_at = arrival  # implicit redefinition ends old epoch
        gen = gen_counter.get(key, 0) + 1
        gen_counter[key] = gen
        epoch = TemplateEpoch(
            exporter, domain, tpl.template_id, tpl.kind, gen, arrival, None, tpl
        )
        epochs.append(epoch)
        live[key] = epoch

    def withdraw(exporter, domain, arrival, template_id, kind):
        key = (exporter, domain, template_id, kind)
        old = live.get(key)
        if old is not None and old.withdrawn_at is None:
            old.withdrawn_at = arrival

    def withdraw_all(exporter, domain, arrival, kind):
        for key, epoch in live.items():
            if (
                key[0] == exporter
                and key[1] == domain
                and key[3] == kind
                and epoch.withdrawn_at is None
            ):
                epoch.withdrawn_at = arrival

    for arrival, exporter, raw in messages:
        msg: Message = parse_message(raw)
        domain = msg.domain_id
        data_records_in_msg = 0
        for set_index, block in enumerate(msg.sets):
            if block.set_id == 2 or block.set_id == 3:
                kind = "template" if block.set_id == 2 else "options"
                if block.all_withdrawal:
                    withdraw_all(exporter, domain, arrival, kind)
                for tpl in block.templates:
                    if tpl.withdrawal:
                        withdraw(exporter, domain, arrival, tpl.template_id, kind)
                    else:
                        define(exporter, domain, arrival, tpl)
                continue
            if not block.is_data:
                continue
            tid = block.set_id
            epoch = live.get((exporter, domain, tid, "template")) or live.get(
                (exporter, domain, tid, "options")
            )
            visible = epoch if (epoch and epoch.visible_at(arrival)) else None
            if visible is None:
                # keep every known generation as a candidate; never drop
                known = [
                    e.generation
                    for e in epochs
                    if e.exporter == exporter
                    and e.domain_id == domain
                    and e.template_id == tid
                ]
                note = (
                    "template never defined"
                    if not known
                    else "no generation visible at this point"
                )
                records.append(
                    RecordResolution(
                        arrival=arrival,
                        set_index=set_index,
                        record_index=0,
                        template_id=tid,
                        status="no_template",
                        tail=block.raw,
                        candidates=known,
                        note=note,
                    )
                )
                continue
            tpl = visible.record
            min_size = max(tpl.min_record_size(), 1)
            pos = 0
            rec_idx = 0
            body = block.raw
            while len(body) - pos >= min_size:
                fields, consumed, truncated = _decode_record(tpl, body, pos)
                if consumed == 0:
                    break
                status = "truncated" if truncated else "resolved"
                records.append(
                    RecordResolution(
                        arrival=arrival,
                        set_index=set_index,
                        record_index=rec_idx,
                        template_id=tid,
                        status=status,
                        generation=visible.generation,
                        fields=fields,
                        tail=body[pos + consumed :] if truncated else b"",
                        candidates=[visible.generation],
                        note="variable-length field truncated" if truncated else "",
                    )
                )
                data_records_in_msg += 1
                rec_idx += 1
                pos += consumed
                if truncated:
                    break
            leftover = body[pos:]
            if leftover:
                if rec_idx > 0 and not records[-1].tail:
                    # bytes smaller than one record: padding
                    records[-1].padding = leftover
                elif rec_idx == 0:
                    records.append(
                        RecordResolution(
                            arrival=arrival,
                            set_index=set_index,
                            record_index=0,
                            template_id=tid,
                            status="resolved",
                            generation=visible.generation,
                            padding=leftover,
                            candidates=[visible.generation],
                            note="padding only",
                        )
                    )
        # sequence numbers count data records per (exporter, domain)
        skey = (exporter, domain)
        observed = msg.sequence
        if skey in expected_seq:
            exp = expected_seq[skey]
            kind = "ok" if observed == exp else "gap"
            seq_events.append(SequenceEvent(arrival, exporter, domain, exp, observed, kind))
        if data_records_in_msg:
            expected_seq[skey] = (observed + data_records_in_msg) % (2**32)
            record_counts[skey] = record_counts.get(skey, 0) + data_records_in_msg
        else:
            expected_seq.setdefault(skey, observed)
    return ReplayResult(epochs, records, seq_events, record_counts)
