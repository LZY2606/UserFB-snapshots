package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
)

func main() {
	cmd := exec.Command("go", "test", "-modfile=support/offline.mod", "-mod=readonly", "-json", "-count=1", "-run", "^TestIndex(Build|ReadAt|Isolation)$", "./bzip2")
	out, err := cmd.CombinedOutput()
	os.Stdout.Write(out)
	if err != nil {
		os.Exit(1)
	}
	d := json.NewDecoder(bytes.NewReader(out))
	ran, passed := map[string]bool{}, map[string]bool{}
	for {
		var event struct{ Action, Test string }
		err := d.Decode(&event)
		if err == io.EOF {
			break
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "invalid go test event stream")
			os.Exit(2)
		}
		if event.Test == "" {
			continue
		}
		switch event.Action {
		case "run":
			ran[event.Test] = true
		case "pass":
			passed[event.Test] = true
		case "skip":
			fmt.Fprintln(os.Stderr, "skipped target test rejected:", event.Test)
			os.Exit(3)
		}
	}
	for _, name := range []string{"TestIndexBuild", "TestIndexReadAt", "TestIndexIsolation"} {
		if !ran[name] || !passed[name] {
			fmt.Fprintln(os.Stderr, "missing executed PASS:", name)
			os.Exit(4)
		}
	}
	fmt.Println("BZ2_INDEX_TARGET_EXECUTED cases=3")
}
