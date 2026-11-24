// Command codeview_samples writes the inert embedded-CodeView PE samples built
// by the task_support/fixtures constructor to analysis/samples. The samples
// contain synthetic data only; they are never executed or loaded as images.
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/mewmew/pe/task_support/fixtures"
)

func main() {
	dir := filepath.Join("analysis", "samples")
	if err := os.MkdirAll(dir, 0755); err != nil {
		fmt.Fprintf(os.Stderr, "mkdir %s: %v\n", dir, err)
		os.Exit(1)
	}
	for _, sig := range []string{"NB09", "NB11"} {
		path := filepath.Join(dir, fmt.Sprintf("embedded-%s.pe", sig))
		if err := os.WriteFile(path, fixtures.Embedded(sig), 0644); err != nil {
			fmt.Fprintf(os.Stderr, "write %s: %v\n", path, err)
			os.Exit(1)
		}
		fmt.Printf("wrote %s (%d bytes)\n", path, len(fixtures.Embedded(sig)))
	}
}
