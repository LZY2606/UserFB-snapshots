# ipfixline

IPFIX (RFC 7011) template epoch evidence and replay tool for network
capacity analysis. It imports IPFIX messages from multiple exporters,
keeps full provenance, builds template/options-template generations,
and resolves every data record against exactly the template that was
visible at that point.

## Features

- **Import with provenance**: every message keeps exporter identity,
  observation domain id, export time, sequence number, set range and
  the SHA-256 digest of the original message bytes.
- **Template epochs**: template and options-template definitions are
  versioned per (exporter, observation domain, template id). The same
  template id in different domains or sessions never shares state.
- **Exact replay resolution**: each data record is decoded with the
  single generation visible at its arrival point. Enterprise elements,
  variable-length fields, padding and unparsed tails are shown.
- **Nothing is dropped**: template withdrawal, all-templates
  withdrawal, id reuse after reconnect, sequence gaps, templates
  arriving after data sets, and truncated variable-length fields are
  all kept as candidates on the resolution.
- **Sequence numbers** advance per data record (not per message) and
  are tracked per (exporter, observation domain).
- **Session boundary revision**: capture session boundaries can be
  corrected; every correction creates a new session version and a new
  replay revision. Old resolutions are retained and remain queryable.
- **Deterministic**: no wall-clock time, no sleeps, no network. A
  logical tick orders revisions, so any replay is reproducible and the
  database can be rebuilt from empty by re-importing.

## Quick start

```sh
uv sync --frozen                 # install locked dependencies, build package
uv run pytest -q                 # run the test suite (built-in sample only)
uv run python -m ipfixline --port 5580
```

Open http://127.0.0.1:5580 — the page shows the
**Template epoch evidence** for the built-in sample capture.

## Built-in sample

The sample capture (loaded automatically into an empty database)
contains two exporters and exercises:

- three generations of template id 256 on exporter A (redefinition,
  explicit withdrawal, id reuse after reconnect),
- an options template with a variable-length field,
- an enterprise-specific element (PEN 9),
- template withdrawal and all-templates withdrawal (empty set 2),
- a reconnect with a sequence gap (expected 4, observed 10),
- a data set arriving before its template (id 300 on exporter B),
- a truncated variable-length field,
- padding bytes at the end of a data set,
- the same template id 256 in a different observation domain (200),
  proving domains do not share template state.

## Revising session boundaries

On the evidence page each session row has a *revise* form. Submitting
new start/end arrival indexes creates session version N+1, starts a
new replay revision, and re-resolves all records — while the previous
session versions and resolutions stay in the database untouched.

## Layout

- `ipfixline/parser.py` — IPFIX message/set/template parsing
- `ipfixline/replay.py` — epoch building and record resolution
- `ipfixline/store.py` — SQLite persistence (rebuildable from empty)
- `ipfixline/sample.py` — synthetic built-in sample capture
- `ipfixline/web.py` — stdlib web UI
- `tests/test_replay.py` — identity, record counts, error ranges,
  session revision and replayability assertions

## Database

Default file: `ipfixline.db` (override with `--db`). Delete it to
rebuild from empty; the sample is re-imported on first start.
