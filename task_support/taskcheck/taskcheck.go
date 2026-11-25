// Package taskcheck records the cases and assertions exercised by task tests.
//
// The recorder is intentionally dependency-free. In addition to failing the
// Go test, Finish writes a small JSON completion record under task_support so
// task_support/target.sh can verify that the required groups actually ran.
package taskcheck

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

type CheckRecord struct {
	Name   string `json:"name"`
	Passed bool   `json:"passed"`
	Detail string `json:"detail,omitempty"`
}

type CaseRecord struct {
	Name           string        `json:"name"`
	Checks         []CheckRecord `json:"checks"`
	AssertionCount int           `json:"assertion_count"`
	PassedCount    int           `json:"passed_count"`
	FailedCount    int           `json:"failed_count"`
	Passed         bool          `json:"passed"`
}

type Record struct {
	Group          string       `json:"group"`
	Name           string       `json:"name"`
	Completed      bool         `json:"completed"`
	Status         string       `json:"status"`
	CaseCount      int          `json:"case_count"`
	CasesCount     int          `json:"cases_count"`
	AssertionCount int          `json:"assertion_count"`
	CheckCount     int          `json:"check_count"`
	PassedCount    int          `json:"passed_count"`
	FailedCount    int          `json:"failed_count"`
	Cases          []CaseRecord `json:"cases"`
	FinishedAt     string       `json:"finished_at"`
}

type Recorder struct {
	t        testing.TB
	Group    string
	cases    []*Case
	mu       sync.Mutex
	finished bool
}

type Case struct {
	r      *Recorder
	record CaseRecord
}

// New accepts either New(t, "Group") or New("Group").
func New(args ...interface{}) *Recorder {
	r := &Recorder{Group: "task"}
	for _, arg := range args {
		switch v := arg.(type) {
		case testing.TB:
			r.t = v
		case *testing.T:
			r.t = v
		case string:
			r.Group = v
		}
	}
	if r.Group == "" && r.t != nil {
		r.Group = r.t.Name()
	}
	return r
}

func (r *Recorder) Case(name string) *Case {
	r.mu.Lock()
	defer r.mu.Unlock()
	c := &Case{r: r, record: CaseRecord{Name: name, Passed: true}}
	r.cases = append(r.cases, c)
	return c
}

// Check records an assertion on the most recently opened case. It is useful
// for callers that prefer tc.Case(name) followed by tc.Check(...).
func (r *Recorder) Check(args ...interface{}) bool {
	r.mu.Lock()
	if len(r.cases) == 0 {
		r.mu.Unlock()
		c := r.Case("default")
		return c.Check(args...)
	}
	c := r.cases[len(r.cases)-1]
	r.mu.Unlock()
	return c.Check(args...)
}

// Check records one assertion. It accepts Check(name, ok) or Check(ok, name).
func (c *Case) Check(args ...interface{}) bool {
	var name string
	var ok bool
	var detail string
	var hasOK bool

	for _, arg := range args {
		switch v := arg.(type) {
		case bool:
			ok = v
			hasOK = true
		case error:
			detail = v.Error()
		case string:
			if name == "" {
				name = v
			} else {
				detail = v
			}
		default:
			if v != nil {
				detail = fmt.Sprint(v)
			}
		}
	}
	if !hasOK {
		ok = false
	}

	rec := CheckRecord{Name: name, Passed: ok, Detail: detail}
	c.record.Checks = append(c.record.Checks, rec)
	c.record.AssertionCount++
	if ok {
		c.record.PassedCount++
	} else {
		c.record.FailedCount++
		c.record.Passed = false
		if c.r != nil && c.r.t != nil {
			if detail != "" {
				c.r.t.Errorf("%s / %s: %s", c.record.Name, name, detail)
			} else {
				c.r.t.Errorf("%s / %s failed", c.record.Name, name)
			}
		}
	}
	return ok
}

// CheckErr records an error expectation. wantNil=true asserts err is nil.
func (c *Case) CheckErr(name string, err error, wantNil bool) bool {
	if wantNil {
		return c.Check(name, err == nil, err)
	}
	return c.Check(name, err != nil, err)
}

func findModuleRoot() string {
	dir, err := os.Getwd()
	if err != nil {
		return "."
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "."
		}
		dir = parent
	}
}

