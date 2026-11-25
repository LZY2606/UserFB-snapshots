# ReDoS budget regression

`test/redos_budget.c` and `test/redos_model.c` are a C-only security
regression for Oniguruma's three search budgets:

- `ONIGERR_RETRY_LIMIT_IN_MATCH_OVER`
- `ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER`
- `ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER`

The fixed entry point is:

```c
int redos_budget_case(const char *group, unsigned seed, int case_id);
```

`checks/budget_driver.c` runs the four groups `corpus`, `search`, `calls`,
and `reuse`, with 24 fixed case slots per group. The default seed is `2677`;
`--seed`, `--case`, and `--group` are handled by the existing driver. Every
assertion goes through `BUDGET_CHECK` from `checks/budget_check.h`.

Run it with:

```bash
bash checks/run.sh target
```

A filtered singleton is useful after a failure:

```bash
bash checks/run.sh target --group search --case 0
```

## Generation scope

All generated samples are ASCII and use neither case folding nor non-ASCII
encodings. Structures are built first and rendered to valid Oniguruma syntax;
the suite does not concatenate arbitrary strings and discard compilation
failures.

The generators use non-empty fragments with a common prefix:

- `a` and `aa`
- `aa` and `aaa`
- `a` and `aaa`
- `aa` and `aaaa`

These feed:

1. ordered alternation,
2. greedy bounded repetition,
3. ordinary captures,
4. the same ordered branch body reached through `\g<item>`,
5. finite named-group recursion of the form
   `(?<r>a\g<r>?b){0}\g<r>\z`,
6. a two-alternative recursion witness whose first called branch returns and
   is then abandoned by backtracking so the second alternative calls the same
   group again.

Fragment order, repeat lower/upper bounds, input length, reject suffix, and
recursion depth vary with `(group, seed, case_id)`. Generated subjects are at
most 32 bytes, recursion depth is at most 5, and ambiguity fragments are at
most 10 bytes. The default generated inputs are smaller than those ceilings.

Before an unlimited Oniguruma run, `rb_expansion_bound()` gives a
structure-derived conservative decomposition/branch bound. Samples whose
bound exceeds 65536 are marked invalid and are not used for timing-free
control runs. A synthetic `hi=20` case is also asserted to be rejected by this
gate.

`checks/budget_inputs.tsv` is retained only as a small starting corpus. The
`corpus` group reads those four rows as smoke inputs, but the 96 fixed cases
and their seed-derived structures provide the actual coverage.

## Independent finite reference

`redos_model.c` does not read Oniguruma regions or search results to construct
its oracle. It enumerates the generated structure's ordered decompositions:

- greedy repetition tries another repetition before stopping;
- alternatives retain their generated order;
- captures record the whole repeated span and the final successful fragment;
- recursion is evaluated by a separate bounded `a R? b` matcher with the same
  greedy-call-then-backtrack order.

The oracle returns whether a match exists at each byte position, the leftmost
search result, match end, and all ordinary captures. Hard-coded hand examples
include:

| Pattern | Text | Result | Captures |
|---|---|---:|---|
| `((a\|aa)+)\z` | `aaaa` | `[0,4)` | outer `[0,4)`, final branch `[3,4)` |
| `((aa\|a)+)\z` | `aaaa` | `[0,4)` | outer `[0,4)`, final branch `[2,4)` |
| `(?<item>a\|aa){0}(?:\g<item>)+\z` | `aaaa` | `[0,4)` | item `[3,4)` |
| `(?<r>a\g<r>?b){0}\g<r>\z` | `aaabbb` | `[0,6)` | group `[0,6)` |

The `corpus` group compares Oniguruma's forward search and every local
`onig_match_with_param()` result against this independent oracle, including
all capture ranges.

## Observing real start positions

Budget samples are determined mismatches. Their patterns include a
progress-only contents callout, `(?{rb})`. The callout callback only appends
`onig_get_current_by_callout_args()` offsets to a recorder; it always returns
`ONIG_CALLOUT_SUCCESS` and never aborts or consumes an external budget.

