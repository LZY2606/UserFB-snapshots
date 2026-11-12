package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"os/exec"
)

func requireTests(file string, names []string) {
	f, err := parser.ParseFile(token.NewFileSet(), file, nil, 0)
	if err != nil {
		panic(err)
	}
	if f.Name.Name != "qframe_test" {
		panic("target package must be qframe_test")
	}
	declared := map[string]bool{}
	for _, d := range f.Decls {
		if fn, ok := d.(*ast.FuncDecl); ok && fn.Recv == nil && fn.Body != nil {
			declared[fn.Name.Name] = true
		}
	}
	for _, n := range names {
		if !declared[n] {
			panic("missing declaration in target file: " + n)
		}
	}
}

func runTests(pattern string, names []string) {
	c := exec.Command("go", "test", "-json", "-count=1", ".", "-run", pattern)
	b, err := c.CombinedOutput()
	os.Stdout.Write(b)
	if err != nil {
		panic(err)
	}
	runs, passed := map[string]bool{}, map[string]bool{}
	s := bufio.NewScanner(bytes.NewReader(b))
	s.Buffer(make([]byte, 4096), 4<<20)
	for s.Scan() {
		var v struct{ Action, Test string }
		if json.Unmarshal(s.Bytes(), &v) != nil {
			continue
		}
		if v.Action == "skip" {
			panic("skipped test: " + v.Test)
		}
		if v.Action == "run" {
			runs[v.Test] = true
		}
		if v.Action == "pass" {
			passed[v.Test] = true
		}
	}
	if err := s.Err(); err != nil {
		panic(err)
	}
	for _, n := range names {
		if !runs[n] || !passed[n] {
			panic("unexecuted or incomplete test: " + n)
		}
	}
}

func main() {
	names := []string{"TestTemporalTimeline", "TestTemporalDifferential", "TestTemporalAtomicViews", "TestTemporalWork"}
	requireTests("temporal_rollup_target_test.go", names)
	runTests("^TestTemporal(Timeline|Differential|AtomicViews|Work)$", names)
	old := []string{"TestQFrame_SortStability", "TestQFrame_GroupByAggregate", "TestQFrame_AggregateGroupByNull", "TestQFrame_RollingWindow"}
	runTests("^TestQFrame_(SortStability|GroupByAggregate|AggregateGroupByNull|RollingWindow)$", old)
	c := exec.Command("go", "run", "./devcheck/baseline")
	c.Stdout, c.Stderr = os.Stdout, os.Stderr
	if err := c.Run(); err != nil {
		panic(err)
	}
	fmt.Println("QFRAME_TEMPORAL_TARGET_OK groups=4")
}
