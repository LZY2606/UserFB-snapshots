# Copyright (C) 2026 jsonpickle contributors
# All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.

"""Object-graph round-trip tests.

The risky part of jsonpickle is not the values -- it is the *reference
topology*: two fields that used to point at the same object may come back as
two independent copies, and circular references may be wired to the wrong
place.  The tests in this module build small object graphs in code and, after
encode/decode, assert all three of:

1. every leaf value survives,
2. every object keeps its type,
3. identity ("who is the same object as whom") is preserved: every alias that
   pointed at one object in the original graph resolves to one object in the
   restored graph, and distinct objects stay distinct.

The option space (keys / refs / unpicklable / safe / classes) is exercised as
an explicit matrix, and the failure messages carry the graph construction
code, the options in effect, the intermediate JSON, a rendering of the
restored graph, and the path of the first alias or type mismatch.

Nothing in here opens a network connection or runs an external command.
"""

import dataclasses
import itertools
import json
import warnings
from typing import ClassVar

import pytest

import jsonpickle
from jsonpickle.handlers import BaseHandler, register, unregister
from jsonpickle.pickler import Pickler
from jsonpickle.unpickler import Unpickler

# ---------------------------------------------------------------------------
# Graph node types
# ---------------------------------------------------------------------------


class PlainNode:
    """Regular __dict__ object participating in aliases and cycles."""

    def __init__(self, label, peer=None, note=None):
        self.label = label
        self.peer = peer
        self.note = note

    def __repr__(self):
        return f"PlainNode(label={self.label!r})"


class SlotNode:
    """Object whose state lives entirely in __slots__."""

    __slots__ = ("label", "note", "peer")

    def __init__(self, label, peer=None, note=None):
        self.label = label
        self.peer = peer
        if note is not None:
            self.note = note

    def __repr__(self):
        return f"SlotNode(label={self.label!r})"


@dataclasses.dataclass
class DataNode:
    """Dataclass with a mutable field used to build cycles."""

    label: str
    peer: object = None


@dataclasses.dataclass(slots=True)
class SlottedDataNode:
    """Dataclass that stores its fields in slots."""

    label: str
    peer: object = None


class ReduceNode:
    """Object reconstructed through the __reduce__ protocol."""

    def __init__(self, label, payload=None):
        self.label = label
        self.payload = payload

    def __reduce__(self):
        return (
            ReduceNode,
            (self.label,),
            {"payload": self.payload},
            None,
            None,
        )

    def __repr__(self):
        return f"ReduceNode(label={self.label!r})"


class StateNode:
    """Object with explicit __getstate__/__setstate__."""

    def __init__(self, label, peer=None):
        self.label = label
        self.peer = peer

    def __getstate__(self):
        return {"label": self.label, "peer": self.peer}

    def __setstate__(self, state):
        self.label = state["label"]
        self.peer = state["peer"]

    def __repr__(self):
        return f"StateNode(label={self.label!r})"


class Box:
    """Wrapper type handled by a custom handler (see TracingBoxHandler)."""

    def __init__(self, label, inner):
        self.label = label
        self.inner = inner

    def __repr__(self):
        return f"Box(label={self.label!r})"


GRAPH_CLASSES = [
    PlainNode,
    SlotNode,
    DataNode,
    SlottedDataNode,
    ReduceNode,
    StateNode,
]


# Side-effect probe used to prove that safe=True never evaluates a py/repr
# payload, while safe=False goes through eval().
SIDE_EFFECT_MARKER = {"calls": 0}


def marker_side_effect():
    SIDE_EFFECT_MARKER["calls"] += 1
    return "marked"


# ---------------------------------------------------------------------------
# Graph builders.  Each returns a fresh graph and carries a `source` string
# with the smallest inline construction code, used in failure output.
# ---------------------------------------------------------------------------


def build_shared_plain():
    a = PlainNode("a")
    a.peer = a  # self alias
    shared = PlainNode("shared")
    return {"root": a, "one": shared, "two": shared, "list": [shared, a]}


build_shared_plain.source = (
    'a = PlainNode("a"); a.peer = a\n'
    'shared = PlainNode("shared")\n'
    'graph = {"root": a, "one": shared, "two": shared, "list": [shared, a]}'
)
build_shared_plain.has_nonstring_keys = False


def build_cycle_plain():
    a = PlainNode("a")
    b = PlainNode("b")
    a.peer = b
    b.peer = a
    return [a, b, a]


build_cycle_plain.source = (
    'a = PlainNode("a"); b = PlainNode("b")\na.peer = b; b.peer = a\ngraph = [a, b, a]'
)
build_cycle_plain.has_nonstring_keys = False


def build_shared_slots():
    s = SlotNode("slot-shared")
    owner = SlotNode("owner", peer=s)
    return {"owner": owner, "alias1": s, "alias2": s}


build_shared_slots.source = (
    's = SlotNode("slot-shared")\n'
    'owner = SlotNode("owner", peer=s)\n'
    'graph = {"owner": owner, "alias1": s, "alias2": s}'
)
build_shared_slots.has_nonstring_keys = False


def build_cycle_slots():
    a = SlotNode("a")
    b = SlotNode("b")
    a.peer = b
    b.peer = a
    return a


build_cycle_slots.source = (
    'a = SlotNode("a"); b = SlotNode("b")\na.peer = b; b.peer = a\ngraph = a'
)
build_cycle_slots.has_nonstring_keys = False


