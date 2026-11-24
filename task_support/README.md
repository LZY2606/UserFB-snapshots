# Forward-declaration / abstract-origin analysis

## What this covers

The production read chain is:

```
ElfReader
  -> CompilationUnit (DWARF parser) + DIEDispatcher
  -> DwarfCUToModule (RootDIEHandler)
  -> Module
  -> Module::Write  (Breakpad symbol text, format unchanged)
```

A concrete `DW_TAG_subprogram` or `DW_TAG_inlined_subroutine` may name its
declaration through `DW_AT_specification` or `DW_AT_abstract_origin`. In legal
DWARF4 the cited declaration DIE may appear *later* in `.debug_info`, possibly
in another compilation unit, and the citation may hop through several
intermediate DIEs before reaching the terminal declaration that carries the
name. The samples in `task_support/samples` and the builders in
`task_support/fixture.h` exercise exactly these shapes.

## Pending declarations and Module object lifetime

When a concrete instance is parsed before the declaration it cites, it has no
name yet. The handler does not guess from an address or from a repeated
string; it keeps the placeholder `"<name omitted>"` and registers the concrete
object as a *waiter* on the cited DIE:

- `DwarfCUToModule::FileContext` keeps, per file, a link table
  (`offset -> cited offset`), a name table, and a multimap of waiting
  `Module::Function*` keyed by the terminal DIE of each chain. One declaration
  can have many waiters (the fan-out case).
- When the terminal declaration is finally parsed, its name is propagated to
  every waiter on that chain. Because a `Module::Function` is owned by the
  `Module` once its CU finishes, the object's address never changes; only the
  `name` field is updated in place. A waiter that still belongs to its
  in-flight CU is renamed the same way and deduplicated through the normal
  `Module::AddFunction` path when that CU finishes.
- Removing one duplicate function calls
  `FileContext::RemoveFunctionWaiter`, which erases only that function's
  waiter entry. Eliminating a duplicate therefore never revokes the pending
  name of the other concrete functions sharing the declaration.
- Same-address records whose names are not known yet are distinct objects and
  are all retained. They are only treated as a real duplicate once both carry
  the same resolved (non-placeholder) name; that retirement happens in
  `Module::RetireDuplicateFunction` for Module-owned objects.
- Inline origins follow the same rule inside `Module::InlineOriginMap`.
  `SetReference` only extends a reference chain; it never moves or deletes an
  `InlineOrigin` that was already handed to a `Module::Inline`. Two origins
  created separately can later converge on one terminal, and every previously
  returned pointer stays valid for the life of the `Module`. `Module::Write`
  still merges same-named origins via `CreateInlineOrigins`, so the symbol
  output keeps one `INLINE_ORIGIN` per name.

The practical rule for callers: hold the `Module` (and its `FileContext`)
alive until every compilation unit has been processed. No extra finalization
call is required — name fix-up happens as the late declaration DIEs are
parsed, and all waiters are resolved by the time the last CU's `Start()`
returns. Do not retain raw `Module::Function*` / `InlineOrigin*` past the
`Module`'s destruction; while the `Module` lives those pointers remain valid
even when names arrive late.

## Reusable offline consumer

`google_breakpad::OfflineSymbolConsumer::Analyze(elf_path)`
(`src/common/dwarf/dwarf_diag_consumer.{h,cc}`) is the reusable offline
analysis entry point. It:

1. opens the ELF with `ElfReader` and maps only the non-allocated
   `.debug_abbrev` / `.debug_info` sections (no machine code is loaded for
   execution and no process memory is read);
2. runs the production chain above to populate a `Module`;
3. runs a second, independent `CompilationUnit` + `DIEDispatcher` pass that
   records only structural evidence — per-DIE CU offset, tag, and
   specification/abstract-origin edges — through the same callbacks;
4. returns an `OfflineAnalysis` that owns:
   - `symbols_text`: verbatim `Module::Write(ALL_SYMBOL_DATA)` output, and
   - `relations`: self-owned `RelationRecord`s. Each record carries the sample
     name, CU and concrete-DIE `.debug_info` **section** offsets (never
     CU-relative), concrete ranges, inline call line, each citation hop's
     source/target section offsets and attribute, the terminal offset, and the
     final name taken from the same populated `Module`.

The evidence pass never resolves names, so a production name-resolution defect
cannot be hidden by a parallel implementation.

## Running

```sh
# Build production sources + the basic reader and print current names.
bash task_support/run.sh baseline

# Same build, plus tests/forward_origins.cc linked against task_support/driver.c.
# Runs the fanout / convergence / lifecycle groups and the basic reader again.
bash task_support/run.sh target
```

The runner compiles every `.cc` under `src/common` and `src/common/dwarf`, then
links the regression (`tests/forward_origins.cc`, registered through
`task_cases` / `TASK_CHECK` in the three groups). The three designated inputs
— `fanout.elf`, `cross-cu.elf`, and the `inline_convergence()` fixture — are
analyzed by the group cases themselves, which write:

- `analysis/symbols.txt` — concatenated Breakpad symbol text,
- `analysis/relations.tsv` — one tab-separated relation row per concrete
  function / inline instance,
- `analysis/relations.md` — a short narrative of the user/declaration/address
  relationships and why the old output dropped names or merged records.

Under the macOS sandbox used here, a bare `mktemp` ignores `$TMPDIR`; if the
runner cannot create its log, prefix the PATH with the wrapper shipped under
`.task-build/bin` (created on first use):
`PATH="$PWD/.task-build/bin:$PATH" bash task_support/run.sh target`.

## Expected results

- `fanout.elf`: ranges `0x1000` and `0x1100` both resolve to `late`; `0x2000`
  stays `normal`.
- `cross-cu.elf`: address `0x3000` retains two functions, `Alpha` and `Beta`,
  from two distinct declarations.
- `inline_convergence()`: the two inline instances in `host` keep their own
  ranges and call lines (10 and 20) and their two-hop chains, but both
  serialize with origin name `late_inline` (one merged `INLINE_ORIGIN`).
