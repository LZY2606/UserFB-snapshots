package grab

import (
	"bytes"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

// memResp is a scripted HTTP response served by memoryHTTPClient without any
// network socket.
type memResp struct {
	status        int
	header        http.Header
	body          []byte
	contentLength int64 // -1 means the length is unknown
}

// trackingBody records that a response body was closed.
type trackingBody struct {
	io.ReadCloser
	client *memoryHTTPClient
}

func (b *trackingBody) Close() error {
	b.client.mu.Lock()
	b.client.bodiesClosed++
	b.client.mu.Unlock()
	return b.ReadCloser.Close()
}

// memoryHTTPClient is an in-process HTTPClient: every request is handed to a
// scripted handler and answered from memory, so no TCP port is required. It
// records every request and every body close for assertions. Header values are
// snapshotted because grab mutates the request after sending it.
type memoryHTTPClient struct {
	handler func(*http.Request) memResp

	mu           sync.Mutex
	methods      []string
	ranges       []string
	bodiesClosed int
}

func (c *memoryHTTPClient) Do(req *http.Request) (*http.Response, error) {
	c.mu.Lock()
	c.methods = append(c.methods, req.Method)
	c.ranges = append(c.ranges, req.Header.Get("Range"))
	c.mu.Unlock()

	script := c.handler(req)
	resp := &http.Response{
		StatusCode:    script.status,
		Header:        script.header.Clone(),
		Body:          &trackingBody{io.NopCloser(bytes.NewReader(script.body)), c},
		ContentLength: script.contentLength,
		Request:       req,
	}
	if resp.Header == nil {
		resp.Header = http.Header{}
	}
	return resp, nil
}

func (c *memoryHTTPClient) countMethod(method string) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, m := range c.methods {
		if m == method {
			n++
		}
	}
	return n
}

func (c *memoryHTTPClient) getRangeHeader() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	for i, m := range c.methods {
		if m == http.MethodGet {
			return c.ranges[i]
		}
	}
	return ""
}

func (c *memoryHTTPClient) closedBodies() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.bodiesClosed
}

// headScript returns the standard HEAD response: a six byte resource that
// supports byte ranges.
func headScript() memResp {
	h := http.Header{}
	h.Set("Accept-Ranges", "bytes")
	h.Set("Content-Length", "6")
	return memResp{status: http.StatusOK, header: h, contentLength: 6}
}

// rangeContractCase describes one scenario exercised by
// TestCollectorRangeContract.
type rangeContractCase struct {
	name string

	// local state
	seed      []byte // pre-existing destination file; nil means the file does not exist
	noResume  bool
	reqSize   int64 // Request.Size; zero means unset
	ignoreBad bool

	// scripted responses
	head *memResp // nil means no HEAD request is expected
	get  func(*http.Request) memResp

	// hooks
	beforeCopyError bool

	// expectations
	wantErr           error  // matched with errors.Is; nil means success
	wantStatusCodeErr bool   // expect a StatusCodeError rather than wantErr
	wantFile          []byte // expected destination contents
	wantSize          int64  // expected Response.Size
	wantComplete      int64  // expected Response.BytesComplete
	wantDidResume     bool
	wantBeforeCopies  int
	wantAfterCopies   int
	wantGetRange      string // expected Range header on the GET
}