def build_deleted_slot():
    node = SlotNode("deleted", note="temporary")
    del node.note
    return node


build_deleted_slot.source = 'node = SlotNode("deleted"); del node.note\ngraph = node'
build_deleted_slot.has_nonstring_keys = False


def build_shared_dataclass():
    s = DataNode("data-shared")
    root = DataNode("root", peer=s)
    return [root, s, s, root]


build_shared_dataclass.source = (
    's = DataNode("data-shared")\n'
    'root = DataNode("root", peer=s)\n'
    "graph = [root, s, s, root]"
)
build_shared_dataclass.has_nonstring_keys = False


def build_cycle_slotted_dataclass():
    a = SlottedDataNode("a")
    b = SlottedDataNode("b")
    a.peer = b
    b.peer = a
    return {"a": a, "b": b, "a_again": a}


build_cycle_slotted_dataclass.source = (
    'a = SlottedDataNode("a"); b = SlottedDataNode("b")\n'
    "a.peer = b; b.peer = a\n"
    'graph = {"a": a, "b": b, "a_again": a}'
)
build_cycle_slotted_dataclass.has_nonstring_keys = False


def build_reduce_shared():
    shared = PlainNode("reduce-payload")
    reduced = ReduceNode("r", payload=shared)
    return [reduced, shared, reduced]


build_reduce_shared.source = (
    'shared = PlainNode("reduce-payload")\n'
    'reduced = ReduceNode("r", payload=shared)\n'
    "graph = [reduced, shared, reduced]"
)
build_reduce_shared.has_nonstring_keys = False


def build_state_cycle():
    a = StateNode("a")
    b = StateNode("b")
    a.peer = b
    b.peer = a
    return a


build_state_cycle.source = (
    'a = StateNode("a"); b = StateNode("b")\na.peer = b; b.peer = a\ngraph = a'
)
build_state_cycle.has_nonstring_keys = False


def build_nonstring_key_graph():
    shared = PlainNode("key-shared")
    # The same object appears as a value, as a dict key, and behind an
    # integer key and a tuple key.
    return {
        "s": shared,
        1: shared,
        (1, 2): shared,
        shared: shared,
        "nested": {(2,): [shared, shared]},
    }


build_nonstring_key_graph.source = (
    'shared = PlainNode("key-shared")\n'
    'graph = {"s": shared, 1: shared, (1, 2): shared, shared: shared,\n'
    '         "nested": {(2,): [shared, shared]}}'
)
build_nonstring_key_graph.has_nonstring_keys = True


def build_mixed_graph():
    """One graph exercising every node kind and relationship at once."""
    plain = PlainNode("plain")
    slot = SlotNode("slot")
    data = DataNode("data")
    slotted_data = SlottedDataNode("slotted-data")
    state_a = StateNode("state-a")
    state_b = StateNode("state-b", peer=state_a)
    state_a.peer = state_b
    reduced = ReduceNode("reduce", payload=plain)

    plain.peer = slot
    slot.peer = data
    data.peer = slotted_data
    slotted_data.peer = state_a

    return {
        "plain": plain,
        "slot": slot,
        "data": data,
        "slotted_data": slotted_data,
        "states": [state_a, state_b, state_a],
        "reduced": reduced,
        "payload_alias": plain,
        "keys": {7: plain, ("x",): slot, "d": data},
        "loop": [plain, slot, data, slotted_data],
    }


build_mixed_graph.source = (
    "plain -> slot -> data -> slotted_data -> state_a <-> state_b;\n"
    "reduce payload aliases plain; non-string keys reuse the same objects"
)
build_mixed_graph.has_nonstring_keys = True


GRAPH_BUILDERS = [
    build_shared_plain,
    build_cycle_plain,
    build_shared_slots,
    build_cycle_slots,
    build_deleted_slot,
    build_shared_dataclass,
    build_cycle_slotted_dataclass,
    build_reduce_shared,
    build_state_cycle,
    build_nonstring_key_graph,
    build_mixed_graph,
]


# ---------------------------------------------------------------------------
# Graph snapshotting: value/type topology plus object-identity bookkeeping
# ---------------------------------------------------------------------------


class _Ref:
    """Marker placed in a normalized tree where an alias was encountered."""

    __slots__ = ("idx",)

    def __init__(self, idx):
        self.idx = idx

    def __eq__(self, other):
        return isinstance(other, _Ref) and other.idx == self.idx

    def __repr__(self):
        return f"#{self.idx}"


class GraphMismatch(AssertionError):
    """Raised with full diagnostics when two graphs do not match."""


def _type_name(obj):
    cls = type(obj)
    return f"{cls.__module__}.{cls.__qualname__}"


