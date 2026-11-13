package expr_test

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/expr-lang/expr"
	"github.com/expr-lang/expr/conf"
	"github.com/expr-lang/expr/file"
)

// This file tests the portable program cache: expr.EncodeProgram and
// expr.DecodeProgram. Compiled rules are encoded here, the bytes are passed
// to a subprocess (the same test binary re-run as TestCapsuleWorker) which
// loads them with expr.DecodeProgram — without ever calling expr.Compile —
// and runs them against a differently laid out environment.

// Compile-time record layout.
type capsuleItem struct {
	Active bool
	Value  int
}

type capsuleMeta struct {
	Factor int
	Name   string
}

type capsuleEnv struct {
	Left  []capsuleItem
	Right []capsuleItem
	Scale int
	Meta  *capsuleMeta
}

// Execution-time layouts: struct names, field order and anonymous embedding
// depth all differ from the compile-time layout. Left and Right use
// different element types; Value is exposed through different embedding
// depths.
type capsuleLeftBase struct {
	Value int
}

type capsuleLeftMid struct {
	capsuleLeftBase
	Other int
}

type capsuleLeftItem struct {
	capsuleLeftMid
	Active bool
}

type capsuleRightBase struct {
	Value int
}

type capsuleRightItem struct {
	capsuleRightBase
	Pad    int
	Active bool
}

type capsuleMetaV2 struct {
	Name   string
	Factor int
}

type capsuleEnvV2 struct {
	Scale int
	Right []capsuleRightItem
	Left  []capsuleLeftItem
	Meta  *capsuleMetaV2
}

func capsuleEnvV2Value() capsuleEnvV2 {
	return capsuleEnvV2{
		Scale: 2,
		Left: []capsuleLeftItem{
			{capsuleLeftMid{capsuleLeftBase{3}, 0}, true},
			{capsuleLeftMid{capsuleLeftBase{100}, 0}, false},
			{capsuleLeftMid{capsuleLeftBase{5}, 0}, true},
		},
		Right: []capsuleRightItem{
			{capsuleRightBase{7}, 99, true},
		},
		Meta: &capsuleMetaV2{Name: "meta", Factor: 3},
	}
}

var capsuleRelinkRules = []string{
	// let aliasing a filtered collection, shared #.Value descriptor between
	// the Left and Right roles.
	`let selected = filter(Left, {#.Active}); [map(selected, {#.Value * Scale}), map(Right, {#.Value * Scale})]`,
	// direct map(filter(...), ...) which Optimize(true) fuses.
	`[map(filter(Left, {#.Active}), {#.Value * Scale}), map(Right, {#.Value * Scale})]`,
	// nested predicates over different collections.
	`filter(Left, {#.Active && any(Right, {#.Value > 6})})`,
	// reduce with initial values.
	`reduce(Left, {#.Value + #acc}, 0) + reduce(Right, {#acc + #.Value}, 100)`,
	`all(Left, {#.Active || #.Value > 3}) && any(Right, {#.Value == 7})`,
	// let inside a predicate, referenced from a nested predicate.
	`filter(Left, { let v = #.Value; v > 1 && any(Right, {#.Value < v}) })`,
	// ternary, short-circuit, comparisons, builtins.
	`Scale > 1 ? map(Left, {#.Value + Scale}) : map(Right, {#.Value})`,
	`Meta.Factor * Scale`,
	`Meta.Name == "meta" && len(Left) > 0`,
	// collection elements in expressions.
	`Right[0].Value + Left[0].Value`,
	// element-returning builtins and optional chaining through pointers.
	`first(Left).Value + last(Right).Value + find(Right, {#.Active}).Value`,
	`(Meta?.Factor ?? -1) + $env.Scale`,
}

func capsuleJSON(v any) string {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// capsuleRunWorker passes encoded program bytes to a subprocess running
// TestCapsuleWorker and returns the content of the result file the worker
// writes. The subprocess never recompiles the expression.
func capsuleRunWorker(t *testing.T, mode string, data []byte) []byte {
	t.Helper()
	out := filepath.Join(t.TempDir(), "out")
	cmd := exec.Command(os.Args[0], "-test.run=^TestCapsuleWorker$")
	cmd.Env = append(os.Environ(),
		"EXPR_CAPSULE_WORKER="+mode,
		"EXPR_CAPSULE_OUT="+out,
	)
	cmd.Stdin = bytes.NewReader(data)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("capsule worker (%s) failed: %v", mode, err)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("capsule worker (%s) wrote no result: %v", mode, err)
	}
	return b
}

