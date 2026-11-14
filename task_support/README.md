# Target runner

Run `bash task_support/run.sh baseline` for the existing decoder behavior.
Run `bash task_support/run.sh target` after implementing the requested feature
and creating `graph_decoder_test.go`.

The required top-level tests are `TestGraphIdentity`, `TestGraphRouting`, and
`TestGraphPublication`, in package `jsonapi`. Each test must call
`taskcheck.Run(t, func(c *taskcheck.C) { ... })` from
`github.com/google/jsonapi/task_support/taskcheck`, and perform content checks
through `c.Check(bool, message, ...args)`. The helper records normal return and
executed checks. The shell independently checks every group's completion,
Go PASS result, and absence of skips. Do not replace these support files.

The repository and this support use only the Go standard library. Proxy,
checksum database and automatic toolchain downloads are disabled; caches stay
under `.task-build`. The target run enables the race detector and then runs
all existing tests. No GraphDecoder implementation or target cases are supplied.