class Snapshot:
    """Normalized view of an object graph.

    ``tree`` is a pure-data structure describing values and types, with _Ref
    placeholders where previously seen objects recur.  ``objects`` maps the
    traversal index to the first concrete object at that index, and
    ``ref_occurrences`` maps an index to every concrete object encountered at
    a reference site together with the path where it was found.
    """

    def __init__(self, root):
        self._seen = {}
        self.objects = {}
        self.ref_occurrences = {}
        self.tree = self._walk(root, ())

    def _register(self, obj, path):
        idx = self._seen.get(id(obj))
        if idx is None:
            idx = len(self._seen)
            self._seen[id(obj)] = idx
            self.objects[idx] = obj
            self.ref_occurrences[idx] = [(obj, path)]
            return idx, True
        self.ref_occurrences[idx].append((obj, path))
        return idx, False

    def _walk(self, obj, path):
        if isinstance(obj, (str, int, float, bool, type(None))):
            return ("scalar", _type_name(obj), obj)
        if isinstance(obj, bytes):
            return ("bytes", obj)
        if isinstance(obj, bytearray):
            return ("bytearray", bytes(obj))

        idx, is_new = self._register(obj, path)
        if not is_new:
            return ("ref", _Ref(idx))

        if isinstance(obj, dict):
            entries = []
            for key, value in obj.items():
                key_node = self._walk(key, path + ("key",))
                value_node = self._walk(value, path + ("item", repr(key)))
                entries.append((key_node, value_node))
            # Decoding reorders non-string keys (two-phase restore), so the
            # comparison is content-based rather than insertion-order-based.
            entries.sort(
                key=lambda pair: json.dumps(pair[0], default=str, sort_keys=True)
            )
            return ("dict", _type_name(obj), entries)

        if isinstance(obj, (list, tuple)):
            items = [
                self._walk(value, path + (type(obj).__name__, i))
                for i, value in enumerate(obj)
            ]
            return (type(obj).__name__, _type_name(obj), items)

        if isinstance(obj, set):
            # Sets have no order; sort by the rendered form for stability.
            items = sorted(
                (self._walk(value, path + ("set",)) for value in obj),
                key=lambda node: json.dumps(node, default=str, sort_keys=True),
            )
            return ("set", _type_name(obj), items)

        # Custom instance: capture type plus attributes, including slots.
        attrs = {}
        attr_names = self._attribute_names(obj)
        for name in attr_names:
            try:
                value = getattr(obj, name)
            except AttributeError:
                attrs[name] = ("missing",)
                continue
            attrs[name] = self._walk(value, path + ("attr", name))
        return ("object", _type_name(obj), attrs)

    @staticmethod
    def _attribute_names(obj):
        names = list(vars(obj).keys()) if hasattr(obj, "__dict__") else []
        slots = []
        for cls in type(obj).__mro__:
            for slot in getattr(cls, "__slots__", ()):
                if isinstance(slot, str) and not slot.startswith("__"):
                    slots.append(slot)
        # Preserve declaration order while removing duplicates.
        seen = set()
        ordered = []
        for name in names + slots:
            if name not in seen:
                seen.add(name)
                ordered.append(name)
        return ordered


def _first_tree_diff(left, right, path=()):
    """Return (path, left, right) for the first structural difference."""
    if type(left) is not type(right):
        return path, left, right
    if isinstance(left, tuple):
        if len(left) != len(right):
            return path, left, right
        for i, (lpart, rpart) in enumerate(zip(left, right)):
            diff = _first_tree_diff(lpart, rpart, path + (i,))
            if diff is not None:
                return diff
        return None
    if left != right:
        return path, left, right
    return None


def _render(obj, seen=None, depth=0):
    """Render a concrete graph, labeling each object with a traversal id."""
    if seen is None:
        seen = {}
    indent = "  " * depth
    if isinstance(obj, (str, int, float, bool, type(None))):
        return repr(obj)
    if isinstance(obj, (bytes, bytearray)):
        return repr(bytes(obj))

    marker = id(obj)
    if marker in seen:
        return f"->#{seen[marker]} {type(obj).__name__}"
    idx = len(seen)
    seen[marker] = idx

    if isinstance(obj, dict):
        if not obj:
            return "{}"
        lines = [f"{type(obj).__name__} #{idx} {{"]
        for key, value in obj.items():
            lines.append(f"{indent}  {key!r}: {_render(value, seen, depth + 1)}")
        lines.append(f"{indent}}}")
        return "\n".join(lines)

    if isinstance(obj, (list, tuple, set)):
        if isinstance(obj, list):
            brackets = "[]"
        elif isinstance(obj, tuple):
            brackets = "()"
        else:
            brackets = "set()"
        if not obj:
            return brackets
        lines = [f"{type(obj).__name__} #{idx} {brackets[0]}"]
        for value in obj:
            lines.append(f"{indent}  {_render(value, seen, depth + 1)}")
        lines.append(f"{indent}{brackets[-1]}")
        return "\n".join(lines)

    lines = [f"{type(obj).__name__} #{idx} {{"]
    for name in Snapshot._attribute_names(obj):
        try:
            value = getattr(obj, name)
        except AttributeError:
            lines.append(f"{indent}  {name}=<missing>")
            continue
        lines.append(f"{indent}  {name}={_render(value, seen, depth + 1)}")
    lines.append(f"{indent}}}")
    return "\n".join(lines)


