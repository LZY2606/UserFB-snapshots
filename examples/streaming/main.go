package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"

	ferret "github.com/MontFerret/ferret"
	httpdriver "github.com/MontFerret/ferret/pkg/drivers/http"
	"github.com/MontFerret/ferret/pkg/runtime"
)

// errEnough is an application-level stop signal. Stream keeps it reachable
// through errors.Is, so the caller can tell "we stopped on purpose" from a
// real query failure.
var errEnough = errors.New("collected enough detail pages")

func main() {
	// A local catalog plus detail pages. Every detail request is counted: the
	// example uses the counter to show what was already fetched when the
	// receiver decided to stop.
	var requests int

	mux := http.NewServeMux()

	mux.HandleFunc("/catalog", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = fmt.Fprint(w, `
			<html><body>
				<a href="/detail/1">1</a>
				<a href="/detail/2">2</a>
				<a href="/detail/3">3</a>
				<a href="/detail/4">4</a>
				<a href="/detail/5">5</a>
			</body></html>
		`)
	})

	mux.HandleFunc("/detail/", func(w http.ResponseWriter, r *http.Request) {
		requests++
		_, _ = fmt.Fprintf(w, `<html><body><h1>detail page %s</h1></body></html>`, r.URL.Path[len("/detail/"):])
	})

	server := httptest.NewServer(mux)
	defer server.Close()

	f := ferret.New()

	if err := f.Drivers().Register(httpdriver.NewDriver()); err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	streamCatalog(f, server.URL)
	fmt.Println()
	sortBoundary(f, server.URL)

	fmt.Printf("\nstopped after %d detail requests; completed requests are not rolled back\n", requests)
}

// streamCatalog walks the catalog and opens every detail page on demand. The
// receiver stops after the second record, so pages 3..5 are never requested.
func streamCatalog(f *ferret.Instance, base string) {
	query := `
		FOR id IN 1..5
			LET page = DOCUMENT(CONCAT(@base, "/detail/", TO_STRING(id)))
			RETURN {
				id: id,
				title: INNER_TEXT(page, "h1")
			}
	`

	program, err := f.Compile(query)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	var delivered []map[string]any

	err = f.Stream(context.Background(), program, func(raw json.RawMessage) error {
		// The bytes handed here belong to the receiver. Keep a copy if the
		// record has to outlive the callback.
		record := make(json.RawMessage, len(raw))
		copy(record, raw)

		var item map[string]any
		if err := json.Unmarshal(record, &item); err != nil {
			return err
		}

		delivered = append(delivered, item)
		fmt.Printf("delivered: %s\n", record)

		// Backpressure driven by the downstream: take two records and stop.
		// The query does not prefetch the remaining detail pages while this
		// callback is running, and it stops immediately afterwards.
		if len(delivered) == 2 {
			return errEnough
		}

		return nil
	}, runtime.WithParam("base", base))

	if !errors.Is(err, errEnough) {
		fmt.Println(err)
		os.Exit(1)
	}

	// The two delivered records stay valid even though the run was aborted;
	// nothing is rolled back, and no further detail page gets fetched.
	fmt.Printf("kept %d records after the early stop\n", len(delivered))
}

// sortBoundary shows the materialization boundary of blocking clauses.
//
// SORT (and COLLECT) need the whole input before they can emit anything, so
// DOCUMENT for every input row runs before the first callback. What is still
// streamed is the RETURN action after sorting: each output record is evaluated
// and delivered separately instead of building one big result array.
func sortBoundary(f *ferret.Instance, base string) {
	query := `
		FOR id IN [3, 1, 2]
			LET page = DOCUMENT(CONCAT(@base, "/detail/", TO_STRING(id)))
			SORT id
			RETURN { id: id, title: INNER_TEXT(page, "h1") }
	`

	program, err := f.Compile(query)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	fmt.Println("SORT materializes all three detail pages, then streams sorted records:")

	err = f.Stream(context.Background(), program, func(raw json.RawMessage) error {
		fmt.Printf("delivered: %s\n", raw)
		return nil
	}, runtime.WithParam("base", base))
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	// COLLECT has the same boundary: all input rows are grouped first, and the
	// groups are then delivered one by one.
	collectQuery := `
		FOR row IN [{g: "b"}, {g: "a"}, {g: "b"}]
			COLLECT group = row.g
			RETURN { group: group }
	`

	collectProgram, err := f.Compile(collectQuery)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	fmt.Println("COLLECT materializes and groups the input, then streams groups:")

	err = f.Stream(context.Background(), collectProgram, func(raw json.RawMessage) error {
		fmt.Printf("delivered: %s\n", raw)
		return nil
	})
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