// TestCapsuleWorker is the subprocess half of the capsule tests. It decodes
// the program cache from stdin and runs it; it must not recompile anything.
func TestCapsuleWorker(t *testing.T) {
	mode := os.Getenv("EXPR_CAPSULE_WORKER")
	if mode == "" {
		t.Skip("not a worker process")
	}
	out := os.Getenv("EXPR_CAPSULE_OUT")
	data, err := io.ReadAll(os.Stdin)
	if err != nil {
		t.Fatal(err)
	}
	write := func(b []byte) {
		if err := os.WriteFile(out, b, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	switch mode {
	case "relink":
		p, err := expr.DecodeProgram(data, expr.Env(capsuleEnvV2{}))
		if err != nil {
			t.Fatal(err)
		}
		got, err := expr.Run(p, capsuleEnvV2Value())
		if err != nil {
			t.Fatal(err)
		}
		write([]byte(capsuleJSON(got)))

	case "reencode":
		p, err := expr.DecodeProgram(data, expr.Env(capsuleEnvV2{}))
		if err != nil {
			t.Fatal(err)
		}
		again, err := expr.EncodeProgram(p)
		if err != nil {
			t.Fatal(err)
		}
		write(again)

	case "bindings":
		env := func() map[string]any { return map[string]any{"x": 2, "s": " a,b "} }
		p1, err := expr.DecodeProgram(data, expr.Env(env()),
			expr.Function("first", capsuleOffset(1)),
			expr.Function("second", capsuleOffset(10)))
		if err != nil {
			t.Fatal(err)
		}
		// The same cache linked against a different set of closures.
		p2, err := expr.DecodeProgram(data, expr.Env(env()),
			expr.Function("first", capsuleOffset(100)),
			expr.Function("second", capsuleOffset(1000)))
		if err != nil {
			t.Fatal(err)
		}
		var results []any
		// Interleaved runs.
		for i := 0; i < 3; i++ {
			r1, err := expr.Run(p1, env())
			if err != nil {
				t.Fatal(err)
			}
			r2, err := expr.Run(p2, env())
			if err != nil {
				t.Fatal(err)
			}
			results = append(results, r1, r2)
		}
		// Concurrent runs of both programs.
		const n = 8
		concurrent := make([]any, n)
		errs := make([]error, n)
		var wg sync.WaitGroup
		for i := 0; i < n; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				if i%2 == 0 {
					concurrent[i], errs[i] = expr.Run(p1, env())
				} else {
					concurrent[i], errs[i] = expr.Run(p2, env())
				}
			}(i)
		}
		wg.Wait()
		for _, err := range errs {
			if err != nil {
				t.Fatal(err)
			}
		}
		results = append(results, concurrent...)
		write([]byte(capsuleJSON(results)))

	default:
		t.Fatalf("unknown worker mode %q", mode)
	}
}

//go:noinline
func capsuleOffset(n int) func(...any) (any, error) {
	return func(a ...any) (any, error) { return a[0].(int) + n, nil }
}

func TestCapsuleRelink(t *testing.T) {
	for _, optimize := range []bool{false, true} {
		for _, rule := range capsuleRelinkRules {
			t.Run(fmt.Sprintf("optimize=%v", optimize), func(t *testing.T) {
				program, err := expr.Compile(rule, expr.Env(capsuleEnv{}), expr.Optimize(optimize))
				if err != nil {
					t.Fatal(err)
				}
				data, err := expr.EncodeProgram(program)
				if err != nil {
					t.Fatalf("EncodeProgram(%q): %v", rule, err)
				}

				// Independent expectation: compile and run the same rule
				// against the new layout in this process.
				fresh, err := expr.Compile(rule, expr.Env(capsuleEnvV2{}), expr.Optimize(optimize))
				if err != nil {
					t.Fatal(err)
				}
				want, err := expr.Run(fresh, capsuleEnvV2Value())
				if err != nil {
					t.Fatal(err)
				}

				// Load the cache in this process and run it on the new layout.
				loaded, err := expr.DecodeProgram(data, expr.Env(capsuleEnvV2{}))
				if err != nil {
					t.Fatalf("DecodeProgram(%q): %v", rule, err)
				}
				got, err := expr.Run(loaded, capsuleEnvV2Value())
				if err != nil {
					t.Fatalf("Run(%q): %v", rule, err)
				}
				if !reflect.DeepEqual(got, want) {
					t.Fatalf("rule %q: got %#v, want %#v", rule, got, want)
				}

				// The encoded bytes must survive an actual process boundary:
				// the worker subprocess loads them without recompiling.
				if got := string(capsuleRunWorker(t, "relink", data)); got != capsuleJSON(want) {
					t.Fatalf("rule %q: subprocess got %s, want %s", rule, got, capsuleJSON(want))
				}
			})
		}
	}
}