def assert_graphs_equal(original, restored):
    """Compare value, type, and identity topology of two graphs."""
    original_snapshot = Snapshot(original)
    restored_snapshot = Snapshot(restored)

    tree_diff = _first_tree_diff(original_snapshot.tree, restored_snapshot.tree)
    if tree_diff is not None:
        path, left, right = tree_diff
        raise GraphMismatch(
            "first value/type mismatch at "
            f"{'/'.join(map(str, path)) or '<root>'}: "
            f"expected {left!r}, got {right!r}"
        )

    if set(original_snapshot.objects) != set(restored_snapshot.objects):
        raise GraphMismatch(
            "object count differs: expected indices "
            f"{sorted(original_snapshot.objects)}, got "
            f"{sorted(restored_snapshot.objects)}"
        )

    # Identity check inside the restored graph: every occurrence recorded for
    # an index must resolve to the same concrete object, and objects at
    # different indices must not have been merged.
    for idx, occurrences in restored_snapshot.ref_occurrences.items():
        first_obj, _ = occurrences[0]
        for obj, path in occurrences[1:]:
            if obj is not first_obj:
                pretty_path = "/".join(map(str, path)) or "<root>"
                raise GraphMismatch(
                    f"first alias mismatch at {pretty_path}: expected the same "
                    f"object as #{idx} ({first_obj!r}), got a distinct object "
                    f"{obj!r}"
                )

    restored_objects = restored_snapshot.objects
    indices = sorted(restored_objects)
    for i, left_idx in enumerate(indices):
        for right_idx in indices[i + 1 :]:
            if restored_objects[left_idx] is restored_objects[right_idx]:
                raise GraphMismatch(
                    f"distinct objects #{left_idx} and #{right_idx} were "
                    "merged into one instance after decode"
                )


def _failure_report(
    builder, encode_kwargs, decode_kwargs, encoded, detail, restored=None
):
    pretty_json = json.dumps(json.loads(encoded), indent=2, sort_keys=True)
    lines = [
        "",
        "=" * 70,
        "OBJECT GRAPH ROUND-TRIP FAILURE",
        f"graph: {builder.__name__}",
        "construction code:",
        builder.source,
        f"encode options: {encode_kwargs}",
        f"decode options: {decode_kwargs}",
        "intermediate JSON:",
        pretty_json,
    ]
    if restored is not None:
        lines.extend(["restored graph:", _render(restored)])
    lines.extend([f"problem: {detail}", "=" * 70])
    return "\n".join(lines)


def assert_roundtrip(builder, encode_kwargs, decode_kwargs, *, classes=None):
    """Encode/decode a builder's graph and compare it with full diagnostics."""
    graph = builder()
    encoded = jsonpickle.encode(graph, **encode_kwargs)
    try:
        restored = jsonpickle.decode(encoded, classes=classes, **decode_kwargs)
    except Exception as exc:
        raise GraphMismatch(
            _failure_report(
                builder,
                encode_kwargs,
                decode_kwargs,
                encoded,
                f"decode raised {type(exc).__name__}: {exc}",
            )
        ) from exc

    try:
        assert_graphs_equal(graph, restored)
    except GraphMismatch as exc:
        raise GraphMismatch(
            _failure_report(
                builder,
                encode_kwargs,
                decode_kwargs,
                encoded,
                str(exc),
                restored,
            )
        ) from exc
    return encoded, restored


# ---------------------------------------------------------------------------
# Option matrix
#
# Three fidelity modes fall out of the refs/unpicklable options:
#   fidelity : full value/type/identity preservation (refs on, unpicklable)
#   norefs   : types survive but aliases are duplicated and cycles are cut
#              with repr() strings
#   lossy    : one-way JSON; types, py/id markers and aliases all disappear
# keys/safe/classes are crossed with the fidelity rows.
# ---------------------------------------------------------------------------


FIDELITY_CASES = [
    pytest.param(
        {"make_refs": True, "keys": True, "unpicklable": True},
        {"keys": True, "safe": True},
        None,
        id="safe-default",
    ),
    pytest.param(
        {"make_refs": True, "keys": True, "unpicklable": True},
        {"keys": True, "safe": False},
        None,
        id="safe-eval-allowed",
    ),
    pytest.param(
        {"make_refs": True, "keys": True, "unpicklable": True},
        {"keys": True, "safe": True},
        GRAPH_CLASSES,
        id="classes-allowlist",
    ),
    pytest.param(
        {"make_refs": True, "keys": True, "unpicklable": True},
        {"keys": True, "safe": False},
        GRAPH_CLASSES,
        id="classes-allowlist-unsafe-decode",
    ),
]


@pytest.mark.parametrize("builder", GRAPH_BUILDERS, ids=lambda b: b.__name__)
@pytest.mark.parametrize("encode_kwargs,decode_kwargs,classes", FIDELITY_CASES)
def test_graph_roundtrip_fidelity_matrix(
    builder, encode_kwargs, decode_kwargs, classes
):
    """Every graph keeps values, types and identity in fidelity mode."""
    assert_roundtrip(builder, dict(encode_kwargs), dict(decode_kwargs), classes=classes)


# Full cross product of the Boolean encode/decode dimensions.  Each row is
# classified into a fidelity mode and checked against mode-specific rules.
MATRIX_ROWS = []
for make_refs, keys, unpicklable, safe in itertools.product((True, False), repeat=4):
    encode_kwargs = {
        "make_refs": make_refs,
        "keys": keys,
        "unpicklable": unpicklable,
    }
    decode_kwargs = {"keys": keys, "safe": safe}
    if not unpicklable:
        mode = "lossy"
    elif not make_refs:
        mode = "norefs"
    else:
        mode = "fidelity"
    MATRIX_ROWS.append(
        pytest.param(
            encode_kwargs,
            decode_kwargs,
            mode,
            id=(f"refs={make_refs}-keys={keys}-unpicklable={unpicklable}-safe={safe}"),
        )
    )


