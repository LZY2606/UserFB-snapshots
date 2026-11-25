// This example runs the manifest-constrained dynamic memory analysis over the
// controlled crosscall sample and prints the full JSON report.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/tetratelabs/wazero/experimental/memanalysis"
)

func main() {
	wasmPath := flag.String("wasm", "samples/crosscall.wasm", "path to the sample Wasm binary")
	planPath := flag.String("plan", "samples/crosscall.json", "path to the sample analysis plan")
	flag.Parse()

	binary, err := os.ReadFile(*wasmPath)
	if err != nil {
		log.Fatalf("read wasm: %v", err)
	}
	planBytes, err := os.ReadFile(*planPath)
	if err != nil {
		log.Fatalf("read plan: %v", err)
	}
	var plan memanalysis.Plan
	if err := json.Unmarshal(planBytes, &plan); err != nil {
		log.Fatalf("parse plan: %v", err)
	}

	report, err := memanalysis.Analyze(context.Background(), binary, plan)
	if err != nil {
		log.Fatalf("analyze: %v", err)
	}
	out, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		log.Fatalf("marshal report: %v", err)
	}
	fmt.Println(string(out))
}
