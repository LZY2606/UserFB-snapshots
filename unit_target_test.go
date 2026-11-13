package govaluate

import (
	"errors"
	"fmt"
	"reflect"
	"regexp"
	"sync"
	"testing"

	"github.com/Knetic/govaluate/support/contract"
)

/*
	Parameters implementation that records every Get call, in order, so tests
	can assert exactly which parameters a row touched and in which sequence.
*/
type traceParams struct {
	log    *[]string
	values map[string]interface{}
	errs   map[string]error
}

func (this *traceParams) Get(name string) (interface{}, error) {

	*this.log = append(*this.log, name)

	if err, found := this.errs[name]; found {
		return nil, err
	}
	if value, found := this.values[name]; found {
		return value, nil
	}
	return nil, errors.New("No parameter '" + name + "' found.")
}

type rowSpec struct {
	values map[string]interface{}
	errs   map[string]error
}

func makeTraceRows(specs []rowSpec) ([]Parameters, []*[]string) {

	rows := make([]Parameters, len(specs))
	logs := make([]*[]string, len(specs))
	for i, spec := range specs {
		log := &[]string{}
		logs[i] = log
		rows[i] = &traceParams{log: log, values: spec.values, errs: spec.errs}
	}
	return rows, logs
}

func resetLogs(logs []*[]string) {
	for _, log := range logs {
		*log = []string{}
	}
}

func snapshotLogs(logs []*[]string) [][]string {

	ret := make([][]string, len(logs))
	for i, log := range logs {
		ret[i] = append([]string{}, (*log)...)
	}
	return ret
}

// cross-checks batch results against per-row scalar Eval on the same parameters.
func checkAgainstEval(t *testing.T, expr *EvaluableExpression, values []interface{}, errs []error, rows []Parameters) {

	t.Helper()
	contract.Check(t, len(values) == len(rows))
	contract.Check(t, len(errs) == len(rows))

	for i, parameters := range rows {
		expectedValue, expectedErr := expr.Eval(parameters)
		if expectedErr != nil {
			contract.Check(t, errs[i] != nil)
			if errs[i] != nil {
				contract.Check(t, errs[i].Error() == expectedErr.Error())
			}
			contract.Check(t, values[i] == nil)
		} else {
			contract.Check(t, errs[i] == nil)
			contract.Check(t, reflect.DeepEqual(values[i], expectedValue))
		}
	}
}

func mustCompile(t *testing.T, expr *EvaluableExpression) *BatchPlan {

	t.Helper()
	plan, err := expr.CompileBatch()
	contract.Check(t, err == nil)
	if err != nil {
		t.FailNow()
	}
	return plan
}