@pytest.mark.parametrize("builder", GRAPH_BUILDERS, ids=lambda b: b.__name__)
@pytest.mark.parametrize("encode_kwargs,decode_kwargs,mode", MATRIX_ROWS)
def test_option_matrix_marker_invariants(builder, encode_kwargs, decode_kwargs, mode):
    """Wire-format invariants that hold for every graph/option combination."""
    graph = builder()
    encoded = jsonpickle.encode(graph, **encode_kwargs)

    if mode == "lossy":
        assert "py/" not in encoded
    if mode == "norefs":
        assert "py/id" not in encoded
    if mode == "fidelity" and (
        not builder.has_nonstring_keys and _graph_has_aliases(graph)
    ):
        # Identity markers must actually be present when aliases exist.
        assert "py/id" in encoded

    # Every combination must decode without raising.
    restored = jsonpickle.decode(encoded, **decode_kwargs)
    if mode == "lossy":
        _assert_plain_json_data(restored)

    # None of these graphs carry a py/repr payload, so flipping safe must not
    # change the restored topology; eval() has nothing to act on.
    other = dict(decode_kwargs)
    other["safe"] = not other["safe"]
    restored_other_safe = jsonpickle.decode(encoded, **other)
    assert (
        _first_tree_diff(Snapshot(restored).tree, Snapshot(restored_other_safe).tree)
        is None
    )


def _graph_has_aliases(root):
    seen = set()

    def walk(obj):
        if isinstance(obj, (str, int, float, bool, type(None), bytes)):
            return False
        if id(obj) in seen:
            return True
        seen.add(id(obj))
        if isinstance(obj, dict):
            return any(walk(k) or walk(v) for k, v in obj.items())
        if isinstance(obj, (list, tuple, set)):
            return any(walk(v) for v in obj)
        for name in Snapshot._attribute_names(obj):
            try:
                if walk(getattr(obj, name)):
                    return True
            except AttributeError:
                continue
        return False

    return walk(root)


def _assert_plain_json_data(obj):
    if isinstance(obj, dict):
        for key, value in obj.items():
            # One-way encoding keeps only hashable JSON scalars as keys;
            # anything more complex falls back to its raw string form.
            assert isinstance(key, (str, int, float, bool, type(None))), (
                f"non-JSON key survived one-way encoding: {key!r}"
            )
            _assert_plain_json_data(value)
    elif isinstance(obj, list):
        for value in obj:
            _assert_plain_json_data(value)
    else:
        assert isinstance(obj, (str, int, float, bool, type(None))), (
            f"non-JSON type survived one-way encoding: {type(obj)}"
        )


# ---------------------------------------------------------------------------
# Documented degradation modes: refs disabled and one-way encoding
# ---------------------------------------------------------------------------


def test_make_refs_false_duplicates_siblings():
    """Without refs, an aliased object is serialized as independent copies."""
    node = PlainNode("twin")
    encoded = jsonpickle.encode([node, node], make_refs=False)
    restored = jsonpickle.decode(encoded)

    assert isinstance(restored[0], PlainNode)
    assert isinstance(restored[1], PlainNode)
    assert restored[0] is not restored[1]
    assert restored[0].label == restored[1].label == "twin"
    assert "py/id" not in encoded


def test_make_refs_false_breaks_cycles_with_repr():
    """A cycle is broken with repr() when reference support is off."""
    a = PlainNode("a")
    b = PlainNode("b")
    a.peer = b
    b.peer = a
    encoded = jsonpickle.encode(a, make_refs=False)
    restored = jsonpickle.decode(encoded)

    assert isinstance(restored, PlainNode)
    assert isinstance(restored.peer, PlainNode)
    # The back edge can no longer point at the real object.
    assert isinstance(restored.peer.peer, str)
    assert "PlainNode" in restored.peer.peer
    assert restored.peer is not restored


def test_make_refs_false_slots_and_dataclass_cycles():
    """Cycle breaking applies identically to slots and dataclass nodes."""
    for cls in (SlotNode, DataNode, SlottedDataNode, StateNode):
        a = cls("a")
        b = cls("b")
        a.peer = b
        b.peer = a
        restored = jsonpickle.decode(jsonpickle.encode(a, make_refs=False))
        assert isinstance(restored, cls), cls
        assert isinstance(restored.peer, cls), cls
        assert isinstance(restored.peer.peer, str), cls


def test_unpicklable_false_drops_types_and_aliases():
    """One-way encoding produces plain JSON: no types, no ids, no aliases."""
    shared = PlainNode("shared")
    graph = [shared, shared]
    encoded = jsonpickle.encode(graph, unpicklable=False)

    assert "py/" not in encoded
    restored = jsonpickle.decode(encoded)
    assert restored == [
        {"label": "shared", "peer": None, "note": None},
        {"label": "shared", "peer": None, "note": None},
    ]
    assert restored[0] is not restored[1]


def test_unpicklable_false_cycle_back_edge_is_null():
    a = PlainNode("a")
    b = PlainNode("b")
    a.peer = b
    b.peer = a
    encoded = jsonpickle.encode(a, unpicklable=False)
    restored = jsonpickle.decode(encoded)

    assert isinstance(restored, dict)
    assert restored["peer"]["peer"] is None


