package astisub_test

import (
	"fmt"
	"log"
	"os"
	"time"

	"github.com/asticode/go-astisub"
)

// ExampleNewTeletextDecoder feeds PES payloads one at a time and writes each
// subtitle page as soon as it is closed by the next page header, instead of
// waiting for the whole transport stream to finish.
func ExampleNewTeletextDecoder() {
	// Page 888 is the subtitle page carried by the sample transport stream.
	dec := astisub.NewTeletextDecoder(888)
	origin := time.Unix(0, 0)

	pesFiles := []string{
		"task_support/fixtures/basic_0.pes",
		"task_support/fixtures/basic_1.pes",
		"task_support/fixtures/basic_2.pes",
	}
	for idx, pesFile := range pesFiles {
		data, err := os.ReadFile(pesFile)
		if err != nil {
			log.Fatal(err)
		}

		// Each PES is timestamped. The first timestamp is the output origin.
		subs, err := dec.Push(data, origin.Add(time.Duration(1+2*idx)*time.Second))
		if err != nil {
			log.Fatal(err)
		}

		// Pages closed by this payload are available immediately.
		for _, item := range subs.Items {
			fmt.Printf("page %q from %s to %s\n", item.String(), item.StartAt, item.EndAt)
		}
	}

	// Flush ends the last open page. The sample's last page is empty, so
	// nothing more is emitted, and calling Flush again never resends it.
	subs, err := dec.Flush(origin.Add(5 * time.Second))
	if err != nil {
		log.Fatal(err)
	}
	for _, item := range subs.Items {
		fmt.Printf("page %q from %s to %s\n", item.String(), item.StartAt, item.EndAt)
	}
	// Output:
	// page "hello" from 0s to 2s
	// page "world" from 2s to 4s
}
