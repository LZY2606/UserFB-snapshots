// Command check verifies the taskcheck records written by the enhanced
// video tests: every required group must be present, passing and meet its
// minimum number of cases and assertions.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

type groupRecord struct {
	Group   string `json:"group"`
	Cases   int    `json:"cases"`
	Checks  int    `json:"checks"`
	Passed  bool   `json:"passed"`
	Details []struct {
		Name   string `json:"name"`
		Checks int    `json:"checks"`
	} `json:"details"`
}

type requirement struct {
	group     string
	minCases  int
	minChecks int
}

func main() {
	path := os.Getenv("TASKCHECK_FILE")
	if path == "" {
		if len(os.Args) < 2 {
			fmt.Fprintln(os.Stderr, "usage: check <record-file> (or set TASKCHECK_FILE)")
			os.Exit(2)
		}
		path = os.Args[1]
	}

	f, err := os.Open(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open record file %s: %v\n", path, err)
		os.Exit(1)
	}
	defer f.Close()

	records := map[string]groupRecord{}
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var rec groupRecord
		if err := json.Unmarshal(line, &rec); err != nil {
			fmt.Fprintf(os.Stderr, "parse record: %v\n", err)
			os.Exit(1)
		}
		records[rec.Group] = rec
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "read record file: %v\n", err)
		os.Exit(1)
	}

	requirements := []requirement{
		{"TestEnhancedVideoHeaders", 4, 12},
		{"TestEnhancedVideoTracks", 5, 15},
		{"TestEnhancedVideoStream", 4, 12},
	}

	failed := false
	for _, req := range requirements {
		rec, ok := records[req.group]
		if !ok {
			fmt.Printf("CHECK %s: MISSING\n", req.group)
			failed = true
			continue
		}
		status := "OK"
		switch {
		case !rec.Passed:
			status = "FAILED_ASSERTIONS"
			failed = true
		case rec.Cases < req.minCases:
			status = fmt.Sprintf("NOT_ENOUGH_CASES(got=%d,want>=%d)", rec.Cases, req.minCases)
			failed = true
		case rec.Checks < req.minChecks:
			status = fmt.Sprintf("NOT_ENOUGH_CHECKS(got=%d,want>=%d)", rec.Checks, req.minChecks)
			failed = true
		}
		fmt.Printf("CHECK %s: cases=%d checks=%d -> %s\n", req.group, rec.Cases, rec.Checks, status)
		for _, d := range rec.Details {
			fmt.Printf("  - %s: %d checks\n", d.Name, d.Checks)
		}
	}

	if failed {
		os.Exit(1)
	}
	fmt.Println("ENHANCED_VIDEO_TASKCHECK_OK")
}
