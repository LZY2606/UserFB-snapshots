# Label debug names

The `name` custom section may carry a **Label** subsection (id `3`) that names
the structured control nodes of a function. It is an indirect, per-function
name table, like the Local subsection. This document describes how the static
reader (`ReadBinaryIr`) turns those bytes into the labels printed by
`WriteWat`, how automatic names (`GenerateNames`) coexist with them, and how
`ApplyNames` keeps every branch bound to its original control node.

The pipeline only reads the module and prints text; it never instantiates the
module or executes a function.

## Binary layout

The Label subsection mirrors the Local subsection:

```
label_subsection := subsection_id(=3) subsection_size
                    vec(func_label_names)
func_label_names := func_index vec(label_name)
label_name       := label_index name
```

`name` is a length-prefixed UTF-8 string. An **empty** name provides no name
for the node and is ignored.

## Function index space

`func_index` is the module-wide function index and **includes imported
functions** (the same index space used by function names and call
instructions). Imported functions have no bodies, so they have no structured
labels; a non-empty label table attached to an imported function is rejected.

## Label numbering (open order)

Within a function, `label_index` numbers the block-like control instructions
in the order their opening opcodes appear in the binary. Only the three
structured instructions in scope participate:

- `block`
- `loop`
- `if`

Numbering starts at zero for each function.

- The implicit function exit label is not numbered.
- `else` and `end` do not occupy a slot.
- For an `if`, the nodes nested in the **true arm** and the nodes nested in
  the **false arm** all take part in the same sequence. The whole true arm is
  numbered before the false arm, matching binary order.

For example:

```wat
(block $a        ;; label 0
  (loop $b       ;; label 1
    (if $c       ;; label 2
      (then (block $d))   ;; label 3 (true arm)
      (else (loop $e))    ;; label 4 (false arm)
    )
  )
)
```

This numbering is independent of the relative **depth** immediate carried by
branch instructions (`br`, `br_if`, `br_table`). A depth counts outward from
the branch's current label stack at runtime; the label index above is a fixed
position in the function's open-order sequence. The two must not be confused.

A branch whose depth reaches the function exit keeps pointing at the function
exit; it is never rebound to a named node.

## Name-section placement

A custom section can appear either between the function declaration section
and the code section, or after the code section. Label names are buffered
while reading and applied to the already-built control tree at the end of the
module, so both positions produce identical node names and identical branch
relationships. The caller does not need to bind anything a second time.

The buffered strings are copied out of the input bytes, so naming and text
output still work after the input buffer is released.

## Conflict rules

Names use the same character convention as function and local debug names: a
`$` prefix is added when the name is stored, and the WAT writer rewrites any
character that is not legal in a text identifier.

Conflicts are resolved **per function**, in binary open order:

- The first node that keeps a debug name keeps it verbatim.
- If a later node in the same function is given the same name, it receives the
  smallest unused `.N` suffix: `.1`, then `.2`, and so on.

Given three nested blocks all named `dup`, the resulting labels are
`$dup`, `$dup.1`, `$dup.2`.

## Interaction with `GenerateNames`

When only some nodes are named, `GenerateNames` fills in the rest with the
usual `$B…`, `$L…`, `$I…` names. Before generating, every identifier already
present on a block-like node — including debug names attached to nodes that
have not been visited yet — is reserved. An automatic name that would collide
with any reserved identifier is disambiguated with its own `_N` suffix, so an
automatic name can never shadow a debug name, even one on a later node.

Debug names are never deleted wholesale, and name conversion is never
disabled, to dodge a collision.

## `ApplyNames` and branch identity

`ApplyNames` converts each numeric branch depth into a label name by walking
the same label stack the binary depth refers to. It therefore does not change
which node a branch targets:

- every `br` and `br_if`,
- every `br_table` entry and its `default`,

resolve through the textual scope back to the original control node. Branches
that target the implicit function exit remain numeric and continue to target
the exit.

Round-tripping the produced WAT back through the text parser and
`ResolveNames` reconstructs the same numeric depths, so the branch target
identity matches the original binary.

## Options and error handling

- With `read_debug_names = false`, label names are ignored, exactly as before.
  Unnamed nodes and numeric branch depths remain fully usable, and ordinary
  modules, function names and local names keep working.
- With `fail_on_custom_section_error = true` (the default), malformed label
  data produces a positioned error, including:
  - duplicate or out-of-order function indices,
  - duplicate or out-of-order label indices,
  - truncated names or subsection payloads,
  - out-of-range function or label indices,
  - a non-empty label table on an imported function.
- With the flag off, such custom-section problems are downgraded to warnings
  and reading continues where possible.
- A failed read does not carry state into a later, independent read.