func TestUnitBatch(t *testing.T) {

	t.Run("ArithmeticMixedRows", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("a * 2 + 1")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"a": int32(3)}},
				{values: map[string]interface{}{"a": 5.5}},
				{values: map[string]interface{}{"a": "s"}},
				{values: map[string]interface{}{}},
			})
			rows = append(rows, nil) // nil Parameters behaves like Eval(nil)

			values, errs, stats := plan.Eval(rows)

			// explicit expectations
			contract.Check(t, len(values) == 5 && len(errs) == 5)
			contract.Check(t, reflect.DeepEqual(values[0], float64(7)))
			contract.Check(t, reflect.DeepEqual(values[1], float64(12)))
			contract.Check(t, values[2] == nil && errs[2] != nil)
			contract.Check(t, errs[2].Error() == "Value 's' cannot be used with the modifier '*', it is not a number")
			contract.Check(t, values[3] == nil && errs[3] != nil)
			contract.Check(t, errs[3].Error() == "No parameter 'a' found.")
			contract.Check(t, values[4] == nil && errs[4] != nil)
			contract.Check(t, errs[4].Error() == "No parameter 'a' found.")

			// stats: 5 instructions (a, 2, *, 1, +); lanes shrink as rows fail
			contract.Check(t, stats.Instructions == 5)
			contract.Check(t, stats.Dispatches == 5)
			contract.Check(t, stats.ActiveLanes == uint64(5+3+3+2+2))

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("StringsRegexInAndBitwise", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("first + ' ' + last")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"first": "foo", "last": "bar"}},
				{values: map[string]interface{}{"first": "solo"}},
			})
			values, errs, _ := plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], "foo bar"))
			contract.Check(t, values[1] == nil && errs[1] != nil)
			checkAgainstEval(t, expr, values, errs, rows)

			expr, err = NewEvaluableExpression("name =~ '^a.*'")
			contract.Check(t, err == nil)
			plan = mustCompile(t, expr)
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"name": "alpha"}},
				{values: map[string]interface{}{"name": "beta"}},
				{values: map[string]interface{}{"name": 1.5}},
			})
			values, errs, _ = plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], true))
			contract.Check(t, reflect.DeepEqual(values[1], false))
			contract.Check(t, values[2] == nil && errs[2] != nil)
			checkAgainstEval(t, expr, values, errs, rows)

			expr, err = NewEvaluableExpression("name !~ pattern")
			contract.Check(t, err == nil)
			plan = mustCompile(t, expr)
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"name": "alpha", "pattern": regexp.MustCompile("^a")}},
				{values: map[string]interface{}{"name": "beta", "pattern": regexp.MustCompile("^a")}},
			})
			values, errs, _ = plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], false))
			contract.Check(t, reflect.DeepEqual(values[1], true))
			checkAgainstEval(t, expr, values, errs, rows)

			expr, err = NewEvaluableExpression("code in (10, 20, 30)")
			contract.Check(t, err == nil)
			plan = mustCompile(t, expr)
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"code": 20}},
				{values: map[string]interface{}{"code": 15}},
			})
			values, errs, _ = plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], true))
			contract.Check(t, reflect.DeepEqual(values[1], false))
			checkAgainstEval(t, expr, values, errs, rows)

			bitwiseCases := []struct {
				expression string
				expected   float64
			}{
				{"flags & 3", 1},
				{"flags | 8", 13},
				{"flags ^ 1", 4},
				{"flags << 1", 10},
				{"flags >> 1", 2},
				{"~flags", -6},
			}
			for _, bitwiseCase := range bitwiseCases {
				expr, err = NewEvaluableExpression(bitwiseCase.expression)
				contract.Check(t, err == nil)
				plan = mustCompile(t, expr)
				rows, _ = makeTraceRows([]rowSpec{
					{values: map[string]interface{}{"flags": 5}},
					{values: map[string]interface{}{"flags": "nope"}},
				})
				values, errs, _ = plan.Eval(rows)
				contract.Check(t, reflect.DeepEqual(values[0], bitwiseCase.expected))
				contract.Check(t, values[1] == nil && errs[1] != nil)
				checkAgainstEval(t, expr, values, errs, rows)
			}
		})
	})

	t.Run("FunctionsAndErrorIdentity", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			sentinel := errors.New("sentinel function failure")
			functions := map[string]ExpressionFunction{
				"sum3": func(args ...interface{}) (interface{}, error) {
					return args[0].(float64) + args[1].(float64) + args[2].(float64), nil
				},
				"fail": func(args ...interface{}) (interface{}, error) {
					return nil, sentinel
				},
			}

			expr, err := NewEvaluableExpressionWithFunctions("sum3(a, b, c) + 1", functions)
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"a": 1, "b": 2, "c": 3}},
				{values: map[string]interface{}{"a": 10, "b": 20, "c": 30}},
				{values: map[string]interface{}{"a": 1, "b": 2}},
			})
			values, errs, _ := plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], float64(7)))
			contract.Check(t, reflect.DeepEqual(values[1], float64(61)))
			contract.Check(t, values[2] == nil && errs[2] != nil)
			checkAgainstEval(t, expr, values, errs, rows)

			expr, err = NewEvaluableExpressionWithFunctions("x > 0 ? fail() : 9", functions)
			contract.Check(t, err == nil)
			plan = mustCompile(t, expr)
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"x": 1}},
				{values: map[string]interface{}{"x": -1}},
			})
			values, errs, _ = plan.Eval(rows)
			// the exact error object returned by the user function is preserved
			contract.Check(t, errs[0] == sentinel)
			contract.Check(t, values[0] == nil)
			contract.Check(t, reflect.DeepEqual(values[1], float64(9)))
			contract.Check(t, errs[1] == nil)
			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("AccessorsAndMethods", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			accessorCases := []struct {
				expression string
				parameters map[string]interface{}
				expected   interface{}
			}{
				{"foo.String", map[string]interface{}{"foo": dummyParameterInstance}, "string!"},
				{"foo.Int", map[string]interface{}{"foo": dummyParameterInstance}, float64(101)},
				{"foo.Func()", map[string]interface{}{"foo": dummyParameterInstance}, "funk"},
				{"foo.Func2()", map[string]interface{}{"foo": dummyParameterInstance}, "frink"},
				{"fooptr.Func3()", map[string]interface{}{"fooptr": &dummyParameterInstance}, "fronk"},
				{"foo.FuncArgStr('bar')", map[string]interface{}{"foo": dummyParameterInstance}, "bar"},
				{"foo.Nested.Funk", map[string]interface{}{"foo": dummyParameterInstance}, "funkalicious"},
				{"foo.Nested.Dunk('x')", map[string]interface{}{"foo": dummyParameterInstance}, "xdunk"},
			}

			for _, accessorCase := range accessorCases {
				expr, err := NewEvaluableExpression(accessorCase.expression)
				contract.Check(t, err == nil)
				plan := mustCompile(t, expr)

				rows, _ := makeTraceRows([]rowSpec{
					{values: accessorCase.parameters},
					{values: map[string]interface{}{}},
				})
				values, errs, _ := plan.Eval(rows)
				contract.Check(t, errs[0] == nil)
				contract.Check(t, reflect.DeepEqual(values[0], accessorCase.expected))
				contract.Check(t, values[1] == nil && errs[1] != nil)
				checkAgainstEval(t, expr, values, errs, rows)
			}

			// accessor failure on one row must not affect the others
			expr, err := NewEvaluableExpression("foo.Nope")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)
			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"foo": dummyParameterInstance}},
			})
			values, errs, _ := plan.Eval(rows)
			contract.Check(t, values[0] == nil && errs[0] != nil)
			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("StatefulParametersReadPerOccurrence", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("x + x")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			// each Get returns an incrementing value; a cached-by-name batch
			// would produce 2 instead of 3 and read only once.
			counters := make([]int, 2)
			rows := make([]Parameters, 2)
			for i := range rows {
				index := i
				rows[i] = parameterFunc(func(name string) (interface{}, error) {
					counters[index]++
					return float64(counters[index]), nil
				})
			}

			values, errs, _ := plan.Eval(rows)
			contract.Check(t, errs[0] == nil && errs[1] == nil)
			contract.Check(t, reflect.DeepEqual(values[0], float64(3)))
			contract.Check(t, reflect.DeepEqual(values[1], float64(3)))
			contract.Check(t, counters[0] == 2 && counters[1] == 2)
		})
	})

	t.Run("EmptyBatch", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("a * 2 + 1")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			values, errs, stats := plan.Eval([]Parameters{})
			contract.Check(t, len(values) == 0)
			contract.Check(t, len(errs) == 0)
			contract.Check(t, stats.Instructions == 5)
			contract.Check(t, stats.Dispatches == 0)
			contract.Check(t, stats.ActiveLanes == 0)
		})
	})
}