func TestCapsuleBindings(t *testing.T) {
	env := func() map[string]any { return map[string]any{"x": 2, "s": " a,b "} }

	// Two closures from the same factory share their code address but capture
	// different values; each must be linked independently.
	first, second := capsuleOffset(1), capsuleOffset(10)
	if reflect.ValueOf(first).Pointer() != reflect.ValueOf(second).Pointer() {
		t.Fatal("expected the two closures to share their code address")
	}

	rule := `first(x) + second(x) + len(split(trim(s), ",")) + len(repeat(s, 2))`
	program, err := expr.Compile(rule, expr.Env(env()),
		expr.Function("first", first),
		expr.Function("second", second))
	if err != nil {
		t.Fatal(err)
	}
	data, err := expr.EncodeProgram(program)
	if err != nil {
		t.Fatal(err)
	}

	// The same cache linked against two different function sets must produce
	// two independent programs that can run interleaved and concurrently.
	// first(x)+second(x) = 3+12, len(split(...)) = 2, len(repeat(...)) = 10.
	want := make([]any, 0, 14)
	for i := 0; i < 3; i++ {
		want = append(want, 27, 1116)
	}
	for i := 0; i < 4; i++ {
		want = append(want, 27, 1116)
	}
	if got := string(capsuleRunWorker(t, "bindings", data)); got != capsuleJSON(want) {
		t.Fatalf("subprocess bindings: got %s, want %s", got, capsuleJSON(want))
	}

	// In-process: the cache linked with the original functions behaves like
	// the original program, and the original program is not polluted by
	// other linkings.
	loaded, err := expr.DecodeProgram(data, expr.Env(env()),
		expr.Function("first", capsuleOffset(1)),
		expr.Function("second", capsuleOffset(10)))
	if err != nil {
		t.Fatal(err)
	}
	other, err := expr.DecodeProgram(data, expr.Env(env()),
		expr.Function("first", capsuleOffset(100)),
		expr.Function("second", capsuleOffset(1000)))
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		got, err := expr.Run(loaded, env())
		if err != nil || got != 27 {
			t.Fatalf("got %v, %v", got, err)
		}
		got, err = expr.Run(other, env())
		if err != nil || got != 1116 {
			t.Fatalf("got %v, %v", got, err)
		}
		got, err = expr.Run(program, env())
		if err != nil || got != 27 {
			t.Fatalf("original program polluted: got %v, %v", got, err)
		}
	}

	// Missing extension function.
	if _, err := expr.DecodeProgram(data, expr.Env(env()),
		expr.Function("first", capsuleOffset(1))); err == nil || !strings.Contains(err.Error(), "second") {
		t.Fatalf("expected missing function error, got %v", err)
	}
	// Disabled builtins: fast (len), func (trim, split) and safe (repeat)
	// call forms must all be rejected.
	for _, name := range []string{"len", "trim", "split", "repeat"} {
		_, err := expr.DecodeProgram(data, expr.Env(env()),
			expr.Function("first", capsuleOffset(1)),
			expr.Function("second", capsuleOffset(10)),
			expr.DisableBuiltin(name))
		if err == nil || !strings.Contains(err.Error(), name) {
			t.Fatalf("expected disabled builtin %q error, got %v", name, err)
		}
	}
	// After the failures the same cache still loads.
	if _, err := expr.DecodeProgram(data, expr.Env(env()),
		expr.Function("first", capsuleOffset(1)),
		expr.Function("second", capsuleOffset(10))); err != nil {
		t.Fatalf("reload after failures: %v", err)
	}

	// An extension function shadowing a builtin name is cached as an
	// extension function; loading must not silently bind the builtin.
	shadow, err := expr.Compile(`trim(s)`, expr.Env(env()),
		expr.Function("trim", func(args ...any) (any, error) {
			return len(args[0].(string)), nil
		}))
	if err != nil {
		t.Fatal(err)
	}
	shadowData, err := expr.EncodeProgram(shadow)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := expr.DecodeProgram(shadowData, expr.Env(env())); err == nil || !strings.Contains(err.Error(), "trim") {
		t.Fatalf("expected error instead of binding the builtin trim, got %v", err)
	}

	// Declared signatures are recorded and checked.
	typed, err := expr.Compile(`double(x)`, expr.Env(env()),
		expr.Function("double", capsuleOffset(0), new(func(int) int)))
	if err != nil {
		t.Fatal(err)
	}
	typedData, err := expr.EncodeProgram(typed)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := expr.DecodeProgram(typedData, expr.Env(env()),
		expr.Function("double", capsuleOffset(0), new(func(int) int))); err != nil {
		t.Fatalf("matching signature rejected: %v", err)
	}
	if _, err := expr.DecodeProgram(typedData, expr.Env(env()),
		expr.Function("double", capsuleOffset(0), new(func(string) string))); err == nil {
		t.Fatal("expected signature mismatch error")
	}
	if _, err := expr.DecodeProgram(typedData, expr.Env(env()),
		expr.Function("double", capsuleOffset(0))); err == nil {
		t.Fatal("expected signature mismatch error for undeclared types")
	}

	// Functions with more than three arguments (OpLoadFunc + OpCallN).
	sum5 := func(args ...any) (any, error) {
		n := 0
		for _, a := range args {
			n += a.(int)
		}
		return n, nil
	}
	wide, err := expr.Compile(`add5(1, 2, 3, 4, 5)`, expr.Env(env()), expr.Function("add5", sum5))
	if err != nil {
		t.Fatal(err)
	}
	wideData, err := expr.EncodeProgram(wide)
	if err != nil {
		t.Fatal(err)
	}
	loadedWide, err := expr.DecodeProgram(wideData, expr.Env(env()), expr.Function("add5", sum5))
	if err != nil {
		t.Fatal(err)
	}
	if got, err := expr.Run(loadedWide, env()); err != nil || got != 15 {
		t.Fatalf("got %v, %v", got, err)
	}
}

