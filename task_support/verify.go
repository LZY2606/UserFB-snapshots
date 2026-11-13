package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

type event struct{ Action, Test, Output string }

func main() {
	groups := os.Args[1:]
	if len(groups) == 0 {
		panic("no required groups")
	}
	passed := map[string]bool{}
	content := map[string]bool{}
	subs := map[string]int{}
	done := false
	scan := bufio.NewScanner(os.Stdin)
	scan.Buffer(make([]byte, 4096), 4<<20)
	for scan.Scan() {
		var e event
		if err := json.Unmarshal(scan.Bytes(), &e); err != nil {
			panic(err)
		}
		if e.Action == "fail" || e.Action == "skip" {
			panic("failure or skip: " + e.Test)
		}
		if e.Action == "pass" {
			passed[e.Test] = true
			if e.Test == "" {
				done = true
			}
			if strings.Contains(e.Test, "/") {
				subs[strings.Split(e.Test, "/")[0]]++
			}
		}
		if strings.Contains(e.Output, "TARGET_CONTENT ") && strings.Contains(e.Test, "/") {
			content[strings.Split(e.Test, "/")[0]] = true
		}
	}
	if err := scan.Err(); err != nil {
		panic(err)
	}
	if !done {
		panic("missing package completion")
	}
	for _, g := range groups {
		if !passed[g] || subs[g] == 0 || !content[g] {
			panic("missing executed content group: " + g)
		}
	}
	fmt.Println("TARGET_GROUPS_COMPLETE", strings.Join(groups, ","))
}
