# Changelog

## Export cleaning pipeline and error attribution

### Added

- `Options.CleanFunc func(interface{}) ([]interface{}, error)`.
  - Invoked exactly once per record by the single export dispatcher, in the
    same order in which `Exports` receives records.
  - No `CleanFunc`: records pass through unchanged.
  - Zero results filter the record out. Several results from one input are
    delivered consecutively. If both results and a non-nil error are returned,
    the whole batch is dropped, the error is recorded, and later inputs are
    still processed.
  - Every live exporter receives the same cleaned sequence: each result is
    handed to all live exporters before the next result is dispatched.
- `(*Geziyor).StartE() error`. `Start()` keeps its signature and now calls the
  same implementation, logging the error `StartE` returns.
  - Returns only after request callbacks finish, the dispatcher closes every
    exporter's input, and every `Export` call has returned.
  - A configured `CleanFunc` also runs, and `Exports` is drained, when there
    are no exporters.
  - Calling `Start`/`StartE` more than once returns `ErrAlreadyStarted`.

### Error ownership

- Clean errors and exporter errors are combined with `errors.Join`, so
  `errors.Is` finds the original clean error and every original export error.
- `CleanFunc` errors are wrapped as `clean: <err>`.
- Exporter errors are attributed to the exporter's zero-based position in
  `Options.Exporters` through `*ExporterError{Index, Err}`; its `Unwrap` keeps
  `errors.Is`/`errors.As` working on the original error. When several exporters
  fail, every failure is kept, not just the first.
- An `Export` call that returns non-nil is a failure. Returning nil before its
  input channel is closed is also a failure (`ErrExporterExitedEarly`). An
  exporter that returns nil after its input was closed by the dispatcher is a
  normal shutdown and contributes no error.
- A failed (or early-exited) exporter is excluded from dispatch immediately:
  sends to it no longer block the dispatcher. Live exporters keep receiving in
  order and still apply backpressure through unbuffered channels; records are
  neither buffered without bound, dropped, timed out, nor replayed. Third-party
  exporters that never return are not forcibly interrupted; they are expected
  to consume their input or return on their own.

### Built-in exporters

- `JSONLine.Export` now returns encoding, write, and close errors instead of
  logging them and reporting success. Successful output is byte-for-byte
  unchanged.
- `CSV.Export` now returns `Write`, `Flush`, and `Close` errors (flush errors
  are read back via `Writer.Error`).
- `JSON` and `PrettyPrint` keep their previous behavior and output format.

### Why the older tests missed these paths

- `export/json_test.go` and `export/csv_test.go` only fed encodable values to
  writable files and asserted success-case bytes after a `time.Sleep`. They
  never passed unencodable data or a writer that fails, so the logging-and-
  continue branches were never observed and no returned error was checked.
- The root `geziyor_test.go` cases exercised only all-healthy fan-out. They had
  no exporter that fails or exits early, no partial-failure isolation, no
  clean/filter/split stage, no assertion that `Start` waits for `Export` to
  return, and no no-exporter path.
- Pre-existing and unrelated: `TestJSONExporter_Export` expects no trailing
  comma for a single JSON object, while the unchanged `JSON` exporter emits
  `,\n` per line. That assertion was failing before this change and was left
  untouched.
