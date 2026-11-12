# AMD64 PE single-frame unwind (`File.UnwindAMD64`)

`pe.File.UnwindAMD64` reconstructs **one** previous stack frame from an
offline crash snapshot of an AMD64 PE32+ image, using the
`IMAGE_RUNTIME_FUNCTION_ENTRY` / `UNWIND_INFO` metadata carried in the
image's exception directory. It is intended for post-mortem tooling
that has a CPU snapshot and a memory reader and needs the caller's
register state without a live process.

## What this is — and what it is not

This is a **restricted, read-only single-frame recovery** routine:

* The PE image is opened purely as data. No machine code is ever
  executed and no language-specific handler (`UNW_FLAG_EHANDLER`,
  `UNW_FLAG_UHANDLER`, …) is ever invoked. A handler RVA is recorded in
  the result for inspection only.
* It is **not an exception dispatcher**. It does not decide whether an
  exception should be handled, does not call unwind/termination handlers,
  and does not walk multiple C++/SEH frames through handler logic.
  `CHAIN_INFO` parents are followed, because they are pure metadata, but
  only until the first terminal record, and the ordinary return address
  is read exactly once.

## Scope and preconditions

| Item | Support |
| --- | --- |
| Image format | PE32+ (`OptionalHeader64`), machine `IMAGE_FILE_MACHINE_AMD64` |
| `UNWIND_INFO` version | version 1 only; other versions return `ErrAMD64UnwindNotSupported` |
| PC location | caller asserts the PC is **not in an epilogue** |
| Op codes | `PUSH_NONVOL`, `ALLOC_SMALL`, `ALLOC_LARGE` (both encodings), `SET_FPREG`, `SAVE_NONVOL`, `SAVE_NONVOL_FAR`, `PUSH_MACHFRAME` (with and without an error code), `CHAIN_INFO` |
| XMM saves (`SAVE_XMM*`), `SET_FPREG_LARGE`, unknown op codes | `ErrAMD64UnwindNotSupported` |
| Functions with no directory entry | leaf rules: RIP = `[RSP]`, RSP += 8 |

Epilogue unwinding cannot be done from metadata alone — it requires
decoding instructions, which this package never does. The caller makes
the "not in an epilogue" assertion explicitly by leaving
`AMD64Snapshot.InEpilogue` false; setting it returns
`ErrAMD64UnwindNotSupported` instead of guessing.

A present but corrupt exception directory never silently degrades to
leaf handling: malformed directory sizes, illegal `RUNTIME_FUNCTION`
entries, overlapping ranges, unmapped xdata, illegal slot counts,
cyclic chains, arithmetic wrap-around and short/failing memory reads all
return an error that names the offending RVA, slot or virtual address.
On error no partial frame is published, and neither the input snapshot
nor the `File` is modified.

## Register numbering

`AMD64Snapshot.Reg` and the `Reg` field of decoded operations use the
Windows x64 `UNWIND_CODE` register numbering:

| Number | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Register | RAX | RCX | RDX | RBX | RSP | RBP | RSI | RDI | R8 | R9 | R10 | R11 | R12 | R13 | R14 | R15 |

Named constants `AMD64RegRAX` … `AMD64RegR15` are provided.

## Result contents

`UnwindAMD64` returns an `AMD64UnwindResult`:

* `Frame` — an independently allocated previous-frame snapshot. Only
  registers the metadata describes change; every other register keeps
  its input value.
* `Function` — the selected `RUNTIME_FUNCTION`, or `nil` together with
  `Leaf == true` when leaf rules were used.
* `Records` — every `UNWIND_INFO` processed (the record selected by the
  PC first, then each `CHAIN_INFO` parent), with decoded operations,
  the frame-register fields and any handler RVA.
* `Applied` — the operations actually reversed, in the order they were
  applied. Operations skipped because the PC is still inside the
  prolog, and chain markers, are not included.
* `Reads` — every memory callback access (`VA`, `Size`) in order, giving
  a full provenance trail for the values placed into the frame.

## Prolog filtering and address stability

When the PC lies inside the prolog, only operations whose
`CodeOffset` is not greater than the PC-relative offset are reversed.
Multi-slot operations (`ALLOC_LARGE`, `SAVE_NONVOL_FAR`, …) are parsed
as one operation with a real `Slots` width, so an unexecuted one is
skipped as a unit; extension slots are never mistaken for new
operations.

