// Local QA driver for finite, benign self-test fixtures only.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

type mutation struct{ Name, File, Old, New string }

var cleanupPath string

func die(format string, args ...any) {
	if cleanupPath != "" {
		_ = os.RemoveAll(cleanupPath)
	}
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}
func run(dir, logPath string, args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, args[0], args[1:]...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if writeErr := os.WriteFile(logPath, out, 0600); writeErr != nil {
		die("write log: %v", writeErr)
	}
	if ctx.Err() != nil {
		die("process deadline exceeded: %s", logPath)
	}
	return out, err
}
func copyTree(src, dst string) error {
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if d.IsDir() {
			if d.Name() == ".git" || d.Name() == ".task-build" {
				return filepath.SkipDir
			}
			return os.MkdirAll(filepath.Join(dst, rel), 0755)
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("non-regular file %s", rel)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(dst, rel), data, info.Mode().Perm())
	})
}
func main() {
	if len(os.Args) != 2 {
		die("usage: qadriver source-root")
	}
	src, err := filepath.Abs(os.Args[1])
	if err != nil {
		die("root: %v", err)
	}
	data, err := os.ReadFile(filepath.Join(src, "task_support/mutations.json"))
	if err != nil {
		die("mutations: %v", err)
	}
	var mutations []mutation
	if err = json.Unmarshal(data, &mutations); err != nil || len(mutations) != 3 {
		die("invalid mutation manifest: %v", err)
	}
	logs := filepath.Join(src, ".task-build/qa-logs")
	if err = os.MkdirAll(logs, 0755); err != nil {
		die("logs: %v", err)
	}
	work, err := os.MkdirTemp("", "rapid-qa-*")
	if err != nil {
		die("temporary tree: %v", err)
	}
	cleanupPath = work
	defer os.RemoveAll(work)
	failedGroup := regexp.MustCompile(`(?m)^--- FAIL: TestTask(ShrinkIdentity|ReplayFiles|ByteBridge)( |/)`)
	for _, m := range mutations {
		dir := filepath.Join(work, m.Name)
		if err = copyTree(src, dir); err != nil {
			die("copy %s: %v", m.Name, err)
		}
		path := filepath.Join(dir, m.File)
		original, readErr := os.ReadFile(path)
		if readErr != nil || bytes.Count(original, []byte(m.Old)) != 1 {
			die("mutation anchor %s: %v", m.Name, readErr)
		}
		changed := bytes.Replace(original, []byte(m.Old), []byte(m.New), 1)
		if err = os.WriteFile(path, changed, 0644); err != nil {
			die("apply %s: %v", m.Name, err)
		}
		bin := filepath.Join(dir, "qa.test")
		if _, err = run(dir, filepath.Join(logs, m.Name+"-build.log"), "go", "test", "-c", "-o", bin, "."); err != nil {
			die("MUTANT_BUILD_FAILED %s; see .task-build/qa-logs", m.Name)
		}
		fmt.Printf("MUTANT_BUILD_OK %s\n", m.Name)
		out, runErr := run(dir, filepath.Join(logs, m.Name+"-test.log"), bin, "-test.v", "-test.timeout=60s", "-test.run=^TestTask(ShrinkIdentity|ReplayFiles|ByteBridge)$", "-rapid.seed=1815", "-rapid.nofailfile")
		exitErr, ok := runErr.(*exec.ExitError)
		abnormal := strings.Contains(string(out), "panic:") || strings.Contains(string(out), "fatal error:") || strings.Contains(string(out), "DATA RACE")
		if !ok || exitErr.ExitCode() != 1 || abnormal || !failedGroup.Match(out) {
			die("MUTANT_NOT_CONTENT_REJECTED %s; see .task-build/qa-logs", m.Name)
		}
		fmt.Printf("MUTANT_CONTENT_REJECTED %s\n", m.Name)
	}
	fmt.Println("RAPID_QA_OK mutants=3")
}
