# Forward-origin relations

All offsets are absolute `.debug_info` section offsets, never CU-relative.

## fanout.elf
Concrete subprograms at section offsets 0x2c (range 0x1000) and 0x3d (range 0x1100)
each carry a DW_AT_abstract_origin pointing at the later DIE 0x4e, which is the
inline declaration named `late`. Both concrete instances are waiters of the same
declaration and both resolve to `late`; the self-named DIE 0x18 at range 0x2000
keeps its own name `normal`. The old code keyed one function per forward target, so
the first waiter was overwritten by the second and stayed `<name omitted>`.

## cross-cu.elf
Two concrete subprograms 0x17 and 0x28 share virtual address 0x3000 but cite two
different declarations in the second CU (header at 0x3a): 0x52 `Alpha` and 0x5a
`Beta`. Equal addresses with unresolved names must not be collapsed, so both
functions are retained. The old comparator ordered functions by address then name,
merging the two placeholder-named entries before either declaration was parsed.

## inline_convergence
Inline instances 0x2a (call line 10, range 0x5010) and 0x3c (call line 20, range
0x5020) point through distinct intermediate subprograms 0x4f and 0x55, each of
which has a DW_AT_specification to terminal DIE 0x5b named `late_inline`. Both
concrete instances and both chains remain in relations.tsv even though they share
one terminal: equal names do not make them the same concrete instance. The symbol
output still merges the two InlineOrigin objects by name into one INLINE_ORIGIN
record, and every previously handed-out InlineOrigin pointer stays valid.
