// Copyright 2025 The Binject Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Command split-attribution performs read-only split-DWARF function
// attribution for a stripped ELF image and its DWARF v5 package.
//
// The command only wires command line arguments and JSON output; all recovery
// logic lives in github.com/Binject/debug/elf.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"

	"github.com/Binject/debug/elf"
)

// queryResult pairs one input PC with the function records whose half-open
// ranges contain it. An unmatched PC keeps an explicit empty matches array.
type queryResult struct {
	PC      uint64              `json:"pc"`
	Matches []elf.SplitFunction `json:"matches"`
}

// report is the top-level JSON document printed to stdout.
type report struct {
	Functions []elf.SplitFunction `json:"functions"`
	Queries   []queryResult       `json:"queries"`
}

func main() {
	imagePath := flag.String("image", "", "path to the stripped ELF image")
	dwpPath := flag.String("dwp", "", "path to the DWARF v5 package (.dwp)")
	queriesPath := flag.String("queries", "", "path to a JSON array of link-time PCs")
	flag.Parse()
	if *imagePath == "" || *dwpPath == "" || *queriesPath == "" {
		fmt.Fprintln(os.Stderr, "split-attribution: -image, -dwp and -queries are all required")
		os.Exit(2)
	}

	queryBytes, err := os.ReadFile(*queriesPath)
	if err != nil {
		fatal("read queries", err)
	}
	var pcs []uint64
	if err := json.Unmarshal(queryBytes, &pcs); err != nil {
		fatal("parse queries JSON", err)
	}

	image, err := elf.Open(*imagePath)
	if err != nil {
		fatal("open image", err)
	}
	defer image.Close()
	packageFile, err := elf.Open(*dwpPath)
	if err != nil {
		fatal("open DWP", err)
	}
	defer packageFile.Close()

	pkg, err := packageFile.DWARFPackage()
	if err != nil {
		fatal("parse DWARF package", err)
	}
	functions, err := image.SplitFunctions(pkg)
	if err != nil {
		fatal("recover split functions", err)
	}

	out := report{Functions: functions, Queries: make([]queryResult, 0, len(pcs))}
	for _, pc := range pcs {
		// SelectSplitFunctions returns an empty, non-nil slice on a miss.
		out.Queries = append(out.Queries, queryResult{
			PC:      pc,
			Matches: elf.SelectSplitFunctions(functions, pc),
		})
	}

	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if err := enc.Encode(out); err != nil {
		fatal("write report", err)
	}
}

func fatal(op string, err error) {
	fmt.Fprintf(os.Stderr, "split-attribution: %s: %v\n", op, err)
	os.Exit(1)
}