func TestUnitMasks(t *testing.T) {

	t.Run("AndSkipsRightSide", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("flag && missing > 0")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, logs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": true, "missing": 5}},
				{values: map[string]interface{}{"flag": true}},
				{values: map[string]interface{}{"flag": false, "missing": 1}},
			})

			values, errs, _ := plan.Eval(rows)

			contract.Check(t, reflect.DeepEqual(values[0], false) && errs[0] == nil)
			contract.Check(t, reflect.DeepEqual(values[1], true) && errs[1] == nil)
			contract.Check(t, values[2] == nil && errs[2] != nil)
			contract.Check(t, reflect.DeepEqual(values[3], false) && errs[3] == nil)

			// rows with flag == false must never read "missing"
			expectedSequences := [][]string{
				{"flag"},
				{"flag", "missing"},
				{"flag", "missing"},
				{"flag"},
			}
			for i := range logs {
				contract.Check(t, reflect.DeepEqual(*logs[i], expectedSequences[i]))
			}

			// scalar evaluation produces the same per-row access sequences
			scalarRows, scalarLogs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": true, "missing": 5}},
				{values: map[string]interface{}{"flag": true}},
				{values: map[string]interface{}{"flag": false, "missing": 1}},
			})
			for i, row := range scalarRows {
				expr.Eval(row)
				contract.Check(t, reflect.DeepEqual(*scalarLogs[i], expectedSequences[i]))
			}

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("OrSkipsFallbackCall", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			var calls []interface{}
			functions := map[string]ExpressionFunction{
				"fallback": func(args ...interface{}) (interface{}, error) {
					calls = append(calls, args[0])
					return args[0], nil
				},
			}

			expr, err := NewEvaluableExpressionWithFunctions("ok || fallback(x)", functions)
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, logs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"ok": true, "x": true}},
				{values: map[string]interface{}{"ok": false, "x": true}},
				{values: map[string]interface{}{"ok": false, "x": false}},
			})

			values, errs, _ := plan.Eval(rows)

			contract.Check(t, reflect.DeepEqual(values[0], true) && errs[0] == nil)
			contract.Check(t, reflect.DeepEqual(values[1], true) && errs[1] == nil)
			contract.Check(t, reflect.DeepEqual(values[2], false) && errs[2] == nil)

			// the short-circuited row never reads x and never calls fallback
			contract.Check(t, reflect.DeepEqual(*logs[0], []string{"ok"}))
			contract.Check(t, reflect.DeepEqual(*logs[1], []string{"ok", "x"}))
			contract.Check(t, reflect.DeepEqual(*logs[2], []string{"ok", "x"}))
			contract.Check(t, reflect.DeepEqual(calls, []interface{}{true, false}))

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("CoalesceChainLanes", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("a ?? b ?? c")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, logs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"a": 1, "b": 2, "c": 3}},
				{values: map[string]interface{}{"a": nil, "b": 2, "c": 3}},
				{values: map[string]interface{}{"a": nil, "b": nil, "c": 3}},
				{values: map[string]interface{}{"a": nil, "b": nil, "c": nil}},
			})

			values, errs, _ := plan.Eval(rows)

			contract.Check(t, reflect.DeepEqual(values[0], float64(1)) && errs[0] == nil)
			contract.Check(t, reflect.DeepEqual(values[1], float64(2)) && errs[1] == nil)
			contract.Check(t, reflect.DeepEqual(values[2], float64(3)) && errs[2] == nil)
			// every alternative nil: nil result, but no error
			contract.Check(t, values[3] == nil && errs[3] == nil)

			expectedSequences := [][]string{
				{"a"},
				{"a", "b"},
				{"a", "b", "c"},
				{"a", "b", "c"},
			}
			for i := range logs {
				contract.Check(t, reflect.DeepEqual(*logs[i], expectedSequences[i]))
			}

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("NestedTernaryLanes", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			var mu sync.Mutex
			var calls []string
			record := func(name string) ExpressionFunction {
				return func(args ...interface{}) (interface{}, error) {
					mu.Lock()
					calls = append(calls, fmt.Sprintf("%s:%v", name, args[0]))
					mu.Unlock()
					return fmt.Sprintf("%s(%v)", name, args[0]), nil
				}
			}
			functions := map[string]ExpressionFunction{
				"then1": record("then1"),
				"then2": record("then2"),
				"else1": record("else1"),
			}

			expr, err := NewEvaluableExpressionWithFunctions("c1 ? (c2 ? then1(x) : then2(y)) : else1(z)", functions)
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, logs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"c1": true, "c2": true, "x": "x0", "y": "y0", "z": "z0"}},
				{values: map[string]interface{}{"c1": true, "c2": false, "x": "x1", "y": "y1", "z": "z1"}},
				{values: map[string]interface{}{"c1": false, "c2": true, "x": "x2", "y": "y2", "z": "z2"}},
			})

			values, errs, _ := plan.Eval(rows)

			contract.Check(t, reflect.DeepEqual(values[0], "then1(x0)") && errs[0] == nil)
			contract.Check(t, reflect.DeepEqual(values[1], "then2(y1)") && errs[1] == nil)
			contract.Check(t, reflect.DeepEqual(values[2], "else1(z2)") && errs[2] == nil)

			// only the taken branch is read; row 2 must not even read c2
			contract.Check(t, reflect.DeepEqual(*logs[0], []string{"c1", "c2", "x"}))
			contract.Check(t, reflect.DeepEqual(*logs[1], []string{"c1", "c2", "y"}))
			contract.Check(t, reflect.DeepEqual(*logs[2], []string{"c1", "z"}))

			// exactly one function call per row, from the taken branch only
			contract.Check(t, reflect.DeepEqual(calls, []string{"then1:x0", "then2:y1", "else1:z2"}))

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("ErrorIsolationBetweenRows", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			sentinelK := errors.New("k blew up")
			sentinelM := errors.New("m blew up")

			expr, err := NewEvaluableExpression("k > 0 && m > 0")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			rows, logs := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"k": 1, "m": 1}},
				{values: map[string]interface{}{"m": 1}, errs: map[string]error{"k": sentinelK}},
				{values: map[string]interface{}{"k": 0}},
				{values: map[string]interface{}{"k": 2}, errs: map[string]error{"m": sentinelM}},
			})

			values, errs, _ := plan.Eval(rows)

			contract.Check(t, reflect.DeepEqual(values[0], true) && errs[0] == nil)
			// error identity is preserved exactly
			contract.Check(t, errs[1] == sentinelK && values[1] == nil)
			contract.Check(t, reflect.DeepEqual(values[2], false) && errs[2] == nil)
			contract.Check(t, errs[3] == sentinelM && values[3] == nil)

			// the row failing on k never touches m
			contract.Check(t, reflect.DeepEqual(*logs[1], []string{"k"}))
			contract.Check(t, reflect.DeepEqual(*logs[2], []string{"k"}))
			contract.Check(t, reflect.DeepEqual(*logs[3], []string{"k", "m"}))

			checkAgainstEval(t, expr, values, errs, rows)
		})
	})

	t.Run("AccessSequenceMatchesScalar", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			logsByArg := map[string]*[]string{}
			functions := map[string]ExpressionFunction{
				"f": func(args ...interface{}) (interface{}, error) {
					key := args[0].(string)
					if log, found := logsByArg[key]; found {
						*log = append(*log, "f:"+key)
					}
					return 10.0, nil
				},
			}

			expr, err := NewEvaluableExpressionWithFunctions("(a + b) * f(c) - a / d", functions)
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			specs := []rowSpec{
				{values: map[string]interface{}{"a": 8, "b": 2, "c": "c0", "d": 4}},
				{values: map[string]interface{}{"a": 1, "b": 1, "c": "c1", "d": 2}},
			}
			rows, logs := makeTraceRows(specs)
			for i := range rows {
				logsByArg[fmt.Sprintf("c%d", i)] = logs[i]
			}

			values, errs, _ := plan.Eval(rows)
			contract.Check(t, errs[0] == nil && errs[1] == nil)
			// (8+2)*10 - 8/4 = 98 and (1+1)*10 - 1/2 = 19.5
			contract.Check(t, reflect.DeepEqual(values[0], float64(98)))
			contract.Check(t, reflect.DeepEqual(values[1], float64(19.5)))
			batchSequences := snapshotLogs(logs)

			// explicit per-row sequence: a, b, c, f, a, d
			for i := range rows {
				expected := []string{"a", "b", "c", fmt.Sprintf("f:c%d", i), "a", "d"}
				contract.Check(t, reflect.DeepEqual(batchSequences[i], expected))
			}

			// scalar Eval on fresh rows produces the identical sequence per row
			resetLogs(logs)
			for i, row := range rows {
				scalarValue, scalarErr := expr.Eval(row)
				contract.Check(t, scalarErr == nil)
				contract.Check(t, reflect.DeepEqual(*logs[i], batchSequences[i]))
				contract.Check(t, reflect.DeepEqual(values[i], scalarValue))
			}
		})
	})
}

