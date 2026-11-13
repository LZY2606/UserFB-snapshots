// Package taskcheck records the cases and assertions of the enhanced video
// task tests so an external checker can verify coverage independently of the
// test process exit status.
package taskcheck

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

var fileMu sync.Mutex

type caseRecord struct {
	Name   string `json:"name"`
	Checks int    `json:"checks"`
}

// Recorder collects the cases of one test group and appends one JSON record
// when Finish is called.
type Recorder struct {
	t      *testing.T
	group  string
	cases  []caseRecord
	cur    int
	passed bool
}

// New starts a record for the current test group.
func New(t *testing.T) *Recorder {
	return &Recorder{
		t:      t,
		group:  t.Name(),
		passed: true,
	}
}

// Case begins a named case inside the group.
func (r *Recorder) Case(name string) {
	r.cases = append(r.cases, caseRecord{Name: name})
	r.cur = len(r.cases) - 1
}

// Check records one assertion and fails the test when ok is false.
func (r *Recorder) Check(desc string, ok bool) {
	if len(r.cases) == 0 {
		r.t.Fatalf("taskcheck: Check %q called before Case", desc)
	}
	r.cases[r.cur].Checks++
	if !ok {
		r.passed = false
		r.t.Errorf("group %s case %s: %s", r.group, r.cases[r.cur].Name, desc)
	}
}

type groupRecord struct {
	Group   string       `json:"group"`
	Cases   int          `json:"cases"`
	Checks  int          `json:"checks"`
	Passed  bool         `json:"passed"`
	Details []caseRecord `json:"details"`
}

// Finish writes the group record, one compact JSON object per line, to
// TASKCHECK_FILE (or a default file under the temporary directory).
func (r *Recorder) Finish() {
	totalChecks := 0
	for _, c := range r.cases {
		totalChecks += c.Checks
	}

	rec := groupRecord{
		Group:   r.group,
		Cases:   len(r.cases),
		Checks:  totalChecks,
		Passed:  r.passed,
		Details: r.cases,
	}
	line, err := json.Marshal(rec)
	if err != nil {
		r.t.Fatalf("taskcheck: marshal record: %v", err)
	}
	line = append(line, '\n')

	path := os.Getenv("TASKCHECK_FILE")
	if path == "" {
		path = filepath.Join(os.TempDir(), "rtmp-taskcheck.json")
	}

	fileMu.Lock()
	defer fileMu.Unlock()
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		r.t.Fatalf("taskcheck: open record file %s: %v", path, err)
	}
	defer f.Close()
	if _, err = f.Write(line); err != nil {
		r.t.Fatalf("taskcheck: write record: %v", err)
	}

	r.t.Logf("taskcheck: group=%s cases=%d checks=%d passed=%v", r.group, len(r.cases), totalChecks, r.passed)
}
