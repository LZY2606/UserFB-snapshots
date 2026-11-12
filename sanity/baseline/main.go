package main

import (
	"fmt"
	"github.com/expr-lang/expr"
	"github.com/expr-lang/expr/vm"
	"github.com/expr-lang/expr/vm/runtime"
	"reflect"
)

type Item struct {
	Active bool
	Value  int
}
type Input struct {
	Left  []Item
	Right []Item
	Scale int
}
type LeftV2 struct {
	Value  int
	Active bool
}
type RightV2 struct {
	Pad    int
	Value  int
	Active bool
}

//go:noinline
func offset(n int) func(...any) (any, error) {
	return func(a ...any) (any, error) { return a[0].(int) + n, nil }
}

func must(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}
func main() {
	input := Input{Left: []Item{{true, 3}, {false, 100}, {true, 5}}, Right: []Item{{true, 7}}, Scale: 2}
	text := `let selected = filter(Left, {#.Active}); [map(selected, {#.Value * Scale}), map(Right, {#.Value * Scale})]`
	for _, opt := range []bool{false, true} {
		p, e := expr.Compile(text, expr.Env(input), expr.Optimize(opt))
		if e != nil {
			panic(e)
		}
		v, e := expr.Run(p, input)
		if e != nil {
			panic(e)
		}
		must(reflect.DeepEqual(v, []any{[]any{6, 10}, []any{14}}), fmt.Sprintf("bad output: %#v", v))
		fmt.Printf("COMPILED_LOOP_OK optimize=%t result=%v instructions=%d\n", opt, v, len(p.Bytecode))
		if !opt {
			slots := map[int]int{}
			for pc, op := range p.Bytecode {
				if op == vm.OpFetchField {
					f := p.Constants[p.Arguments[pc]].(*runtime.Field)
					if reflect.DeepEqual(f.Path, []string{"Value"}) {
						slots[p.Arguments[pc]]++
					}
				}
			}
			shared := false
			for _, n := range slots {
				if n >= 2 {
					shared = true
				}
			}
			must(shared, "expected shared Value field descriptor between distinct collection roles")
			fmt.Println("FIELD_ROLE_WITNESS shared-constant=true left-role!=right-role")
		}
	}
	oldField := &runtime.Field{Index: []int{1}, Path: []string{"Value"}}
	left := runtime.FetchField(LeftV2{Value: 3, Active: true}, oldField)
	right := runtime.FetchField(RightV2{Pad: 99, Value: 7, Active: true}, oldField)
	must(left == true && right == 7, "unexpected field index witness")
	fmt.Printf("LAYOUT_WITNESS stale-left=%v stale-right=%v logical-values=3,7\n", left, right)
	a, b := offset(1), offset(10)
	must(reflect.ValueOf(a).Pointer() == reflect.ValueOf(b).Pointer(), "expected same closure code address")
	p, e := expr.Compile(`first(x) + second(x) + len(split(trim(s), ",")) + len(repeat(s, 2))`, expr.Env(map[string]any{"x": 2, "s": " a,b "}), expr.Function("first", a), expr.Function("second", b))
	if e != nil {
		panic(e)
	}
	out, e := expr.Run(p, map[string]any{"x": 2, "s": " a,b "})
	if e != nil {
		panic(e)
	}
	must(out == 27, fmt.Sprint(out))
	fmt.Printf("CALL_BINDING_WITNESS same-code=true distinct-results=true result=%v\n", out)
	fmt.Println("EXPR_CAPSULE_BASELINE_OK loops=2 roles=2 bindings=2")
}
