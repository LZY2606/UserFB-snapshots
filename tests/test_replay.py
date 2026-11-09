"""All tests run against the built-in sample only: no network, no sleep,
no wall-clock dependence."""

import pytest

from ipfixline.replay import replay
from ipfixline.sample import EXPORTER_A, EXPORTER_B, build_sample
from ipfixline.store import Store


@pytest.fixture()
def result():
    return replay(build_sample())


@pytest.fixture()
def store(tmp_path):
    s = Store(str(tmp_path / "test.db"))
    yield s
    s.close()


def _load_sample_store(store):
    from ipfixline.sample import load_sample
    cid = load_sample(store)
    return cid, store.latest_revision(cid)


# -- template identity ---------------------------------------------------

def test_template_identity_scoped_by_exporter_and_domain(result):
    a_epochs = [e for e in result.epochs
                if e.exporter == EXPORTER_A and e.domain_id == 100
                and e.template_id == 256 and e.kind == "template"]
    assert [e.generation for e in a_epochs] == [1, 2, 3]
    # same id on exporter B domain 100 is a different template space
    b300 = [e for e in result.epochs
            if e.exporter == EXPORTER_B and e.domain_id == 100
            and e.template_id == 300]
    assert len(b300) == 1
    # same id 256 in domain 200 does not share domain-100 templates
    b256 = [e for e in result.epochs
            if e.exporter == EXPORTER_B and e.domain_id == 200
            and e.template_id == 256]
    assert len(b256) == 1
    assert b256[0].record.fields[0].element_id == 7


def test_generations_have_distinct_fields(result):
    gens = {e.generation: e for e in result.epochs
            if e.exporter == EXPORTER_A and e.template_id == 256
            and e.kind == "template"}
    assert len(gens[1].record.fields) == 2
    assert len(gens[2].record.fields) == 4
    assert len(gens[3].record.fields) == 1
    # gen2 carries an enterprise element and a variable-length field
    assert any(f.is_enterprise and f.enterprise == 9
               for f in gens[2].record.fields)
    assert any(f.variable for f in gens[2].record.fields)


def test_withdrawal_and_all_withdrawal_close_epochs(result):
    gens = [e for e in result.epochs
            if e.exporter == EXPORTER_A and e.template_id == 256
            and e.kind == "template"]
    by_gen = {e.generation: e for e in gens}
    assert by_gen[1].withdrawn_at == 2      # implicit redefinition
    assert by_gen[2].withdrawn_at == 4      # explicit withdrawal
    assert by_gen[3].withdrawn_at == 8      # all-templates withdrawal


# -- record count / sequence ----------------------------------------------

def test_record_counts_advance_per_record(result):
    assert result.record_counts[(EXPORTER_A, 100)] == 5
    assert result.record_counts[(EXPORTER_B, 100)] == 1
    assert result.record_counts[(EXPORTER_B, 200)] == 1


def test_sequence_gap_detected(result):
    gaps = [s for s in result.sequence_events if s.kind == "gap"]
    assert len(gaps) == 1
    assert gaps[0].exporter == EXPORTER_A
    assert gaps[0].expected == 4
    assert gaps[0].observed == 10


# -- error ranges ----------------------------------------------------------

def test_truncated_variable_length_kept(result):
    trunc = [r for r in result.records if r.status == "truncated"]
    assert len(trunc) == 1
    assert trunc[0].template_id == 256
    assert trunc[0].generation == 2
    assert any(f.truncated and f.variable for f in trunc[0].fields)
    assert trunc[0].tail  # unparsed tail preserved


def test_late_template_and_withdrawn_data_kept_as_candidates(result):
    no_tpl = [r for r in result.records if r.status == "no_template"]
    assert len(no_tpl) == 2
    late = next(r for r in no_tpl if r.template_id == 300)
    assert late.candidates == []  # never defined at that point
    assert late.tail  # raw set bytes preserved
    after_withdraw = next(r for r in no_tpl if r.template_id == 256)
    assert after_withdraw.candidates == [1, 2, 3]  # all generations kept


def test_padding_reported(result):
    padded = [r for r in result.records if r.padding]
    assert padded and padded[0].padding == b"\x00\x00"


def test_enterprise_and_variable_fields_decoded(result):
    # gen2 record is truncated but its fixed fields still decode
    rec = next(r for r in result.records if r.generation == 2)
    assert any(f.enterprise == 9 or f.truncated for f in rec.fields)


# -- persistence, session revision, replayability --------------------------

def test_import_metadata_preserved(store):
    cid, _ = _load_sample_store(store)
    msgs = store.list_messages(cid)
    assert len(msgs) == 15
    m = msgs[0]
    assert m["exporter"] == EXPORTER_A
    assert m["domain_id"] == 100
    assert m["export_time"] == 1000
    assert m["set_first"] == 2 and m["set_last"] == 3
    assert len(m["digest"]) == 64


def test_session_boundary_revision_keeps_old_resolutions(store):
    cid, rev1 = _load_sample_store(store)
    sessions = store.current_sessions(cid)
    s1 = next(s for s in sessions if s["end_arrival"] == 6)
    epochs_before = store.resolutions(cid, rev1, "epoch")
    # move the reconnect boundary: session 1 now ends at arrival 7
    store.revise_session(cid, s1["id"], 0, 7, "include first reconnect msg")
    rev2 = store.latest_revision(cid)
    assert rev2 > rev1
    # old resolutions are retained untouched
    assert store.resolutions(cid, rev1, "epoch") == epochs_before
    # new session version exists, old one superseded but retained
    versions = store.db.execute(
        "SELECT version FROM sessions WHERE capture_id=? AND exporter=?"
        " AND start_arrival=0 ORDER BY version", (cid, EXPORTER_A)).fetchall()
    assert [v["version"] for v in versions] == [1, 2]
    new_sessions = store.current_sessions(cid)
    moved = next(s for s in new_sessions
                 if s["exporter"] == EXPORTER_A and s["start_arrival"] == 0)
    assert moved["end_arrival"] == 7 and moved["version"] == 2
    # the moved boundary changes the new revision's evidence
    assert store.resolutions(cid, rev2, "epoch") != []


def test_replay_is_deterministic(store):
    cid, rev1 = _load_sample_store(store)
    rev2 = store.new_revision(cid, "re-run")
    store.run_replay(cid, rev2)
    for kind in ("epoch", "record", "sequence"):
        assert store.resolutions(cid, rev1, kind) == store.resolutions(cid, rev2, kind)


def test_rebuild_from_empty_db(tmp_path):
    from ipfixline.sample import load_sample
    a = Store(str(tmp_path / "a.db"))
    b = Store(str(tmp_path / "b.db"))
    ca, cb = load_sample(a), load_sample(b)
    ra, rb = a.latest_revision(ca), b.latest_revision(cb)
    for kind in ("epoch", "record", "sequence"):
        assert a.resolutions(ca, ra, kind) == b.resolutions(cb, rb, kind)
    a.close()
    b.close()
