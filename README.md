gofuzz
======

gofuzz is a library for populating go objects with random values.

[![GoDoc](https://godoc.org/github.com/google/gofuzz?status.svg)](https://godoc.org/github.com/google/gofuzz)

This is useful for testing:

* Do your project's objects really serialize/unserialize correctly in all cases?
* Is there an incorrectly formatted object that will cause your project to panic?

Import with ```import "github.com/google/gofuzz"```

You can use it on single variables:
```go
f := fuzz.New()
var myInt int
f.Fuzz(&myInt) // myInt gets a random value.
```

You can use it on maps:
```go
f := fuzz.New().NilChance(0).NumElements(1, 1)
var myMap map[ComplexKeyType]string
f.Fuzz(&myMap) // myMap will have exactly one element.
```

Customize the chance of getting a nil pointer:
```go
f := fuzz.New().NilChance(.5)
var fancyStruct struct {
  A, B, C, D *string
}
f.Fuzz(&fancyStruct) // About half the pointers should be set.
```

You can even customize the randomization completely if needed:
```go
type MyEnum string
const (
        A MyEnum = "A"
        B MyEnum = "B"
)
type MyInfo struct {
        Type MyEnum
        AInfo *string
        BInfo *string
}

f := fuzz.New().NilChance(0).Funcs(
        func(e *MyInfo, c fuzz.Continue) {
                switch c.Intn(2) {
                case 0:
                        e.Type = A
                        c.Fuzz(&e.AInfo)
                case 1:
                        e.Type = B
                        c.Fuzz(&e.BInfo)
                }
        },
)

var myObject MyInfo
f.Fuzz(&myObject) // Type will correspond to whether A or B info is set.
```

See more examples in ```example_test.go```.

## Generation trees: record, minimize, replay

`Fuzzer.RecordTree(&obj)` fills `obj` like `Fuzz` would, but also returns a
`*GenerationTree` that records exactly how the object was generated. The tree
can be replayed byte-for-byte, minimized against a failure condition, and
persisted:

```go
f := fuzz.New().NilChance(0).NumElements(1, 4)
var obj MyType
tree, err := f.RecordTree(&obj)        // obj is filled, tree records how

small, calls, err := tree.Minimize(keep, 1000) // shrink while keep(obj) is true

data, err := small.MarshalBinary()     // self-contained record
back, err := fuzz.DecodeGenerationTree(data, &MyType{}) // prototype gives the type only
var out MyType
err = back.Replay(&out)                // regenerates the recorded object
```

`RecordTree` draws exactly one `Int63` from the Fuzzer's random source (the
root seed) after validating the type and configuration; a failed precheck
consumes no randomness. Every tree node owns an independent random stream.
Child seeds are derived from the parent seed and a stable slot (struct field
index, slice/array element index, 0 for pointer elements) — tree derivation
version 1, frozen by the record format version:

```
x  = uint64(parentSeed) ^ (slot * 0x9E3779B97F4A7C15) ^ 0xD1B54A32D192ED03
x += 0x9E3779B97F4A7C15
x  = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9
x  = (x ^ (x >> 27)) * 0x94D049BB133111EB
x  =  x ^ (x >> 31)
childSeed = int64(x)
```

Because sibling seeds never depend on sibling count or values, deleting a
subtree during minimization preserves the identity, local seeds, order and
values of the survivors.

Supported types: bools, signed/unsigned integers (including named types),
strings, and structs, arrays, slices and pointers built from them, with
recursive types expanded up to `MaxDepth`. Maps, interfaces, floats,
complexes, chans, funcs, `unsafe.Pointer`, custom `Funcs`, `fuzz.Interface`
implementations, default handlers and `AllowUnexportedFields` are rejected by
the precheck with the offending field path. `NilChance`, `NumElements`,
`MaxDepth` and the field skip patterns are frozen into the record; unexported
and skipped fields stay zero.

`Minimize(keep, maxCalls)` enumerates candidates in node preorder: slice and
string contiguous-interval deletions (longest first, left to right; strings
delete by rune), pointer-to-nil, true-to-false, and integer shrinking (zero,
then halving toward zero). `keep` gets a fresh pointer per candidate, may
mutate it freely, and only `true` accepts. The initial check of the original
record counts against the call budget. Minimization is greedy and does not
guarantee a global minimum.

### Record format and ownership

`MarshalBinary` produces a canonical record (same tree, same bytes; decoding
and re-encoding is the identity) containing:

* an 8-byte magic with the format version (`GFTREE01`),
* a static type binding (named types pinned by package path and name,
  unnamed types described structurally),
* the frozen policy (NilChance, NumElements, MaxDepth, skip patterns),
* the preorder node stream: unique node ID, local seed, kind, and either the
  leaf override (for shrunk leaves) or the child count (for containers).

Records hold no machine addresses and no Go object snapshots. Decoding
rejects unsupported versions, type mismatches, duplicate IDs, inconsistent
structure, out-of-range leaf values, truncation and trailing data before
publishing a tree, and never writes to the prototype. Records are limited to
64 MiB, 2^20 nodes and a depth of 1024.

`GenerationTree` and `TreeNode` are immutable once published. The node
accessors (`ID`, `Kind`, `Seed`, `NumChildren`, `Child`, `IsNil`,
`LeafValue`) are the read-only interface; returned nodes are shared with the
owning tree, stay valid while the tree is reachable, and must not be
modified. Trees returned by `Minimize` may share substructure with their
input, which is safe because both are immutable.

## dvyukov/go-fuzz integration

You can use this library for easier [go-fuzz](https://github.com/dvyukov/go-fuzz)ing.
go-fuzz provides the user a byte-slice, which should be converted to different inputs
for the tested function. This library can help convert the byte slice. Consider for
example a fuzz test for a the function `mypackage.MyFunc` that takes an int arguments:
```go
// +build gofuzz
package mypackage

import fuzz "github.com/google/gofuzz"

func Fuzz(data []byte) int {
        var i int
        fuzz.NewFromGoFuzz(data).Fuzz(&i)
        MyFunc(i)
        return 0
}
```

Happy testing!
