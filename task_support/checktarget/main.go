package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"
)

type record struct {
	Group          string `json:"group"`
	Status         string `json:"status"`
	CaseCount      int    `json:"case_count"`
	AssertionCount int    `json:"assertion_count"`
	Completed      bool   `json:"completed"`
}

type requirement struct {
	name      string
	minCases  int
	minChecks int
}

func main() {
	reqs := []requirement{
		{name: "TestEnhancedVideoHeaders", minCases: 4, minChecks: 12},
		{name: "TestEnhancedVideoTracks", minCases: 5, minChecks: 15},
		{name: "TestEnhancedVideoStream", minCases: 4, minChecks: 12},
	}

	failed := false
	for _, req := range reqs {
		paths := []string{
			filepath.Join("task_support", "records", req.name+".json"),
			filepath.Join("task_support", "taskcheck", req.name+".json"),
			filepath.Join("task_support", "taskcheck", "records", req.name+".json"),
			filepath.Join("task_support", ".taskcheck", req.name+".json"),
			filepath.Join("task_support", "completion", req.name+".json"),
		}

		var rec record
		var loaded bool
		for _, path := range paths {
			data, err := ioutil.ReadFile(path)
			if err != nil {
				continue
			}
			if err := json.Unmarshal(data, &rec); err != nil {
				fmt.Printf("TARGET FAIL %s: invalid record %s: %v\n", req.name, path, err)
				failed = true
				break
			}
			loaded = true
			break
		}
		if !loaded {
			fmt.Printf("TARGET FAIL %s: no completion record\n", req.name)
			failed = true
			continue
		}

		ok := rec.Completed && rec.Status == "passed" &&
			rec.CaseCount >= req.minCases && rec.AssertionCount >= req.minChecks
		status := "OK"
		if !ok {
			status = "FAIL"
			failed = true
		}
		fmt.Printf("TARGET %s %s cases=%d/%d assertions=%d/%d status=%s\n",
			status, req.name, rec.CaseCount, req.minCases,
			rec.AssertionCount, req.minChecks, rec.Status)
	}

	if failed {
		os.Exit(1)
	}
}
