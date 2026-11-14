// Command mutator applies exactly one named, compilable degradation to a
// throwaway copy of worker.go. It exists solely to support
// task_support/check_mutations.sh and is never used by the tests themselves.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type mutation struct {
	name string
	from string
	to   string
}

// Each replacement is a single, exact source anchor so the degraded copy
// still compiles but changes one protocol decision.
var mutations = []mutation{
	{
		name: "robots-bypass",
		from: "} else if w.isAllowedPerRobotsPolicies(ctx.url) {",
		to:   "} else if true || w.isAllowedPerRobotsPolicies(ctx.url) {",
	},
	{
		name: "requestget-veto-ignored",
		from: "if !w.opts.Extender.RequestGet(ctx, res) {",
		to:   "if false && !w.opts.Extender.RequestGet(ctx, res) {",
	},
	{
		name: "visited-harvest-nil",
		from: "w.opts.Extender.Visited(ctx, harvested)",
		to:   "w.opts.Extender.Visited(ctx, nil)",
	},
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: mutator <repo-copy-dir> <mutation-name>")
		os.Exit(2)
	}
	dir, name := os.Args[1], os.Args[2]

	var mut *mutation
	for i := range mutations {
		if mutations[i].name == name {
			mut = &mutations[i]
			break
		}
	}
	if mut == nil {
		fmt.Fprintf(os.Stderr, "unknown mutation %q\n", name)
		os.Exit(2)
	}

	path := filepath.Join(dir, "worker.go")
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read worker.go: %v\n", err)
		os.Exit(2)
	}
	text := string(data)
	if n := strings.Count(text, mut.from); n != 1 {
		fmt.Fprintf(os.Stderr, "anchor for %q matched %d times, want exactly 1\n", name, n)
		os.Exit(2)
	}
	text = strings.Replace(text, mut.from, mut.to, 1)
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write worker.go: %v\n", err)
		os.Exit(2)
	}
	fmt.Printf("applied mutation %q\n", name)
}