The recorded sequence is the source of truth for which starts the VM actually
reached. Existing upstream map, anchor, threshold, and skip-search processing
can remove a prefix or tail before the callout executes. The tests therefore
do not charge skipped starts. They assert that the observed forward sequence is
a hole-free increasing interval, that backward search is its reverse, and that
the interval lies within the range explained by the common `a` prefix and the
generated lower repetition bound. Per-start measurements exist for every legal
byte position, but composition uses only the observed sequence.

## Local costs and independent composition

For every legal position, the suite uses finite threshold probes with
`onig_match_with_param()`:

- retry cost is one less than the smallest nonzero per-match limit that no
  longer returns `ONIGERR_RETRY_LIMIT_IN_MATCH_OVER`;
- subexpression-call cost is the smallest nonzero global call-search limit
  that permits the local match.

The probes are binary searches bounded by the same conservative structural
bound plus a small constant. They do not wait for a runaway process.

The test code, not another `onig_search()` oracle, then walks the observed
start sequence in order and combines those local costs.

### Endpoint rules asserted

For retry checks, upstream increments first and stops at equality with a
nonzero limit:

```c
if (++counter >= limit && limit != 0) ...
```

Therefore a local match containing `R` retry checks:

- aborts with limit `R`;
- completes with limit `R + 1`.

For CALL accounting, upstream increments the search-wide total and stops only
when the total is greater than a nonzero global limit:

```c
if (total > limit) ...
```

Thus a start containing `C >= 2` calls completes at limit `C` and returns
`ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER` at limit `C - 1`. Limit zero is
unlimited for both retry and CALL budgets.

### Combining match and search retry limits

At the beginning of each local match, the effective retry ceiling is the
smaller of:

- the parameter's per-match limit, and
- the remaining search budget `search_limit - accumulated_search_retry`.

The implementation uses strict `<` when replacing the match limit with the
remaining search quota. If the two are equal, the effective value is still the
match limit, and the error ternary attributes the equality step to
`ONIGERR_RETRY_LIMIT_IN_MATCH_OVER`. The tests cover:

- the remaining total quota becoming smaller after an earlier start;
- the local quota being exhausted first;
- equal local and remaining-total quotas, asserting the library's actual
  match-limit attribution;
- forward and complete backward ranges.

The canonical `search` witness chooses one common match limit above every
observed local retry cost but sets the total search limit from the first
start's cost plus part of the second start's cost. Every local match therefore
completes under the same match quota, while the independently composed search
predicts `ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER` at the second observed start.

### Cross-start CALL accumulation

There is no per-match CALL parameter; the nonzero process-global CALL search
limit is compared with a counter stored in the search `MatchArg`. The suite
probes each local call count and sets `G` to the largest local count. Every
start can therefore finish individually at `G`, but the test's cumulative
composition predicts
`ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER` on a later start. The actual
`onig_search_with_param()` result and last callout position are compared with
that prediction.

Retries executed inside a subexpression call remain part of the current
match's retry count; CALL count is the separate cross-start total. Recursion
samples stay at depth at most five while the library's default maximum nest
level remains 20; the tests assert that value and never raise the nesting cap
to hide a cycle. `RB_KIND_RECALL` additionally requires enough CALL executions
to show the second ordered branch invoking the recursive group after the first
called branch has returned and been backtracked.

## Reuse and configuration isolation

The `reuse` group deliberately triggers a budget error, then reuses:

- the same `OnigRegex`,
- the same low-limit `OnigMatchParam`,
- and the same `OnigRegion`,

on a short deterministic success text. It compares the search start and every
capture with newly created parameter and region objects and with the
independent reference.

It then alternates the low-limit object, a default snapshot parameter created
before mutating globals, and a fresh parameter created after restoring the
global retry/CALL settings. The global values are restored and asserted after
each sequence; both request orders are checked. No assertion depends on
capture contents left in the region by the failed budget call.

