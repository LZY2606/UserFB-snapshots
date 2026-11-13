package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
)

func run(args ...string) {
	c := exec.Command("go", args...)
	c.Stdout = os.Stdout
	c.Stderr = os.Stderr
	if e := c.Run(); e != nil {
		panic(e)
	}
}
func main() {
	const target = "capsule_target_test.go"
	b, e := os.ReadFile(target)
	if e != nil || len(bytes.TrimSpace(b)) == 0 {
		panic("missing or empty " + target)
	}
	required := []string{"TestCapsuleRelink", "TestCapsuleBindings", "TestCapsuleRoundTrip", "TestCapsuleReject"}
	c := exec.Command("go", "test", "-json", "-count=1", "-v", ".", "-run", "^TestCapsule(Relink|Bindings|RoundTrip|Reject)$")
	out, e := c.CombinedOutput()
	os.Stdout.Write(out)
	if e != nil {
		panic(e)
	}
	passes := map[string]bool{}
	runs := map[string]bool{}
	s := bufio.NewScanner(bytes.NewReader(out))
	s.Buffer(make([]byte, 4096), 4<<20)
	for s.Scan() {
		var ev struct{ Action, Test string }
		if json.Unmarshal(s.Bytes(), &ev) != nil {
			continue
		}
		if ev.Action == "skip" {
			panic("skipped target test: " + ev.Test)
		}
		if ev.Action == "run" {
			runs[ev.Test] = true
		}
		if ev.Action == "pass" && ev.Test != "" {
			passes[ev.Test] = true
		}
	}
	if e := s.Err(); e != nil {
		panic(e)
	}
	for _, n := range required {
		if !runs[n] || !passes[n] {
			panic("unexecuted target: " + n)
		}
	}
	run("test", "-count=1", "-v", "./vm", "-run", "^TestRun_(NilProgram|ReuseVM|ReuseVM_for_different_variables|TaggedFieldName)$")
	run("test", "-count=1", "-v", "./optimizer", "-run", "^TestOptimize_filter_map($|_)")
	run("run", "./sanity/baseline")
	fmt.Println("EXPR_CAPSULE_TARGET_OK groups=4")
}