func TestUnitPlan(t *testing.T) {

	t.Run("CompileRules", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			// zero-value expression: ChecksTypes was never enabled, rejected
			zero := &EvaluableExpression{}
			_, err := zero.CompileBatch()
			contract.Check(t, err != nil)

			// parsed expression with type checking explicitly disabled, rejected
			expr, err := NewEvaluableExpression("1 + 1")
			contract.Check(t, err == nil)
			expr.ChecksTypes = false
			_, err = expr.CompileBatch()
			contract.Check(t, err != nil)

			// ChecksTypes enabled but no stages: compiles, yields nil, nil per row
			empty := &EvaluableExpression{ChecksTypes: true}
			plan, err := empty.CompileBatch()
			contract.Check(t, err == nil)
			values, errs, stats := plan.Eval([]Parameters{nil, nil, nil})
			contract.Check(t, len(values) == 3 && len(errs) == 3)
			for i := range values {
				contract.Check(t, values[i] == nil && errs[i] == nil)
			}
			contract.Check(t, stats.Instructions == 0 && stats.Dispatches == 0 && stats.ActiveLanes == 0)
		})
	})

	t.Run("PlanCapturedAtCompileTime", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("a * 2")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			// changing the expression's public configuration after compilation
			// must not change the plan: type checks still apply.
			expr.ChecksTypes = false

			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"a": 3}},
				{values: map[string]interface{}{"a": "s"}},
			})
			values, errs, _ := plan.Eval(rows)
			contract.Check(t, reflect.DeepEqual(values[0], float64(6)))
			contract.Check(t, errs[0] == nil)
			contract.Check(t, values[1] == nil && errs[1] != nil)
			contract.Check(t, errs[1].Error() == "Value 's' cannot be used with the modifier '*', it is not a number")
		})
	})

	t.Run("DispatchAndLaneStats", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			expr, err := NewEvaluableExpression("flag && missing > 0")
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			// leaf(flag), split, leaf(missing), leaf(0), apply(>), merge(&&)
			contract.Check(t, planInstructionCount(plan) == 6)

			rows, _ := makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": true, "missing": 1}},
			})
			_, _, stats := plan.Eval(rows)
			contract.Check(t, stats.Instructions == 6)
			contract.Check(t, stats.Dispatches == 6)
			contract.Check(t, stats.ActiveLanes == uint64(3+3+1+1+1+1))

			// doubling the records doubles ActiveLanes, not Dispatches
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": true, "missing": 1}},
				{values: map[string]interface{}{"flag": true, "missing": 2}},
			})
			_, _, stats = plan.Eval(rows)
			contract.Check(t, stats.Dispatches == 6)
			contract.Check(t, stats.ActiveLanes == uint64(6+6+2+2+2+2))

			// every row short-circuits: right-side instructions never dispatch
			rows, _ = makeTraceRows([]rowSpec{
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
				{values: map[string]interface{}{"flag": false}},
			})
			values, errs, stats := plan.Eval(rows)
			contract.Check(t, stats.Dispatches == 2)
			contract.Check(t, stats.ActiveLanes == 8)
			for i := range values {
				contract.Check(t, reflect.DeepEqual(values[i], false) && errs[i] == nil)
			}

			// empty batch: no dispatches at all
			_, _, stats = plan.Eval([]Parameters{})
			contract.Check(t, stats.Instructions == 6)
			contract.Check(t, stats.Dispatches == 0)
			contract.Check(t, stats.ActiveLanes == 0)
		})
	})

	t.Run("ConcurrentBatchesIsolated", func(t *testing.T) {
		contract.Run(t, func(t *testing.T) {

			var captureMu sync.Mutex
			var captured [][]interface{}
			functions := map[string]ExpressionFunction{
				"pick": func(args ...interface{}) (interface{}, error) {
					captureMu.Lock()
					captured = append(captured, args)
					captureMu.Unlock()
					return args[0], nil
				},
			}

			expr, err := NewEvaluableExpressionWithFunctions("x * 2 + pick(a, b, c)", functions)
			contract.Check(t, err == nil)
			plan := mustCompile(t, expr)

			firstRows := []Parameters{
				MapParameters{"x": 1.0, "a": 10.0, "b": 20.0, "c": 30.0},
				MapParameters{"x": 2.0, "a": 40.0, "b": 50.0, "c": 60.0},
			}
			firstValues, firstErrs, _ := plan.Eval(firstRows)
			contract.Check(t, reflect.DeepEqual(firstValues[0], float64(12)))
			contract.Check(t, reflect.DeepEqual(firstValues[1], float64(44)))
			contract.Check(t, firstErrs[0] == nil && firstErrs[1] == nil)
			firstSnapshot := append([]interface{}{}, firstValues...)

			captureMu.Lock()
			capturedBefore := len(captured)
			captureMu.Unlock()
			contract.Check(t, capturedBefore == 2)

			// run many overlapping batches on the same plan
			var wait sync.WaitGroup
			failures := make(chan string, 64)
			for worker := 0; worker < 8; worker++ {
				wait.Add(1)
				go func(worker int) {
					defer wait.Done()
					rows := make([]Parameters, 4)
					for i := range rows {
						base := float64(worker*100 + i)
						rows[i] = MapParameters{"x": base, "a": base + 1, "b": base + 2, "c": base + 3}
					}
					values, errs, _ := plan.Eval(rows)
					for i := range rows {
						base := float64(worker*100 + i)
						if errs[i] != nil || !reflect.DeepEqual(values[i], base*2+base+1) {
							failures <- fmt.Sprintf("worker %d row %d: %v %v", worker, i, values[i], errs[i])
						}
					}
				}(worker)
			}
			wait.Wait()
			close(failures)
			for failure := range failures {
				contract.Check(t, false)
				t.Log(failure)
			}

			// earlier results and captured argument lists are untouched
			contract.Check(t, reflect.DeepEqual(firstValues, firstSnapshot))
			captureMu.Lock()
			contract.Check(t, reflect.DeepEqual(captured[0], []interface{}{float64(10), float64(20), float64(30)}))
			contract.Check(t, reflect.DeepEqual(captured[1], []interface{}{float64(40), float64(50), float64(60)}))
			contract.Check(t, len(captured) == 2+8*4)
			captureMu.Unlock()
		})
	})
}

// parameterFunc adapts a function to the Parameters interface.
type parameterFunc func(name string) (interface{}, error)

func (this parameterFunc) Get(name string) (interface{}, error) {
	return this(name)
}

// planInstructionCount exposes the compiled instruction count for assertions.
func planInstructionCount(plan *BatchPlan) int {
	return len(plan.instructions)
}