## Failure artifacts, reduction, and replay

On an unexpected comparison failure, the harness writes under
`REDOS_ARTIFACT_DIR` or `./.budget-failures`:

- `failure.txt` with seed, case, structure, explicit pattern/text lengths,
  pattern and subject, direction, limits, per-start retry/CALL costs, actual
  forward and backward start sequences, and the first difference;
- `original.spec`, a hex-encoded replayable specification;
- up to `RB_REDUCTION_ATTEMPTS` (48) structured reduction attempts;
- `reduced.spec` and an executable `replay.sh` when a preserving reduction is
  found.

The reducer only performs structured mutations:

- shorten the subject / recursion depth,
- lower repetition bounds,
- simplify the ordered branch to one fragment.

A candidate is accepted only if it still compiles, remains a reference
mismatch, reaches at least two actual starts, predicts the same budget error
category at an index beyond the first start, and still exhibits the same
prediction/implementation difference. Cross-start witnesses therefore cannot
be reduced to a single-start over-budget match.

`replay.sh` invokes the fixed driver with the original group, seed, and case
while setting `REDOS_REPLAY_SPEC` to the reduced specification. The replay
re-runs threshold probing and composition rather than trusting saved Oniguruma
state.

## Minimal witnesses

The reducer reaches these two runnable cross-start witnesses:

| Budget | Pattern | Text | Observed starts | Local work | Composition |
|---|---|---|---|---|---|
| retry search | `((a){1,3})\z` | `aa!` | `0,1` | retry `3,2` | `M=4`, `S=4`; start 0 consumes 3, leaving 1, so start 1 must stop with `-18` |
| CALL search | `(?<item>a){0}(?:\g<item>){1,3}\z` | `aa!` | `0,1` | calls `3,2` | `G=3`; start 0 uses all three allowed calls, so the first call of start 1 makes total 4 and must stop with `-19` |

Both retain two actual starts; neither is a single-start over-budget sample.
For the unreduced default seed-2677 case 0 the corresponding canonical
values are `M=67,S=86`, expected retry abort at position 1, and `G=33`,
expected CALL abort at position 1.

## Mutation validation

```bash
bash test/redos_mutations.sh
```

The script makes independent source copies and applies two deliberately
broken patches:

1. delete the `retry_limit_in_search_counter += retry_in_match_counter`
   accumulation after a match returns;
2. reset `subexp_call_in_search_counter` at every search start.

Each copy gets a clean CMake build through `checks/run.sh`. The unmutated copy
must pass the complete fixed target. Each mutant must still build, pass the
baseline successful witness and syntax executable, and then fail the
corresponding singleton with assertions and a concrete expected/actual
budget difference:

- retry mutant: expected `-18`
  (`ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER`) at the second start, actual `-1`
  (`ONIG_MISMATCH`) with no abort position;
- CALL-reset mutant: expected `-19`
  (`ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER`) at a later start, actual `-1`.

Compilation failure, missing files, zero assertions, a skip marker, early
process exit, a signal, or a timeout is not accepted as detection. The script
also runs the generated reducer/replay for both witnesses and requires the
reduced singleton to reproduce the same difference.

Patches, build logs, stdout/stderr, exit codes, first differences, and replay
logs are preserved below:

```text
test/redos_mutation_artifacts/<timestamp>-<pid>/
```

The production library is not modified by this regression. No additional
production defect was found while building these tests; the two rejected
behaviors are deliberate mutation copies. No new budget API and no literal
prefilter/search acceleration are added.

## Boundaries

This is a bounded structural regression, not a proof that every ReDoS input is
covered. Its independence is limited to the small generated grammar and its
finite ordered-decomposition oracle. Local costs are observed through the
library's own threshold endpoints; those observations are then combined by the
test rather than by a second whole-search oracle. The suite proves the stated
composition, endpoint, reuse, and mutation-detection properties for the fixed
and seed-selected cases; it does not establish a universal complexity bound.
