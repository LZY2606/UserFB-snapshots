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

## Editable generation trees (RecordTree / Replay / Minimize)

Sometimes a fixed-seed value is only the starting point: you want to shrink a
failing input down to a smaller structure while keeping the *identity* of the
parts that survive. Lowering `NumElements` and regenerating instead re-draws
every later field, and deleting one nested element renumbers everything else.
The generation-tree mode records one run as an editable tree and lets you
reduce that tree directly.

```go
type packet struct {
    Items []int32
    Tail  int64
}

f := fuzz.NewWithSeed(1234).NilChance(0).NumElements(4, 4)
tree, err := f.RecordTree(&packet{}) // packet is not modified

var first packet
tree.Replay(&first) // full value rebuilt on a fresh zero value

reduced, calls, err := tree.Minimize(func(x interface{}) (bool, error) {
    return len(x.(*packet).Items) >= 1, nil // the failure still reproduces
}, 1000)

data, _ := reduced.MarshalBinary()                 // save the shrunk record
reloaded, _ := fuzz.DecodeGenerationTree(data, &packet{})
```

A complete, runnable version is `Example_generationTree` in
`generation_tree_test.go`.

### Recording and replay

`RecordTree(dst)` takes a non-nil pointer only to learn the type; `dst` is
never written. Inside the fuzzer lock it first validates the type and
configuration, then draws **exactly one** `Int63()` from the fuzzer's random
source; that value is the root seed. A rejected record consumes no
randomness.

Every node has its own independent `rand.New(rand.NewSource(seed))` stream.
A child seed is derived from its parent seed and a *stable slot* (struct
field index, element index, rune index, or `0` for a pointed-to value) using
the FNV-style mixing

```
s = ((parentSeed*1099511628211) XOR mix(slot)) XOR 1469598103934665603
mix(slot) = (slot*11400714819323198485) XOR (slot<<7 | slot>>57)   // mod 2^64
```

with the high bit cleared. The derivation and the binary record layout are
tied to `treeFormatVersion` (currently `1`); records carrying any other
version are rejected. Because slots are positions rather than element
indices, deleting slice elements or string runes never re-seeds or
renumbers the survivors.

`Replay(dst)` drives the same reflection dispatch and the same scalar
generators (`randBool`, `randUint64`, `randString`) used by `Fuzz`, but from
the recorded seeds only: it needs no `Fuzzer` and reads no external random
source. It builds a complete value on a fresh zero object and only assigns it
to `*dst` after succeeding, so an invalid record or a type mismatch leaves
`dst` untouched. The ordinary `Fuzz`, `FuzzNoCustom` and `NewFromGoFuzz`
paths are unchanged.

The supported type subset is `bool`, all signed/unsigned integers (including
named types), `string`, and the `struct`, `array`, `slice` and `pointer`
types built from them. Recursive types are supported and are bounded by
`MaxDepth`. `map`, `interface`, floats, complex numbers, `chan`, `func` and
`unsafe.Pointer` are unsupported, as are custom fuzz functions,
`fuzz.Interface`, default fuzz functions and `AllowUnexportedFields`; if any
reachable, non-skipped field needs them, `RecordTree` returns
`ErrTreeUnsupportedType` with the field path before drawing the root seed.
Skipped and unexported fields are simply left at zero. `NilChance`,
`NumElements`, `MaxDepth` and the skip rules are frozen into the record and
constrain only the initial generation.

### Minimization

`Minimize(keep, maxCalls)` returns the last accepted tree, the number of
`keep` invocations (the mandatory initial check counts as call one) and an
error. Candidates are enumerated in node preorder: slice and string
contiguous-interval deletions (longest first, then left-to-right; strings are
edited by rune), slice and pointer nil-ing, `true`→`false`, and non-zero
integers to zero then halved toward zero without overflowing. Each accepted
candidate restarts the enumeration; the result is reduced but not necessarily
globally minimal. Structural edits may drop below the original
`NumElements` and may produce nils. Every `keep` call replays into a brand
new object, so mutating that object cannot leak into the tree or into later
calls. `false` never replaces the current tree; a `keep` error or an
exhausted budget returns the last accepted tree together with the call count.

### Persistence, ownership and limits

`MarshalBinary` writes a versioned, self-describing, canonical big-endian
record containing only the format version, the bound static type name, the
frozen policy and the tree nodes (ids, slots, seeds and explicit leaf
overrides). It never stores machine addresses or live Go objects, so one
valid tree always encodes to identical bytes and `decode → encode` is stable.
`DecodeGenerationTree(data, prototype)` uses `prototype` only for its element
type (it is never modified), consumes no randomness, and rejects truncation,
trailing bytes, unknown versions, duplicate ids, broken parent/child or kind
relationships and out-of-range leaf values before returning a usable tree.

`GenerationTree` and `GenerationNode` are immutable snapshots. The node
getters expose the stable `ID`, `Kind`, `Slot`, `Seed`, scalar values and a
copied, ordered `Children()` view through read-only methods; there is no way
to mutate a tree through them. `Minimize` always returns a freshly allocated,
detached tree and leaves its receiver unchanged, so the caller may retain any
tree or node view indefinitely without aliasing later reductions. Records are
rejected beyond `200000` nodes, depth `500`, or `64 MiB` encoded.

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
