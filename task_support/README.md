This directory contains build support and a normal upstream API example, not an
implementation of supplementary macro traversal. All files here are MIT licensed.

Run `bash task_support/run.sh baseline` from the source root. CMake, a C99 compiler
and ordinary system build tools are required. Decompression is disabled. No package
installation, download, Python runtime, external ELF tools, or browser is used.
`fixture.c` writes ELF64 little-endian data with a DWARF5 CU, line and macro units.
The supplementary object deliberately has only `.debug_macro` and `.debug_str`.
The baseline reads the main macro context and confirms the existing tied relation;
it also runs upstream selfmacrocheck, whose expected error diagnostics are normal.

For `bash task_support/run.sh target`, add `test/supplementary_macro_contract.c`.
Include `task_support/check.h` and register `const struct task_case task_cases[]`
plus `const size_t task_case_count`. Each case has a group, name and `int(void)`
function. Use groups `owner_context`, `import_execution`, `recovery`; every group
needs at least one case with a real TASK_CHECK, returning zero on success.
The shared driver owns main. Missing/empty files, no registered cases, no checks,
failing checks and incomplete group execution fail the command.

The target links the current static production library. `TASK_DWARFDUMP` names the
freshly built production CLI, `TASK_BUILD` a scratch directory; use those paths
for CLI regressions. `task_basic_fixtures(TASK_BUILD)` may create the normal input
pair. Add separate C fixtures for the new scenarios. CLI arguments include
`--file-name=src/bin/dwarfdump/dwarfdump.conf` and `--file-tied=<supplementary>`.
Preserve the CLI exit code and inspect diagnostics as well as output content.
CMake recompiles files registered with the library/tool targets, including new
production sources if the implementation adds them.
