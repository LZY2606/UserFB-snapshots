// This example demonstrates the portable program cache: a cleaning rule is
// compiled and encoded once, then loaded and executed against a different
// record layout — different struct names, field order and anonymous
// embedding — without recompiling the expression.
package main

import (
	"fmt"
	"reflect"

	"github.com/expr-lang/expr"
)

// Layout used by the compiling (rule authoring) side.
type ItemV1 struct {
	Active bool
	Value  int
}

type RecordV1 struct {
	Left  []ItemV1
	Right []ItemV1
	Scale int
}

// Layout used by the offline execution side: the struct names differ, the
// fields are reordered, extra fields appear, and Value is exposed through
// anonymous embedding on one side only.
type Base struct {
	Value int
}

type LeftItemV2 struct {
	Value  int
	Active bool
}

type RightItemV2 struct {
	Base
	Pad    int
	Active bool
}

type RecordV2 struct {
	Scale int
	Right []RightItemV2
	Left  []LeftItemV2
}

const rule = `let selected = filter(Left, {#.Active});
[map(selected, {#.Value * Scale}), map(Right, {#.Value * Scale})]`

func main() {
	// Compile and encode the rule once.
	program, err := expr.Compile(rule, expr.Env(RecordV1{}))
	if err != nil {
		panic(err)
	}
	cache, err := expr.EncodeProgram(program)
	if err != nil {
		panic(err)
	}
	fmt.Printf("encoded cache: %d bytes\n", len(cache))

	// ... the cache bytes are shipped with the job package ...

	// The execution process loads the cache against its own record layout.
	// No compilation happens here.
	loaded, err := expr.DecodeProgram(cache, expr.Env(RecordV2{}))
	if err != nil {
		panic(err)
	}
	got, err := expr.Run(loaded, RecordV2{
		Scale: 2,
		Left:  []LeftItemV2{{3, true}, {100, false}, {5, true}},
		Right: []RightItemV2{{Base{7}, 99, true}},
	})
	if err != nil {
		panic(err)
	}
	fmt.Printf("execution layout result: %v\n", got)

	// The original layout keeps working with the same cache.
	loadedV1, err := expr.DecodeProgram(cache, expr.Env(RecordV1{}))
	if err != nil {
		panic(err)
	}
	want, err := expr.Run(loadedV1, RecordV1{
		Left:  []ItemV1{{true, 3}, {false, 100}, {true, 5}},
		Right: []ItemV1{{true, 7}},
		Scale: 2,
	})
	if err != nil {
		panic(err)
	}
	fmt.Printf("compile layout result:   %v\n", want)

	if !reflect.DeepEqual(got, want) {
		panic("results differ between layouts")
	}
	fmt.Println("both layouts agree")
}