func TestCapsuleRoundTrip(t *testing.T) {
	env := func() map[string]any {
		return map[string]any{"x": 3, "s": "b", "xs": []int{1, 2, 3}, "n": int64(5)}
	}
	rules := []string{
		`[1, 2.5, "three", true]`, // folded []any constant
		`"text" matches "^t"`,     // compiled regexp constant
		`x in [1, 2, 3]`,          // map[int]struct{} constant
		`s in ["a", "b"]`,         // map[string]struct{} constant
		`reduce(xs, {#acc + #})`,  // error constant for the empty case
		`x > 1 ? "big" : "small"`, // ternary
		`let y = x * 2; y + 1`,    // variable slots
		`nil == nil && x != 4`,    // nil, comparisons, short-circuit
		`{"k": x, "l": [s]}`,      // runtime map/array construction
		`n + 1`,                   // int64 constant
	}
	for _, optimize := range []bool{false, true} {
		for _, rule := range rules {
			program, err := expr.Compile(rule, expr.Env(env()), expr.Optimize(optimize))
			if err != nil {
				t.Fatal(err)
			}
			data, err := expr.EncodeProgram(program)
			if err != nil {
				t.Fatalf("EncodeProgram(%q): %v", rule, err)
			}
			// Encoding the same program twice yields the same bytes.
			again, err := expr.EncodeProgram(program)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(data, again) {
				t.Fatalf("EncodeProgram(%q) is not deterministic", rule)
			}

			loaded, err := expr.DecodeProgram(data, expr.Env(env()))
			if err != nil {
				t.Fatalf("DecodeProgram(%q): %v", rule, err)
			}
			// Bytecode, variable slots and source locations are preserved.
			if !reflect.DeepEqual(program.Bytecode, loaded.Bytecode) {
				t.Fatalf("rule %q: bytecode changed", rule)
			}
			if program.Variables() != loaded.Variables() {
				t.Fatalf("rule %q: variable slots changed", rule)
			}
			if !reflect.DeepEqual(program.Locations(), loaded.Locations()) {
				t.Fatalf("rule %q: locations changed", rule)
			}

			want, err := expr.Run(program, env())
			if err != nil {
				t.Fatal(err)
			}
			got, err := expr.Run(loaded, env())
			if err != nil {
				t.Fatalf("Run(%q): %v", rule, err)
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("rule %q: got %#v, want %#v", rule, got, want)
			}

			// A loaded program encodes back to the original bytes.
			reencoded, err := expr.EncodeProgram(loaded)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(data, reencoded) {
				t.Fatalf("rule %q: re-encoded bytes differ", rule)
			}

			// The loaded program does not borrow the caller's data slice.
			fresh, err := expr.DecodeProgram(data, expr.Env(env()))
			if err != nil {
				t.Fatal(err)
			}
			for i := range data {
				data[i] = 0
			}
			if _, err := expr.Run(fresh, env()); err != nil {
				t.Fatalf("rule %q: loaded program borrowed the input slice: %v", rule, err)
			}
		}
	}

	// A loaded program encodes back to the original bytes even across a
	// process boundary and a different environment layout.
	rule := capsuleRelinkRules[0]
	program, err := expr.Compile(rule, expr.Env(capsuleEnv{}))
	if err != nil {
		t.Fatal(err)
	}
	data, err := expr.EncodeProgram(program)
	if err != nil {
		t.Fatal(err)
	}
	if got := capsuleRunWorker(t, "reencode", data); !bytes.Equal(got, data) {
		t.Fatal("subprocess re-encoded bytes differ from the original cache")
	}

	// Runtime errors keep their position, snippet and cause after loading.
	boom := errors.New("capsule boom")
	call := `x + bad(s)`
	callEnv := func() map[string]any { return map[string]any{"x": 1, "s": "y"} }
	bad := expr.Function("bad", func(args ...any) (any, error) { return nil, boom })
	failing, err := expr.Compile(call, expr.Env(callEnv()), bad)
	if err != nil {
		t.Fatal(err)
	}
	failingData, err := expr.EncodeProgram(failing)
	if err != nil {
		t.Fatal(err)
	}
	loadedFailing, err := expr.DecodeProgram(failingData, expr.Env(callEnv()), bad)
	if err != nil {
		t.Fatal(err)
	}
	_, wantErr := expr.Run(failing, callEnv())
	_, gotErr := expr.Run(loadedFailing, callEnv())
	if wantErr == nil || gotErr == nil {
		t.Fatal("expected runtime errors")
	}
	if !errors.Is(gotErr, boom) {
		t.Fatalf("errors.Is lost the cause: %v", gotErr)
	}
	var wantFileErr, gotFileErr *file.Error
	if !errors.As(wantErr, &wantFileErr) || !errors.As(gotErr, &gotFileErr) {
		t.Fatalf("expected *file.Error, got %T and %T", wantErr, gotErr)
	}
	if wantFileErr.Line != gotFileErr.Line || wantFileErr.Column != gotFileErr.Column {
		t.Fatalf("error position changed: %d:%d vs %d:%d",
			gotFileErr.Line, gotFileErr.Column, wantFileErr.Line, wantFileErr.Column)
	}
	if wantFileErr.Snippet != gotFileErr.Snippet {
		t.Fatalf("error snippet changed: %q vs %q", gotFileErr.Snippet, wantFileErr.Snippet)
	}
}

