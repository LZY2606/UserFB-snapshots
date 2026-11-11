// Command check_target rejects missing, skipped, or unsuccessful target tests.
package main

import (
	"encoding/json"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) != 2 {
		panic("expected go test JSON path")
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	want := map[string]bool{"TestRedirectAdmissionPolicy": false, "TestRedirectAdmissionHostLease": false, "TestRedirectAdmissionCancel": false, "TestRedirectAdmissionCompatibility": false}
	dec := json.NewDecoder(f)
	for dec.More() {
		var e struct{ Action, Test string }
		if err := dec.Decode(&e); err != nil {
			panic(err)
		}
		if e.Action == "skip" {
			panic("target test skipped: " + e.Test)
		}
		if _, ok := want[e.Test]; ok && e.Action == "pass" {
			want[e.Test] = true
		}
	}
	for name, passed := range want {
		if !passed {
			panic("target test did not pass: " + name)
		}
	}
	fmt.Println("TARGET_FOUR_TESTS_PASSED")
}