def test_unpicklable_false_reduce_node_becomes_state_data():
    shared = PlainNode("payload")
    reduced = ReduceNode("r", payload=shared)
    encoded = jsonpickle.encode([reduced, shared], unpicklable=False)
    restored = jsonpickle.decode(encoded)

    assert "py/" not in encoded
    # The payload is emitted twice as independent plain dicts.
    assert restored[0] == {
        "label": "r",
        "payload": {"label": "payload", "peer": None, "note": None},
    }
    assert restored[0]["payload"] == restored[1]
    assert restored[0]["payload"] is not restored[1]
    assert isinstance(restored[0], dict)


# ---------------------------------------------------------------------------
# Non-string keys
# ---------------------------------------------------------------------------


def test_nonstring_keys_preserve_types_and_identity():
    graph = build_nonstring_key_graph()
    encoded = jsonpickle.encode(graph, keys=True)
    restored = jsonpickle.decode(encoded, keys=True)

    key_types = {type(key) for key in restored if key != "nested"}
    assert key_types == {str, int, tuple, PlainNode}
    assert restored[1] is restored[(1, 2)] is restored["s"]
    object_key = next(key for key in restored if isinstance(key, PlainNode))
    assert object_key is restored["s"]
    assert restored["nested"][(2,)][0] is restored["nested"][(2,)][1]
    assert restored["nested"][(2,)][0] is restored["s"]


def test_nonstring_keys_repr_coerced_when_disabled():
    graph = build_nonstring_key_graph()
    encoded = jsonpickle.encode(graph, keys=False)
    restored = jsonpickle.decode(encoded, keys=False)

    assert all(isinstance(key, str) for key in restored if key != "nested")
    assert "1" in restored and "(1, 2)" in restored
    # Values still share identity through py/id; only the keys are strings.
    assert restored["1"] is restored["(1, 2)"] is restored["s"]


def test_mismatched_keys_options_keep_json_key_prefix_literal():
    graph = build_nonstring_key_graph()
    encoded = jsonpickle.encode(graph, keys=True)
    restored = jsonpickle.decode(encoded, keys=False)

    prefixed = [
        key for key in restored if isinstance(key, str) and key.startswith("json://")
    ]
    assert prefixed  # object keys are handed back untouched as strings
    assert restored["s"] is restored[prefixed[0]]


# ---------------------------------------------------------------------------
# Custom handlers: call traces and inner/outer session handling
# ---------------------------------------------------------------------------


HANDLER_TRACE = []


class TracingBoxHandler(BaseHandler):
    """Handler that re-enters jsonpickle on the shared outer session."""

    def flatten(self, obj, data):
        HANDLER_TRACE.append(
            ("flatten", obj.label, id(self.context), len(self.context._objs))
        )
        data["label"] = obj.label
        data["inner_enc"] = jsonpickle.encode(
            obj.inner, context=self.context, reset=False
        )
        return data

    def restore(self, obj):
        HANDLER_TRACE.append(
            ("restore", obj["label"], id(self.context), len(self.context._objs))
        )
        inner = jsonpickle.decode(obj["inner_enc"], context=self.context, reset=False)
        return Box(obj["label"], inner)


class IsolatedSessionBoxHandler(BaseHandler):
    """Broken handler: inner calls start fresh sessions, losing outer refs."""

    def flatten(self, obj, data):
        HANDLER_TRACE.append(
            (
                "isolated-flatten",
                obj.label,
                id(self.context),
                len(self.context._objs),
            )
        )
        # Deliberately no context/reset=False: this inner encode cannot see
        # reference ids assigned by the outer session.
        data["label"] = obj.label
        data["inner_enc"] = jsonpickle.encode(obj.inner)
        return data

    def restore(self, obj):
        HANDLER_TRACE.append(
            (
                "isolated-restore",
                obj["label"],
                id(self.context),
                len(self.context._objs),
            )
        )
        return Box(obj["label"], jsonpickle.decode(obj["inner_enc"]))


@pytest.fixture
def tracing_handler():
    HANDLER_TRACE.clear()
    register(Box, TracingBoxHandler)
    yield TracingBoxHandler
    unregister(Box)
    HANDLER_TRACE.clear()


def build_handler_graph():
    shared = PlainNode("handler-shared")
    box = Box("box", shared)
    return {"box": box, "box_alias": box, "inner_alias": shared}


def test_handler_shares_outer_session_and_preserves_aliases(tracing_handler):
    graph = build_handler_graph()
    encoded = jsonpickle.encode(graph)
    restored = jsonpickle.decode(encoded)

    # Identity survives across the handler boundary.
    assert restored["box"] is restored["box_alias"]
    assert restored["box"].inner is restored["inner_alias"]
    assert isinstance(restored["box"], Box)
    assert isinstance(restored["box"].inner, PlainNode)

    kinds = [entry[0] for entry in HANDLER_TRACE]
    assert kinds == ["flatten", "restore"]
    # Both handler invocations run while outer objects already hold ids, so
    # the inner call participates in the outer session's id table.
    assert HANDLER_TRACE[0][3] >= 1  # box itself is id 0
    assert HANDLER_TRACE[1][3] >= 1
    # The outer alias is expressed as a reference, never a second copy.
    assert '"py/id"' in encoded