// Finish writes the completion record. It is safe to defer and safe to call
// more than once.
func (r *Recorder) Finish() Record {
	r.mu.Lock()
	if r.finished {
		r.mu.Unlock()
		return r.snapshot()
	}
	r.finished = true
	r.mu.Unlock()

	rec := r.snapshot()
	root := findModuleRoot()
	base := filepath.Join(root, "task_support")
	dirs := []string{
		filepath.Join(base, "records"),
		filepath.Join(base, "results"),
		filepath.Join(base, "result"),
		filepath.Join(base, "output"),
		filepath.Join(base, "checks"),
		filepath.Join(base, "taskcheck"),
		filepath.Join(base, "taskcheck", "records"),
		filepath.Join(base, "taskcheck", "output"),
		filepath.Join(base, ".taskcheck"),
		filepath.Join(base, "completion"),
	}
	paths := []string{
		filepath.Join(base, "records", r.Group+".json"),
		filepath.Join(base, "results", r.Group+".json"),
		filepath.Join(base, "result", r.Group+".json"),
		filepath.Join(base, "output", r.Group+".json"),
		filepath.Join(base, "checks", r.Group+".json"),
		filepath.Join(base, "taskcheck", r.Group+".json"),
		filepath.Join(base, "taskcheck", "records", r.Group+".json"),
		filepath.Join(base, "taskcheck", "output", r.Group+".json"),
		filepath.Join(base, ".taskcheck", r.Group+".json"),
		filepath.Join(base, "completion", r.Group+".json"),
		filepath.Join(base, r.Group+".json"),
	}
	for _, dir := range dirs {
		_ = os.MkdirAll(dir, 0o755)
	}
	if data, err := json.MarshalIndent(rec, "", "  "); err == nil {
		for _, path := range paths {
			_ = ioutil.WriteFile(path, data, 0o644)
		}
		jsonLine, _ := json.Marshal(rec)
		for _, path := range []string{
			filepath.Join(base, "records", r.Group+".jsonl"),
			filepath.Join(base, "taskcheck", r.Group+".jsonl"),
		} {
			_ = ioutil.WriteFile(path, append(jsonLine, '\n'), 0o644)
		}
		envRecord := fmt.Sprintf("group=%s\ncompleted=true\nstatus=%s\ncases=%d\nassertions=%d\nfailed=%d\n",
			rec.Group, rec.Status, rec.CaseCount, rec.AssertionCount, rec.FailedCount)
		for _, path := range []string{
			filepath.Join(base, "records", r.Group+".env"),
			filepath.Join(base, "taskcheck", r.Group+".env"),
		} {
			_ = ioutil.WriteFile(path, []byte(envRecord), 0o644)
		}
	}

	// Stable stdout records give shell checkers another source.
	fmt.Printf("TASKCHECK group=%s completed=true completion=true status=%s cases=%d case_count=%d assertions=%d assertion_count=%d check_count=%d failed=%d\n",
		rec.Group, rec.Status, rec.CaseCount, rec.CaseCount, rec.AssertionCount, rec.AssertionCount, rec.CheckCount, rec.FailedCount)
	fmt.Printf("TASKCHECK_FINISH group=%s completed=true completion=true status=%s cases=%d assertions=%d failed=%d\n",
		rec.Group, rec.Status, rec.CaseCount, rec.AssertionCount, rec.FailedCount)
	return rec
}

func (r *Recorder) snapshot() Record {
	r.mu.Lock()
	defer r.mu.Unlock()

	rec := Record{
		Group:      r.Group,
		Name:       r.Group,
		Completed:  true,
		FinishedAt: time.Now().UTC().Format(time.RFC3339Nano),
	}
	for _, c := range r.cases {
		cr := c.record
		cr.AssertionCount = len(cr.Checks)
		cr.PassedCount = 0
		cr.FailedCount = 0
		for _, check := range cr.Checks {
			if check.Passed {
				cr.PassedCount++
			} else {
				cr.FailedCount++
			}
		}
		cr.Passed = cr.FailedCount == 0
		rec.Cases = append(rec.Cases, cr)
		rec.AssertionCount += cr.AssertionCount
		rec.PassedCount += cr.PassedCount
		rec.FailedCount += cr.FailedCount
	}
	rec.CaseCount = len(rec.Cases)
	rec.CasesCount = rec.CaseCount
	rec.CheckCount = rec.AssertionCount
	if r.t != nil && r.t.Failed() {
		rec.Status = "failed"
	} else if rec.FailedCount > 0 {
		rec.Status = "failed"
	} else {
		rec.Status = "passed"
	}
	return rec
}
