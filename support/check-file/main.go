package main

import (
	"encoding/json"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"os/exec"
	"path/filepath"
)

// This checks the delivery entry point, not the analysis implementation.
func check() error {
	const target = "analysis/memory_range_contract_test.go"
	info, err := os.Lstat(target)
	if err != nil {
		return fmt.Errorf("required target file %s: %w", target, err)
	}
	if !info.Mode().IsRegular() || info.Size() == 0 {
		return fmt.Errorf("required target file must be regular and nonempty: %s", target)
	}
	cmd := exec.Command("go", "list", "-mod=readonly", "-json", "./analysis")
	cmd.Stderr = os.Stderr
	output, err := cmd.Output()
	if err != nil {
		return fmt.Errorf("list target package: %w", err)
	}
	var pkg struct {
		Dir          string
		TestGoFiles  []string
		XTestGoFiles []string
	}
	if err := json.Unmarshal(output, &pkg); err != nil {
		return fmt.Errorf("decode target package: %w", err)
	}
	want, err := filepath.Abs(target)
	if err != nil {
		return err
	}
	selected := false
	for _, name := range append(pkg.TestGoFiles, pkg.XTestGoFiles...) {
		if filepath.Clean(filepath.Join(pkg.Dir, name)) == want {
			selected = true
		}
	}
	if !selected {
		return fmt.Errorf("required target file is not selected by the current Go build: %s", target)
	}
	file, err := parser.ParseFile(token.NewFileSet(), target, nil, parser.AllErrors)
	if err != nil {
		return fmt.Errorf("parse required target file: %w", err)
	}
	declared := map[string]bool{}
	for _, decl := range file.Decls {
		if f, ok := decl.(*ast.FuncDecl); ok && f.Recv == nil && f.Body != nil {
			declared[f.Name.Name] = true
		}
	}
	for _, name := range []string{
		"TestMemoryBranches", "TestMemoryLoops", "TestMemoryCalls",
		"TestMemoryArithmetic", "TestMemoryIsolation",
	} {
		if !declared[name] {
			return fmt.Errorf("required top-level test %s is not declared in %s", name, target)
		}
	}
	fmt.Println("TARGET_FILE_BOUND " + target)
	return nil
}

func main() {
	if err := check(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