def test_handler_trace_records_every_invocation(tracing_handler):
    first = Box("first", PlainNode("a"))
    second = Box("second", PlainNode("b"))
    graph = [first, second, first]
    encoded = jsonpickle.encode(graph)
    jsonpickle.decode(encoded)

    flatten_labels = [e[1] for e in HANDLER_TRACE if e[0] == "flatten"]
    restore_labels = [e[1] for e in HANDLER_TRACE if e[0] == "restore"]
    # The third occurrence is an alias, so the handler runs only twice each
    # way, in encounter order.
    assert flatten_labels == ["first", "second"]
    assert restore_labels == ["first", "second"]
    # Both flatten calls share one Pickler session; both restores one
    # Unpickler session.
    flatten_contexts = {e[2] for e in HANDLER_TRACE if e[0] == "flatten"}
    restore_contexts = {e[2] for e in HANDLER_TRACE if e[0] == "restore"}
    assert len(flatten_contexts) == 1
    assert len(restore_contexts) == 1
    assert flatten_contexts != restore_contexts


def test_handler_with_isolated_session_breaks_aliases():
    HANDLER_TRACE.clear()
    register(Box, IsolatedSessionBoxHandler)
    try:
        graph = build_handler_graph()
        encoded = jsonpickle.encode(graph)
        restored = jsonpickle.decode(encoded)

        # The inner object was flattened by a brand-new session and emitted a
        # second, independent full object instead of a py/id reference.
        assert restored["box"].inner is not restored["inner_alias"]
        assert restored["box"].inner.label == "handler-shared"
        kinds = [entry[0] for entry in HANDLER_TRACE]
        assert kinds == ["isolated-flatten", "isolated-restore"]
    finally:
        unregister(Box)
        HANDLER_TRACE.clear()


def test_top_level_sessions_do_not_leak_reference_ids():
    """Two independent encode calls both number their first object id 0."""
    first = [PlainNode("first-a"), PlainNode("first-b")]
    second = [PlainNode("second-a"), PlainNode("second-b")]

    encoded_first = jsonpickle.encode(first)
    encoded_second = jsonpickle.encode(second)

    def max_ref_id(payload):
        ids = []

        def walk(node):
            if isinstance(node, dict):
                if "py/id" in node:
                    ids.append(node["py/id"])
                for value in node.values():
                    walk(value)
            elif isinstance(node, list):
                for value in node:
                    walk(value)

        walk(json.loads(payload))
        return max(ids, default=-1)

    # Each call only knows its own two objects, so ids stay within [0, 1].
    assert max_ref_id(encoded_first) <= 1
    assert max_ref_id(encoded_second) <= 1

    pickler = Pickler()
    pickler.flatten(first)
    assert pickler._objs == {}  # reset() ran when flatten() returned
    pickler.flatten(second)
    assert pickler._objs == {}


# ---------------------------------------------------------------------------
# Allowlist and safe-mode refusals
# ---------------------------------------------------------------------------


def _make_local_secret_class():
    class LocalSecret:
        """Class defined inside a function: not importable by name."""

        def __init__(self, token):
            self.token = token

    return LocalSecret


def test_unlisted_class_is_not_importable_and_is_refused_visibly():
    secret_cls = _make_local_secret_class()
    encoded = jsonpickle.encode(secret_cls("top-secret"))
    class_name = jsonpickle.util.importable_name(secret_cls)
    assert "<locals>" in class_name

    # The global import machinery cannot resolve the local class.
    assert jsonpickle.util.loadclass(class_name) is None

    # on_missing="error": a visible, class-named refusal.
    with pytest.raises(jsonpickle.errors.ClassNotFoundError) as exc_info:
        jsonpickle.decode(encoded, on_missing="error")
    assert class_name in str(exc_info.value)

    # on_missing="warn": the refusal is surfaced as a warning and the raw
    # dictionary is handed back.
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        restored = jsonpickle.decode(encoded, on_missing="warn")
    assert any(
        issubclass(entry.category, UserWarning) and class_name in str(entry.message)
        for entry in caught
    )
    assert isinstance(restored, dict)
    assert restored["token"] == "top-secret"

    # on_missing callback: receives the offending class name.
    missing = []
    restored = jsonpickle.decode(encoded, on_missing=missing.append)
    assert missing == [class_name]
    assert isinstance(restored, dict)


def test_allowlisted_local_class_roundtrips_with_identity():
    secret_cls = _make_local_secret_class()
    instance = secret_cls("allowed")
    graph = [instance, instance]
    encoded = jsonpickle.encode(graph)

    restored = jsonpickle.decode(encoded, classes=secret_cls)
    assert type(restored[0]) is secret_cls
    assert restored[0].token == "allowed"
    assert restored[0] is restored[1]

    # A sequence allowlist works as well and still rejects outsiders.
    restored_seq = jsonpickle.decode(encoded, classes=[secret_cls])
    assert type(restored_seq[0]) is secret_cls

    # An allowlist containing an unrelated, differently-named class cannot
    # satisfy the payload's local-class reference.
    with pytest.raises(jsonpickle.errors.ClassNotFoundError):
        jsonpickle.decode(encoded, classes=PlainNode, on_missing="error")


