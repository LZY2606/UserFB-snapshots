package grab

import (
	"bufio"
	"bytes"
	"context"
	"crypto/md5"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// ExampleResponse_OpenStream records newline-delimited rows while the download
// is still running. Everything scanned here is provisional: checksum validation
// (or another post-processing step) can still reject the whole transfer
// afterwards, as this example demonstrates.
func ExampleResponse_OpenStream() {
	payload := "row-1\nrow-2\nrow-3\n"
	client := NewClient()
	client.HTTPClient = exampleStreamTransport{payload: payload}

	req, err := NewRequest("", "http://example.invalid/records.ndjson")
	if err != nil {
		panic(err)
	}
	// Keep everything in memory and demand a checksum that cannot match.
	req.NoStore = true
	req.SetChecksum(md5.New(), bytes.Repeat([]byte{0}, md5.Size), true)

	resp := client.Do(req)
	stream, err := resp.OpenStream(context.Background())
	if err != nil {
		panic(err)
	}
	defer stream.Close()

	// Records become available as chunks land; none of them has been validated
	// yet, so they must not be treated as committed input at this point.
	scanner := bufio.NewScanner(stream)
	for scanner.Scan() {
		fmt.Println("scanned: " + scanner.Text())
	}

	if err := resp.Err(); err != nil {
		fmt.Println("final validation: " + err.Error())
	}
	// Output:
	// scanned: row-1
	// scanned: row-2
	// scanned: row-3
	// final validation: checksum mismatch
}

type exampleStreamTransport struct {
	payload string
}

func (t exampleStreamTransport) Do(req *http.Request) (*http.Response, error) {
	return &http.Response{
		StatusCode:    http.StatusOK,
		ContentLength: int64(len(t.payload)),
		Header:        http.Header{},
		Body:          io.NopCloser(strings.NewReader(t.payload)),
		Request:       req,
	}, nil
}