func TestCollectorRangeContract(t *testing.T) {
	// the partial local file and the complete resource used across cases
	partial := []byte("abc")
	complete := []byte("abcdef")
	suffix := []byte("def")
	replacement := []byte("uvwxyz")

	header := func(pairs ...string) http.Header {
		h := http.Header{}
		for i := 0; i < len(pairs); i += 2 {
			h.Set(pairs[i], pairs[i+1])
		}
		return h
	}
	partialContent := func(contentRange string, body []byte, contentLength int64) memResp {
		h := header("Content-Range", contentRange)
		return memResp{
			status:        http.StatusPartialContent,
			header:        h,
			body:          body,
			contentLength: contentLength,
		}
	}
	fullContent := func(body []byte) memResp {
		return memResp{
			status:        http.StatusOK,
			header:        header(),
			body:          body,
			contentLength: int64(len(body)),
		}
	}

	cases := []rangeContractCase{
		{
			// the happy path: 206 with a Content-Range matching the offset
			name:             "ResumeOK",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return partialContent("bytes 3-5/6", suffix, 3) },
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    true,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			// the server ignores Range and answers 200 with the whole resource
			name:             "ServerIgnoresRangeReturnsFull200",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return fullContent(replacement) },
			wantFile:         replacement,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    false,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			// the reported bug: a 206 whose start does not match the offset
			name:             "PartialContentWrongStart",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return partialContent("bytes 1-3/6", []byte("bcd"), 3) },
			wantErr:          ErrBadContentRange,
			wantFile:         partial,
			wantBeforeCopies: 0,
			wantAfterCopies:  0,
			wantGetRange:     "bytes=3-",
		},
		{
			// a 206 that carries no Content-Range at all
			name:             "PartialContentMissingContentRange",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return memResp{http.StatusPartialContent, header(), suffix, 3} },
			wantErr:          ErrBadContentRange,
			wantFile:         partial,
			wantBeforeCopies: 0,
			wantAfterCopies:  0,
			wantGetRange:     "bytes=3-",
		},
		{
			name: "DuplicateContentRange",
			seed: partial,
			head: ptr(headScript()),
			get: func(*http.Request) memResp {
				h := http.Header{}
				h.Add("Content-Range", "bytes 3-5/6")
				h.Add("Content-Range", "bytes 3-5/6")
				return memResp{http.StatusPartialContent, h, suffix, 3}
			},
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "StarTotalLength",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-5/*", suffix, 3) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "UnsatisfiedRange",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes */6", nil, 0) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name: "MultipartByteRanges",
			seed: partial,
			head: ptr(headScript()),
			get: func(*http.Request) memResp {
				h := header(
					"Content-Type", "multipart/byteranges; boundary=xyz",
					"Content-Range", "bytes 3-5/6",
				)
				return memResp{http.StatusPartialContent, h, suffix, 3}
			},
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "ContentLengthMismatch",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-5/6", suffix, 2) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "HeadTotalMismatch",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-6/7", []byte("defg"), 4) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "RequestSizeMismatch",
			seed:         partial,
			reqSize:      6,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-6/7", []byte("defg"), 4) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "EndBeforeStart",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-2/6", nil, 0) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "EndEqualsTotal",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3-6/6", []byte("defg"), 4) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name: "DecimalOverflow",
			seed: partial,
			head: ptr(headScript()),
			get: func(*http.Request) memResp {
				return partialContent(
					"bytes 9223372036854775808-9223372036854775810/9223372036854775811",
					nil, 0)
			},
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "NonNumericFields",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes x3-5/6", suffix, 3) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			name:         "InternalWhitespaceRejected",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 3 - 5/6", suffix, 3) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			// boundary OWS (SP/HTAB) around the field value is allowed
			name:             "BoundaryWhitespaceAccepted",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return partialContent("\tbytes 3-5/6  ", suffix, 3) },
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    true,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			name:         "UnknownRangeUnit",
			seed:         partial,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("items 3-5/6", suffix, 3) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			// all-digit fields with leading zeros are still valid decimal
			// integers under the HTTP field grammar and must not be scraped
			name: "LeadingZerosAccepted",
			seed: partial,
			head: func() *memResp {
				h := header("Accept-Ranges", "bytes")
				return &memResp{http.StatusOK, h, nil, -1}
			}(),
			get:              func(*http.Request) memResp { return partialContent("bytes 03-05/06", suffix, 3) },
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    true,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			// Request.Size alone must cross-check the GET total even when HEAD
			// did not advertise a length
			name: "RequestSizeOnlyMismatch",
			seed: partial,
			head: func() *memResp {
				h := header("Accept-Ranges", "bytes")
				return &memResp{http.StatusOK, h, nil, -1}
			}(),
			reqSize:      6,
			get:          func(*http.Request) memResp { return partialContent("bytes 3-6/7", []byte("defg"), 4) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			// IgnoreBadStatusCodes must not bypass range validation
			name:         "IgnoreBadStatusCodesDoesNotBypassRangeCheck",
			seed:         partial,
			ignoreBad:    true,
			head:         ptr(headScript()),
			get:          func(*http.Request) memResp { return partialContent("bytes 1-3/6", []byte("bcd"), 3) },
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			// an unknown HEAD length is simply not cross-checked
			name: "UnknownHeadTotalAccepted",
			seed: partial,
			head: func() *memResp {
				h := header("Accept-Ranges", "bytes")
				return &memResp{http.StatusOK, h, nil, -1}
			}(),
			get:              func(*http.Request) memResp { return partialContent("bytes 3-5/6", suffix, 3) },
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    true,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			// a valid resume rejected by BeforeCopy must leave the prefix intact
			name:             "BeforeCopyRejectKeepsPartialFile",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return partialContent("bytes 3-5/6", suffix, 3) },
			beforeCopyError:  true,
			wantErr:          errBeforeCopyReject,
			wantFile:         partial,
			wantBeforeCopies: 1,
			wantAfterCopies:  0,
			wantGetRange:     "bytes=3-",
		},
		{
			// a 200 overwrite rejected by BeforeCopy must also keep the old file
			name:             "BeforeCopyRejectKeepsFileOnFullOverwrite",
			seed:             partial,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return fullContent(replacement) },
			beforeCopyError:  true,
			wantErr:          errBeforeCopyReject,
			wantFile:         partial,
			wantBeforeCopies: 1,
			wantAfterCopies:  0,
			wantGetRange:     "bytes=3-",
		},
		{
			// NoResume overwrites and never sends a Range or a HEAD
			name:             "NoResumeOverwrites",
			seed:             partial,
			noResume:         true,
			get:              func(*http.Request) memResp { return fullContent(replacement) },
			wantFile:         replacement,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    false,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "",
		},
		{
			// a first download with no local file is a plain full GET
			name:             "FirstFullDownload",
			seed:             nil,
			get:              func(*http.Request) memResp { return fullContent(complete) },
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    false,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "",
		},
		{
			// IgnoreBadStatusCodes keeps its ordinary meaning on a normal,
			// non-ranged request: a 404 body is stored as-is
			name:             "IgnoreBadStatusCodesNormalRequest",
			seed:             nil,
			ignoreBad:        true,
			get:              func(*http.Request) memResp { return memResp{http.StatusNotFound, header(), []byte("notfound"), 8} },
			wantFile:         []byte("notfound"),
			wantSize:         8,
			wantComplete:     8,
			wantDidResume:    false,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "",
		},
		{
			// without IgnoreBadStatusCodes a non-2xx resume response fails with
			// a StatusCodeError and leaves the partial file untouched
			name:              "BadStatusFailsByDefault",
			seed:              partial,
			head:              ptr(headScript()),
			get:               func(*http.Request) memResp { return memResp{http.StatusNotFound, header(), []byte("nope"), 4} },
			wantStatusCodeErr: true,
			wantFile:          partial,
			wantBeforeCopies:  0,
			wantAfterCopies:   0,
			wantGetRange:      "bytes=3-",
		},
		{
			// a 416 unsatisfied range still carries Content-Range; even with bad
			// status codes accepted it cannot be appended and must fail the
			// range contract rather than be treated as a full overwrite
			name:      "RangeNotSatisfiableRejected",
			seed:      partial,
			ignoreBad: true,
			head:      ptr(headScript()),
			get: func(*http.Request) memResp {
				return memResp{
					status:        http.StatusRequestedRangeNotSatisfiable,
					header:        header("Content-Range", "bytes */6"),
					contentLength: 0,
				}
			},
			wantErr:      ErrBadContentRange,
			wantFile:     partial,
			wantGetRange: "bytes=3-",
		},
		{
			// with bad status codes accepted, a non-range error response (no
			// Content-Range) is treated as a complete response and overwrites
			// the partial file, exactly as it would for a non-resume request
			name:             "IgnoreBadStatusCodesNonRangeResponseOverwrites",
			seed:             partial,
			ignoreBad:        true,
			head:             ptr(headScript()),
			get:              func(*http.Request) memResp { return memResp{http.StatusNotFound, header(), []byte("nope"), 4} },
			wantFile:         []byte("nope"),
			wantSize:         4,
			wantComplete:     4,
			wantDidResume:    false,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
		{
			// a valid 206 with no Content-Length still resolves the total size
			// from the validated Content-Range
			name: "PartialContentUnknownLengthUsesRangeTotal",
			seed: partial,
			head: func() *memResp {
				h := header("Accept-Ranges", "bytes")
				return &memResp{http.StatusOK, h, nil, -1}
			}(),
			get: func(*http.Request) memResp {
				return memResp{
					status:        http.StatusPartialContent,
					header:        header("Content-Range", "bytes 3-5/6"),
					body:          suffix,
					contentLength: -1,
				}
			},
			wantFile:         complete,
			wantSize:         6,
			wantComplete:     6,
			wantDidResume:    true,
			wantBeforeCopies: 1,
			wantAfterCopies:  1,
			wantGetRange:     "bytes=3-",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			filename := filepath.Join(t.TempDir(), "collector-range")
			if tc.seed != nil {
				if err := os.WriteFile(filename, tc.seed, 0666); err != nil {
					t.Fatal(err)
				}
			}

			client := NewClient()
			memory := &memoryHTTPClient{handler: func(req *http.Request) memResp {
				switch req.Method {
				case http.MethodHead:
					if tc.head == nil {
						t.Errorf("unexpected HEAD request")
						return memResp{status: http.StatusNotFound, header: header()}
					}
					return *tc.head
				case http.MethodGet:
					return tc.get(req)
				default:
					t.Errorf("unexpected request method: %s", req.Method)
					return memResp{status: http.StatusMethodNotAllowed, header: header()}
				}
			}}
			client.HTTPClient = memory

			req := mustNewRequest(filename, "http://example.test/resource")
			req.NoResume = tc.noResume
			req.IgnoreBadStatusCodes = tc.ignoreBad
			if tc.reqSize != 0 {
				req.Size = tc.reqSize
			}

			beforeCopies, afterCopies := 0, 0
			req.BeforeCopy = func(*Response) error {
				beforeCopies++
				if tc.beforeCopyError {
					return errBeforeCopyReject
				}
				return nil
			}
			req.AfterCopy = func(*Response) error {
				afterCopies++
				return nil
			}

			resp := client.Do(req)
			err := resp.Err()

			// error expectations
			switch {
			case tc.wantErr != nil:
				if !errors.Is(err, tc.wantErr) {
					t.Fatalf("expected error wrapping %v, got: %v", tc.wantErr, err)
				}
			case tc.wantStatusCodeErr:
				if !IsStatusCodeError(err) {
					t.Fatalf("expected a StatusCodeError, got: %v", err)
				}
			case err != nil:
				t.Fatalf("unexpected error: %v", err)
			}

			// the destination file must contain exactly what was expected
			got, statErr := os.ReadFile(filename)
			if statErr != nil {
				t.Fatalf("reading destination: %v", statErr)
			}
			if !bytes.Equal(got, tc.wantFile) {
				t.Errorf("destination contents = %q, want %q", got, tc.wantFile)
			}

			// exactly one GET, no retries; a HEAD only where the flow expects one
			if n := memory.countMethod(http.MethodGet); n != 1 {
				t.Errorf("expected exactly 1 GET request, got %d", n)
			}
			wantHeadCount := 0
			if tc.head != nil {
				wantHeadCount = 1
			}
			if n := memory.countMethod(http.MethodHead); n != wantHeadCount {
				t.Errorf("HEAD requests = %d, want %d", n, wantHeadCount)
			}
			if got := memory.getRangeHeader(); got != tc.wantGetRange {
				t.Errorf("GET Range header = %q, want %q", got, tc.wantGetRange)
			}

			// every response body, including failed ones, must be closed
			wantClosed := memory.countMethod(http.MethodHead) + memory.countMethod(http.MethodGet)
			if n := memory.closedBodies(); n != wantClosed {
				t.Errorf("closed response bodies = %d, want %d", n, wantClosed)
			}

			// hook call counts
			if beforeCopies != tc.wantBeforeCopies {
				t.Errorf("BeforeCopy calls = %d, want %d", beforeCopies, tc.wantBeforeCopies)
			}
			if afterCopies != tc.wantAfterCopies {
				t.Errorf("AfterCopy calls = %d, want %d", afterCopies, tc.wantAfterCopies)
			}

			// progress and resume state on successful transfers
			if tc.wantErr == nil && !tc.wantStatusCodeErr {
				if resp.DidResume != tc.wantDidResume {
					t.Errorf("DidResume = %v, want %v", resp.DidResume, tc.wantDidResume)
				}
				if got := resp.Size(); got != tc.wantSize {
					t.Errorf("Size = %d, want %d", got, tc.wantSize)
				}
				if got := resp.BytesComplete(); got != tc.wantComplete {
					t.Errorf("BytesComplete = %d, want %d", got, tc.wantComplete)
				}
				if p := resp.Progress(); p != 1 {
					t.Errorf("Progress = %v, want 1", p)
				}
			}
		})
	}
}

var errBeforeCopyReject = errors.New("before copy rejected")

func ptr[T any](v T) *T { return &v }