def test_safe_mode_does_not_evaluate_repr_payload():
    payload = '{"py/repr": "object_graph_test/object_graph_test.marker_side_effect()"}'
    SIDE_EFFECT_MARKER["calls"] = 0

    safe_result = jsonpickle.decode(payload, safe=True)
    assert SIDE_EFFECT_MARKER["calls"] == 0
    # Safe resolution refuses eval(); the dotted top-level function cannot be
    # resolved through the import-only loader, so the refusal is visible as
    # None rather than an evaluated result.
    assert safe_result is None

    unsafe_result = jsonpickle.decode(payload, safe=False)
    assert SIDE_EFFECT_MARKER["calls"] == 1
    assert unsafe_result == "marked"


# ---------------------------------------------------------------------------
# Mutation tests: deliberately broken reference-id sessions must fail here
# ---------------------------------------------------------------------------


LEAKED_PICKLER_OBJS = {}


class LeakyPickler(Pickler):
    """Pickler whose id table is shared across every encode call."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._objs = LEAKED_PICKLER_OBJS

    def reset(self):
        # Deliberate bug: drop the clear of _objs so ids leak across calls.
        self._depth = -1
        self._seen = []
        self._flattened = {}


class LeakyUnpickler(Unpickler):
    """Unpickler whose object table survives reset() between decodes."""

    _leaked_objs: ClassVar[list] = []
    _leaked_index: ClassVar[dict] = {}

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._objs = type(self)._leaked_objs
        self._obj_to_idx = type(self)._leaked_index

    def reset(self):
        # Deliberate bug: keep the previously restored objects reachable.
        self._namedict = {}
        self._namestack = []
        self._proxies = []
        self._classes = {}


FIDELITY_ENCODE = {"make_refs": True, "keys": True, "unpicklable": True}
FIDELITY_DECODE = {"keys": True, "safe": True}
REPORT_SECTIONS = (
    "construction code",
    "encode options",
    "decode options",
    "intermediate JSON",
    "restored graph",
)


def _assert_full_report(exc, builder):
    report = str(exc.value)
    for section in REPORT_SECTIONS:
        assert section in report, section
    assert builder.__name__ in report
    # The report must point at the first alias/type mismatch location.
    assert (
        "first alias mismatch at" in report
        or "first value/type mismatch at" in report
        or "decode raised" in report
    )


def test_leaky_pickler_is_caught(monkeypatch):
    """A cross-call id leak makes every aliased graph fail the same checks."""
    monkeypatch.setattr(jsonpickle.pickler, "Pickler", LeakyPickler)
    aliased_builders = [b for b in GRAPH_BUILDERS if _graph_has_aliases(b())]
    assert aliased_builders, "the suite needs aliased graphs to detect leaks"

    failures = 0
    for builder in aliased_builders:
        LEAKED_PICKLER_OBJS.clear()
        # A prior, unrelated call consumes ids in the shared table.  Keep the
        # warmup objects alive so their id() entries cannot be reused, which
        # makes the leaked (offset) ids deterministic.
        warmup = [PlainNode(f"warm-{i}") for i in range(3)]
        jsonpickle.encode(warmup)

        with pytest.raises(GraphMismatch) as exc_info:
            assert_roundtrip(builder, dict(FIDELITY_ENCODE), dict(FIDELITY_DECODE))
        _assert_full_report(exc_info, builder)
        failures += 1
        del warmup

    assert failures == len(aliased_builders)


def test_leaky_unpickler_is_caught(monkeypatch):
    """A cross-call object-table leak breaks every aliased graph."""
    monkeypatch.setattr(jsonpickle.unpickler, "Unpickler", LeakyUnpickler)
    aliased_builders = [b for b in GRAPH_BUILDERS if _graph_has_aliases(b())]

    for builder in aliased_builders:
        LeakyUnpickler._leaked_objs.clear()
        LeakyUnpickler._leaked_index.clear()

        # First call populates the table that reset() fails to clear.
        jsonpickle.decode(jsonpickle.encode(PlainNode("call-one")))

        with pytest.raises(GraphMismatch) as exc_info:
            assert_roundtrip(builder, dict(FIDELITY_ENCODE), dict(FIDELITY_DECODE))
        _assert_full_report(exc_info, builder)


def test_leaky_unpickler_reroutes_self_ref_to_stale_object(monkeypatch):
    """With an object root, id 0 of the next call resolves to a stale object."""
    monkeypatch.setattr(jsonpickle.unpickler, "Unpickler", LeakyUnpickler)
    LeakyUnpickler._leaked_objs.clear()
    LeakyUnpickler._leaked_index.clear()

    first = PlainNode("call-one")
    first_decoded = jsonpickle.decode(jsonpickle.encode(first))

    second = StateNode("call-two")
    second.peer = second
    second_decoded = jsonpickle.decode(
        jsonpickle.encode(second, **FIDELITY_ENCODE), **FIDELITY_DECODE
    )

    assert second_decoded.label == "call-two"
    # The self-reference id 0 now resolves to the previous call's object.
    assert second_decoded.peer is first_decoded
    with pytest.raises(GraphMismatch):
        assert_graphs_equal(second, second_decoded)


def test_suite_passes_against_the_real_implementation():
    """Sanity anchor: the same helper used by the matrix accepts good output."""
    for builder in GRAPH_BUILDERS:
        assert_roundtrip(
            builder,
            dict(FIDELITY_ENCODE),
            dict(FIDELITY_DECODE),
        )