func TestCapsuleReject(t *testing.T) {
	// Features that cannot be relocated reliably are rejected at encode
	// time; nothing is silently dropped.
	t.Run("encode rejects methods", func(t *testing.T) {
		program, err := expr.Compile(`Double(X)`, expr.Env(capsuleMethodEnv{X: 2}))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.EncodeProgram(program); err == nil {
			t.Fatal("expected methods to be rejected")
		}
	})
	t.Run("encode rejects typed env function values", func(t *testing.T) {
		program, err := expr.Compile(`double(x)`, expr.Env(map[string]any{
			"double": func(i int) int { return i * 2 },
			"x":      1,
		}))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.EncodeProgram(program); err == nil {
			t.Fatal("expected typed env function values to be rejected")
		}
	})
	t.Run("encode rejects profiling", func(t *testing.T) {
		program, err := expr.Compile(`1 + 1`, expr.Env(map[string]any{}),
			expr.Option(func(c *conf.Config) { c.Profile = true }))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.EncodeProgram(program); err == nil {
			t.Fatal("expected profiling to be rejected")
		}
	})
	t.Run("encode rejects ConstExpr results", func(t *testing.T) {
		program, err := expr.Compile(`Moment()`, expr.Env(capsuleConstExprEnv{}), expr.ConstExpr("Moment"))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.EncodeProgram(program); err == nil {
			t.Fatal("expected ConstExpr results to be rejected")
		}
	})
	t.Run("encode rejects unknown field sources", func(t *testing.T) {
		program, err := expr.Compile(`(Scale > 1 ? Left : Right)[0].Value`, expr.Env(capsuleEnv{}))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.EncodeProgram(program); err == nil || !strings.Contains(err.Error(), "Value") {
			t.Fatalf("expected unknown source error, got %v", err)
		}
	})

	// A valid cache used for the decode rejection cases below.
	rule := capsuleRelinkRules[0]
	program, err := expr.Compile(rule, expr.Env(capsuleEnv{}))
	if err != nil {
		t.Fatal(err)
	}
	data, err := expr.EncodeProgram(program)
	if err != nil {
		t.Fatal(err)
	}

	t.Run("decode rejects bad magic", func(t *testing.T) {
		bad := bytes.Clone(data)
		bad[0] = 'X'
		if _, err := expr.DecodeProgram(bad, expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected bad magic error")
		}
	})
	t.Run("decode rejects unknown version", func(t *testing.T) {
		bad := bytes.Clone(data)
		bad[4] = 0x7f
		if _, err := expr.DecodeProgram(bad, expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected unknown version error")
		}
	})
	t.Run("decode rejects truncation", func(t *testing.T) {
		if _, err := expr.DecodeProgram(data[:len(data)-3], expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected truncation error")
		}
	})
	t.Run("decode rejects trailing content", func(t *testing.T) {
		bad := append(bytes.Clone(data), 0)
		if _, err := expr.DecodeProgram(bad, expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected trailing content error")
		}
	})
	t.Run("decode rejects corrupt opcode", func(t *testing.T) {
		bad := bytes.Clone(data)
		off, _ := capsuleHeader(bad)
		bad[off] = 0xff
		if _, err := expr.DecodeProgram(bad, expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected corrupt opcode error")
		}
	})
	t.Run("decode rejects huge declarations", func(t *testing.T) {
		var bad []byte
		bad = append(bad, data[:4]...) // magic
		var tmp [binary.MaxVarintLen64]byte
		n := binary.PutUvarint(tmp[:], 1) // version
		bad = append(bad, tmp[:n]...)
		n = binary.PutUvarint(tmp[:], 1<<40) // absurd variable count
		bad = append(bad, tmp[:n]...)
		if _, err := expr.DecodeProgram(bad, expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected huge declaration error")
		}
	})
	t.Run("decode rejects oversized input", func(t *testing.T) {
		if _, err := expr.DecodeProgram(make([]byte, 8<<20+1), expr.Env(capsuleEnvV2{})); err == nil {
			t.Fatal("expected oversized input error")
		}
	})

	// Environment incompatibilities are reported with the logical field path.
	t.Run("decode rejects missing field", func(t *testing.T) {
		type envNoScale struct {
			Left  []capsuleLeftItem
			Right []capsuleRightItem
			Meta  *capsuleMetaV2
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envNoScale{})); err == nil || !strings.Contains(err.Error(), "Scale") {
			t.Fatalf("expected missing field error mentioning Scale, got %v", err)
		}
	})
	t.Run("decode rejects hidden field", func(t *testing.T) {
		type envHidden struct {
			Left  []capsuleLeftItem
			Right []capsuleRightItem
			Scale int `expr:"-"`
			Meta  *capsuleMetaV2
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envHidden{})); err == nil || !strings.Contains(err.Error(), "Scale") {
			t.Fatalf("expected hidden field error mentioning Scale, got %v", err)
		}
	})
	t.Run("decode rejects incompatible leaf type", func(t *testing.T) {
		type envBadLeaf struct {
			Left  []capsuleLeftItem
			Right []capsuleRightItem
			Scale string
			Meta  *capsuleMetaV2
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envBadLeaf{})); err == nil || !strings.Contains(err.Error(), "Scale") {
			t.Fatalf("expected leaf type error mentioning Scale, got %v", err)
		}
	})
	t.Run("decode rejects changed collection shape", func(t *testing.T) {
		type envBadShape struct {
			Left  capsuleLeftItem
			Right []capsuleRightItem
			Scale int
			Meta  *capsuleMetaV2
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envBadShape{})); err == nil || !strings.Contains(err.Error(), "Left") {
			t.Fatalf("expected collection shape error mentioning Left, got %v", err)
		}
	})
	t.Run("decode rejects changed environment kind", func(t *testing.T) {
		if _, err := expr.DecodeProgram(data, expr.Env(map[string]any{
			"Left": []capsuleLeftItem{}, "Right": []capsuleRightItem{}, "Scale": 1,
		})); err == nil {
			t.Fatal("expected environment kind error")
		}
	})
	t.Run("unreferenced recursive types do not block loading", func(t *testing.T) {
		type node struct {
			Next *node
			Name string
		}
		type envRecursive struct {
			Left  []capsuleLeftItem
			Right []capsuleRightItem
			Scale int
			Meta  *capsuleMetaV2
			Root  *node
		}
		loaded, err := expr.DecodeProgram(data, expr.Env(envRecursive{}))
		if err != nil {
			t.Fatal(err)
		}
		got, err := expr.Run(loaded, envRecursive{
			Scale: 2,
			Left: []capsuleLeftItem{
				{capsuleLeftMid{capsuleLeftBase{3}, 0}, true},
				{capsuleLeftMid{capsuleLeftBase{100}, 0}, false},
				{capsuleLeftMid{capsuleLeftBase{5}, 0}, true},
			},
			Right: []capsuleRightItem{{capsuleRightBase{7}, 99, true}},
			Meta:  &capsuleMetaV2{Name: "meta", Factor: 3},
		})
		if err != nil {
			t.Fatal(err)
		}
		if capsuleJSON(got) != `[[6,10],[14]]` {
			t.Fatalf("got %s", capsuleJSON(got))
		}
	})
	t.Run("unreferenced fields may be added or removed", func(t *testing.T) {
		type envExtra struct {
			Left    []capsuleLeftItem
			Right   []capsuleRightItem
			Scale   int
			Meta    *capsuleMetaV2
			Extra   string
			Another []int
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envExtra{})); err != nil {
			t.Fatal(err)
		}
	})
	t.Run("reload after failure", func(t *testing.T) {
		type envNoScale struct {
			Left  []capsuleLeftItem
			Right []capsuleRightItem
			Meta  *capsuleMetaV2
		}
		if _, err := expr.DecodeProgram(data, expr.Env(envNoScale{})); err == nil {
			t.Fatal("expected missing field error")
		}
		loaded, err := expr.DecodeProgram(data, expr.Env(capsuleEnvV2{}))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := expr.Run(loaded, capsuleEnvV2Value()); err != nil {
			t.Fatal(err)
		}
	})
}

// capsuleHeader returns the offset of the opcode stream and the code length
// of an encoded program cache.
func capsuleHeader(data []byte) (off, codeLen int) {
	off = 4 // magic
	_, n := binary.Uvarint(data[off:])
	off += n // version
	_, n = binary.Uvarint(data[off:])
	off += n // variables
	l, n := binary.Uvarint(data[off:])
	off += n // code length
	return off, int(l)
}

type capsuleMethodEnv struct {
	X int
}

func (capsuleMethodEnv) Double(i int) int { return i * 2 }

type capsuleConstExprEnv struct{}

func (capsuleConstExprEnv) Moment() time.Time { return time.Unix(0, 0) }