`SAVE_NONVOL` addresses are relative to the base of the frame's fixed
allocation area (RSP at function entry). That base is computed once and
is not allowed to drift while `ALLOC`/`PUSH` operations are undone.
When `SET_FPREG` has executed, RSP is first re-derived from the frame
register (absorbing dynamic, `alloca`-style stack allocation); before
`SET_FPREG` has executed the frame register is deliberately not used.

`PUSH_MACHFRAME` restores RIP and the original RSP from the prescribed
trap-frame slots and terminates the record, so no extra return-address
pop happens.

## Example: a byte-snapshot memory callback

A crash dump typically stores memory as disjoint byte ranges rather
than one contiguous buffer. The callback below maps a virtual address
into a set of byte-snapshot blocks.

```go
package main

import (
	"errors"
	"fmt"
	"io/ioutil"
	"os"

	"github.com/Binject/debug/pe"
)

// memBlock is one contiguous byte range captured from the target
// process, recorded at its original virtual address.
type memBlock struct {
	base uint64
	data []byte
}

// byteSnapshot implements pe.MemReaderAt over sparse byte blocks.
type byteSnapshot struct {
	blocks []memBlock
}

func (s *byteSnapshot) ReadAtVA(p []byte, va uint64) (int, error) {
	for _, b := range s.blocks {
		if va >= b.base && va+uint64(len(p)) <= b.base+uint64(len(b.data)) {
			copy(p, b.data[va-b.base:])
			return len(p), nil
		}
	}
	return 0, fmt.Errorf("virtual address %#x is not present in the snapshot", va)
}

// A function adapter is also available:
//   reader := pe.MemReaderAtFunc(func(p []byte, va uint64) (int, error) { ... })
var _ pe.MemReaderAt = (*byteSnapshot)(nil)

func main() {
	imagePath := os.Args[1]
	f, err := pe.Open(imagePath)
	if err != nil {
		panic(err)
	}
	defer f.Close()

	// Values as captured from the faulting thread's context.
	const loadBase = 0x7ff6_0000_0000 // actual load base, not ImageBase
	snap := pe.AMD64Snapshot{
		RIP: 0x7ff6_0001_4a20,
		RSP: 0x9f2_4d8,
		Reg: pe.AMD64RegisterSet{
			pe.AMD64RegRBX: 0x100,
			pe.AMD64RegRBP: 0x9f2_520,
			// ... the remaining integer registers
		},
		// Leave false after confirming externally that RIP is not in an
		// epilogue. Set true to make UnwindAMD64 return
		// pe.ErrAMD64UnwindNotSupported instead of guessing.
		InEpilogue: false,
	}

	mem := &byteSnapshot{
		blocks: []memBlock{
			// Stack region containing saved registers and return addresses.
			{base: 0x9f2_000, data: mustRead(os.Args[2])},
		},
	}

	res, err := f.UnwindAMD64(loadBase, snap, mem)
	if err != nil {
		if errors.Is(err, pe.ErrAMD64UnwindNotSupported) {
			// Epilogue PC, XMM save, unsupported version/opcode, ...
			fmt.Println("unwind not supported for this frame:", err)
			return
		}
		panic(err) // corrupt metadata or a failed/short memory read
	}

	fmt.Printf("previous RIP=%#x RSP=%#x leaf=%t\n", res.Frame.RIP, res.Frame.RSP, res.Leaf)
	for i, r := range res.Reads {
		fmt.Printf("read %d: %d bytes at %#x\n", i, r.Size, r.VA)
	}
}

func mustRead(path string) []byte {
	b, err := ioutil.ReadFile(path)
	if err != nil {
		panic(err)
	}
	return b
}
```

The same `File` can be opened from an ordinary on-disk image with
`pe.NewFile`/`pe.Open`, or from a loader-mapped image with
`pe.NewFileFromMemory`; both layouts resolve RVAs through the section
readers and produce identical unwind results. Directory entries and the
xdata they reference need not live in the same section.

## Guarantees

* Read-only: imports, relocations, section contents and every write-out
  interface are unaffected by repeated `UnwindAMD64` calls.
* No panics on malformed input: every validation failure is an
  `error` carrying a source position.
* Deterministic: unwind state is kept inside the call; a failed unwind
  leaves the `File` usable for a subsequent successful unwind.
