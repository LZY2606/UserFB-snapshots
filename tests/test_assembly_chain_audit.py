"""Reproduction cases for the NMEA -> multipart assembly -> AIS decode audit.

Each test can be selected on its own, for example::

    .venv/bin/python -m pytest tests/test_assembly_chain_audit.py::TestAssemblyChainAudit::test_bad_checksum_is_emitted_as_invalid_without_diagnostic -q

The tests pin down the *current* behavior (including the defects described in
ANALYSIS.md) so that every failure site is reproducible from this commit.
"""
import pytest

from pyais.bit_vector import bit_vector
from pyais.exceptions import UnknownMessageException
from pyais.messages import NMEASentenceFactory
from pyais.queue import NMEAQueue
from pyais.stream import IterMessages, TagBlockQueue
from pyais.util import compute_checksum


def make_sentence(frag_cnt, frag_num, seq_id, channel, payload, fill_bits=0,
                  talker=b"AI"):
    """Build a syntactically valid AIVDM sentence with a correct checksum."""
    seq_field = b"" if seq_id is None else str(seq_id).encode()
    body = b",".join([
        b"!" + talker + b"VDM",
        str(frag_cnt).encode(),
        str(frag_num).encode(),
        seq_field,
        channel.encode(),
        payload,
        str(fill_bits).encode(),
    ])
    return body + b"*%02X" % compute_checksum(body)


# A real two-part type 5 message (seq id 4, channel A).
PART_1 = b"!AIVDM,2,1,4,A,55O0W7`00001L@gCWGA2uItLth@DqtL5@F22220j1h742t0Ht0000000,0*08"
PART_2 = b"!AIVDM,2,2,4,A,000000000000000,2*20"


class TestAssemblyChainAudit:

    def test_interleaved_channels_share_buffer_slot(self):
        """Two sources interleave on channels A/B with the same seq id 4.

        pyais/stream.py:184 includes the channel in the buffer slot, so the two
        feeds stay separate here; this case documents the channel dimension that
        the same-seq-id case below relies on to tell interleaved feeds apart.
        """
        other_1 = make_sentence(2, 1, 4, "B", b"55O0W7BBBBBBBBBB")
        other_2 = make_sentence(2, 2, 4, "B", b"000000000000001", fill_bits=2)
        decoded = list(IterMessages([PART_1, other_1, other_2, PART_2]))
        assert len(decoded) == 2
        assert {m.channel for m in decoded} == {"A", "B"}

    def test_same_seq_id_different_sources_get_stitched_together(self):
        """Same slot tuple from two sources overwrites each other's fragments.

        Boundary #1 (ANALYSIS.md): pyais/stream.py:184-190 has no source
        identity in the slot key. A second source reusing seq id 4 / channel A
        overwrites fragment 1 of the first source; when its fragment 2 arrives
        the stream emits a stitched message and the first source is lost.
        """
        foreign_1 = make_sentence(2, 1, 4, "A", b"55O0W7`00001FOREIGNFRAG1")
        foreign_2 = make_sentence(2, 2, 4, "A", b"000000000000001", fill_bits=2)

        outputs = list(IterMessages([PART_1, foreign_1, foreign_2]))
        assert len(outputs) == 1
        assembled = outputs[0]
        assert assembled.payload == b"55O0W7`00001FOREIGNFRAG1000000000000001"
        # The original source's fragment 1 was overwritten; PART_2 is never
        # consumed, so its raw line is absent from the only output.
        assert PART_2 not in assembled.raw

    def test_empty_seq_id_collides_in_nmea_queue(self):
        """Empty seq id is normalized to -1 and collides across sources.

        Boundary #1 on the queue path: pyais/queue.py:60-70 keys the buffer on
        (seq_id, channel) only. Two multi-part sentences with an empty sequence
        id share slot (-1, 'A'); fragment 1 of source A is overwritten by
        source B and source A's completed message is silently dropped.
        """
        a1 = make_sentence(2, 1, None, "A", b"55O0W7AAAA")
        a2 = make_sentence(2, 2, None, "A", b"0000000000")
        b1 = make_sentence(2, 1, None, "A", b"55O0W7BBBB")
        b2 = make_sentence(2, 2, None, "A", b"0000000001")

        q = NMEAQueue()
        emitted = []
        for line in (a1, b1, b2, a2):
            q.put_line(line)
            msg = q.get_or_none()
            if msg is not None:
                emitted.append(msg)

        assert len(emitted) == 1
        assert emitted[0].payload == b"55O0W7BBBB0000000001"

    def test_missing_fragment_is_dropped_silently_at_stream_end(self):
        """An incomplete fragment buffer vanishes when the stream ends.

        Boundary #3 (ANALYSIS.md): pyais/stream.py:155-198 only deletes a slot
        once all fragments arrived (stream.py:198) and never flushes or reports
        leftovers when the generator finishes. No message, no exception.
        """
        outputs = list(IterMessages([PART_1]))  # fragment 2 never arrives
        assert outputs == []

    def test_bad_checksum_is_emitted_as_invalid_without_diagnostic(self):
        """Corrupted sentences flow through; the stream does not flag them.

        Boundary #2 (ANALYSIS.md): the checksum is validated lazily in
        pyais/messages.py:561-565 but the assembly loop at pyais/stream.py:167
        only swallows parse exceptions and never checks is_valid, so a
        bad-checksum sentence is yielded as if it were fine.
        """
        good = b"!AIVDM,1,1,,B,91b55wi;hbOS@OdQAC062Ch2089h,0*30"
        bad = good.replace(b"*30", b"*00")
        [msg] = list(IterMessages([bad]))
        assert msg.is_valid is False
        assert msg.checksum != compute_checksum(msg.raw)

    def test_unknown_message_type_streams_but_fails_at_decode(self):
        """Unknown AIS type id passes assembly and is rejected by dispatch.

        Boundary #2: pyais/messages.py:753-766 dispatches on MSG_CLASS and
        raises UnknownMessageException for unsupported ids. The streaming layer
        never decodes, so the bad type is only discovered on explicit .decode().
        """
        # First six bits 111111 (ais id 63) armor to payload char 'w'.
        line = make_sentence(1, 1, None, "B", b"w5M67FC000G")
        [streamed] = list(IterMessages([line]))
        assert streamed.ais_id == 63
        with pytest.raises(UnknownMessageException):
            streamed.decode()

    def test_sixbit_out_of_alphabet_byte_is_silently_mapped_to_zero(self):
        """Non-armored payload characters decode as zero instead of erroring.

        Boundary #2 at the six-bit layer: pyais/bit_vector.py:11-12 maps every
        byte outside ASCII 48..119 to value 0. A corrupted payload char (here
        '!', ordinal 33) is therefore indistinguishable from a real '0' and no
        diagnostic is produced.
        """
        assert bit_vector(b"!").get(0, 6) == 0
        assert bit_vector(b"0").get(0, 6) == 0

    def test_tag_block_group_orphan_fragment_is_dropped_silently(self):
        """A tag-block group whose first sentence is missing is ignored.

        Boundary #3 in TagBlockQueue: pyais/stream.py:93-95 returns without any
        signal when a group id is unknown, and stream end never reclaims groups,
        so the orphan sentence and its missing-part condition are invisible.
        """
        tbq = TagBlockQueue()
        orphan = NMEASentenceFactory.produce(
            b"\\g:2-2-42*00\\!AIVDM,1,1,,B,91b55wi;hbOS@OdQAC062Ch2089h,0*30"
        )
        orphan.tag_block.init()
        tbq.put_sentence(orphan)
        assert tbq.qsize() == 0
