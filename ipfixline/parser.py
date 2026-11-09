"""IPFIX (RFC 7011) message parsing.

Parses raw message bytes into headers, sets, template records and data
sets. No network access: callers feed bytes obtained from their own
capture sources.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

TEMPLATE_SET_ID = 2
OPTIONS_TEMPLATE_SET_ID = 3
VARIABLE_LENGTH = 0xFFFF
ENTERPRISE_BIT = 0x8000


class ParseError(ValueError):
    pass


@dataclass(frozen=True)
class FieldSpec:
    element_id: int
    length: int  # VARIABLE_LENGTH means variable-length
    enterprise: int | None = None

    @property
    def variable(self) -> bool:
        return self.length == VARIABLE_LENGTH

    @property
    def is_enterprise(self) -> bool:
        return self.enterprise is not None

    def describe(self) -> str:
        base = f"element {self.element_id}"
        if self.is_enterprise:
            base += f" (enterprise {self.enterprise})"
        if self.variable:
            base += " variable-length"
        else:
            base += f" len={self.length}"
        return base


@dataclass(frozen=True)
class TemplateRecord:
    template_id: int
    fields: tuple[FieldSpec, ...]
    kind: str  # "template" or "options"
    scope_count: int = 0
    withdrawal: bool = False

    def min_record_size(self) -> int:
        size = 0
        for f in self.fields:
            size += 1 if f.variable else f.length
        return size


@dataclass
class SetBlock:
    set_id: int
    offset: int  # offset of set header within the message
    length: int  # total set length including set header
    raw: bytes  # set body (after the 4-byte set header)
    templates: list[TemplateRecord] = field(default_factory=list)
    all_withdrawal: bool = False  # empty template/options set

    @property
    def is_data(self) -> bool:
        return self.set_id >= 256


@dataclass
class Message:
    version: int
    length: int
    export_time: int
    sequence: int
    domain_id: int
    sets: list[SetBlock]
    raw: bytes
    digest: str


def _parse_field_spec(buf: bytes, pos: int) -> tuple[FieldSpec, int]:
    if pos + 4 > len(buf):
        raise ParseError("truncated field specifier")
    raw_id, length = struct.unpack_from("!HH", buf, pos)
    pos += 4
    enterprise = None
    if raw_id & ENTERPRISE_BIT:
        if pos + 4 > len(buf):
            raise ParseError("truncated enterprise number")
        (enterprise,) = struct.unpack_from("!I", buf, pos)
        pos += 4
    return FieldSpec(raw_id & ~ENTERPRISE_BIT, length, enterprise), pos


def _parse_template_set(block: SetBlock, kind: str) -> None:
    body = block.raw
    if len(body) == 0:
        block.all_withdrawal = True
        return
    pos = 0
    while pos < len(body):
        if kind == "template":
            if pos + 4 > len(body):
                raise ParseError("truncated template record header")
            tid, count = struct.unpack_from("!HH", body, pos)
            pos += 4
            scope = 0
        else:
            if pos + 6 > len(body):
                raise ParseError("truncated options template record header")
            tid, count, scope = struct.unpack_from("!HHH", body, pos)
            pos += 6
        if count == 0:
            block.templates.append(
                TemplateRecord(tid, (), kind, scope, withdrawal=True)
            )
            continue
        fields: list[FieldSpec] = []
        for _ in range(count):
            spec, pos = _parse_field_spec(body, pos)
            fields.append(spec)
        block.templates.append(TemplateRecord(tid, tuple(fields), kind, scope))


def parse_message(raw: bytes) -> Message:
    if len(raw) < 16:
        raise ParseError("message shorter than 16-byte header")
    version, length, export_time, sequence, domain_id = struct.unpack_from(
        "!HHIII", raw, 0
    )
    if version != 10:
        raise ParseError(f"not an IPFIX message (version={version})")
    if length > len(raw):
        raise ParseError("declared length exceeds available bytes")
    raw = raw[:length]
    sets: list[SetBlock] = []
    pos = 16
    while pos + 4 <= length:
        set_id, set_len = struct.unpack_from("!HH", raw, pos)
        if set_len < 4 or pos + set_len > length:
            raise ParseError(f"bad set length {set_len} at offset {pos}")
        body = raw[pos + 4 : pos + set_len]
        block = SetBlock(set_id, pos, set_len, body)
        if set_id == TEMPLATE_SET_ID:
            _parse_template_set(block, "template")
        elif set_id == OPTIONS_TEMPLATE_SET_ID:
            _parse_template_set(block, "options")
        sets.append(block)
        pos += set_len
    digest = hashlib.sha256(raw).hexdigest()
    return Message(version, length, export_time, sequence, domain_id, sets, raw, digest)
