package mail_test

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"reflect"
	"strings"
	"testing"
	"testing/iotest"

	"github.com/emersion/go-message/mail"
)

func readPresentation(t *testing.T, r io.Reader, preferred []string) *mail.Presentation {
	t.Helper()
	p, err := mail.ReadPresentation(r, preferred)
	if err != nil {
		t.Fatalf("ReadPresentation() = %v", err)
	}
	return p
}

func TestPresentationSelectionContract(t *testing.T) {
	// The related root is listed after an image: the image must not be
	// skipped to find another displayable leaf, and an explicit start
	// must be honored.
	t.Run("root after image", func(t *testing.T) {
		const msg = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=rel; start=\"<page>\"\r\n" +
			"\r\n" +
			"--rel\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <logo>\r\n" +
			"Content-Transfer-Encoding: base64\r\n" +
			"\r\n" +
			"AQID\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"Content-ID: <page>\r\n" +
			"\r\n" +
			"<p>hi</p>\r\n" +
			"--rel--\r\n"

		p := readPresentation(t, strings.NewReader(msg), nil)
		if p.MediaType != "text/html" {
			t.Fatalf("MediaType = %q, want text/html", p.MediaType)
		}
		if !reflect.DeepEqual(p.Path, []int{1}) {
			t.Fatalf("Path = %v, want [1]", p.Path)
		}
		if string(p.Body) != "<p>hi</p>" {
			t.Fatalf("Body = %q", p.Body)
		}
		if len(p.Resources) != 1 {
			t.Fatalf("len(Resources) = %d, want 1", len(p.Resources))
		}
		res := p.Resources[0]
		if res.ContentID != "logo" || res.MediaType != "image/png" {
			t.Fatalf("resource = %+v", res)
		}
		if !reflect.DeepEqual(res.Path, []int{0}) {
			t.Fatalf("resource Path = %v, want [0]", res.Path)
		}
		if !reflect.DeepEqual(res.Body, []byte{1, 2, 3}) {
			t.Fatalf("resource Body = %v, want [1 2 3]", res.Body)
		}

		// Without a start parameter the first child (the image) is the
		// root. It is not displayable, and the HTML part behind it must
		// not be reached instead.
		const noStart = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=rel\r\n" +
			"\r\n" +
			"--rel\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <logo>\r\n" +
			"\r\n" +
			"not-a-body\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"\r\n" +
			"<p>hi</p>\r\n" +
			"--rel--\r\n"
		p, err := mail.ReadPresentation(strings.NewReader(noStart), nil)
		if !errors.Is(err, mail.ErrNoPresentation) {
			t.Fatalf("ReadPresentation() = %v, want ErrNoPresentation", err)
		}
		if p != nil {
			t.Fatalf("Presentation = %+v, want nil", p)
		}
	})

	// Inside multipart/alternative the preference order is honored, and
	// on a media type tie the last candidate in wire order wins.
	t.Run("alternative prefers plain", func(t *testing.T) {
		const msg = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/alternative; boundary=alt\r\n" +
			"\r\n" +
			"--alt\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"\r\n" +
			"<p>html</p>\r\n" +
			"--alt\r\n" +
			"Content-Type: text/plain; charset=utf-8\r\n" +
			"\r\n" +
			"plain\r\n" +
			"--alt--\r\n"

		p := readPresentation(t, strings.NewReader(msg), nil)
		if p.MediaType != "text/html" || string(p.Body) != "<p>html</p>" {
			t.Fatalf("default selection = %q %q, want text/html", p.MediaType, p.Body)
		}

		p = readPresentation(t, strings.NewReader(msg), []string{"text/plain"})
		if p.MediaType != "text/plain" || string(p.Body) != "plain" {
			t.Fatalf("plain-first selection = %q %q, want text/plain", p.MediaType, p.Body)
		}
		if !reflect.DeepEqual(p.Path, []int{1}) {
			t.Fatalf("Path = %v, want [1]", p.Path)
		}

		const tie = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/alternative; boundary=alt\r\n" +
			"\r\n" +
			"--alt\r\n" +
			"Content-Type: text/plain\r\n" +
			"\r\n" +
			"first\r\n" +
			"--alt\r\n" +
			"Content-Type: text/plain\r\n" +
			"\r\n" +
			"second\r\n" +
			"--alt--\r\n"
		p = readPresentation(t, strings.NewReader(tie), nil)
		if string(p.Body) != "second" {
			t.Fatalf("tie selection = %q, want last in wire order", p.Body)
		}
		if !reflect.DeepEqual(p.Path, []int{1}) {
			t.Fatalf("tie Path = %v, want [1]", p.Path)
		}
	})

	// A resource Content-ID in an inner related layer shadows the same
	// ID in an outer layer.
	t.Run("nested related shadowing", func(t *testing.T) {
		const msg = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=outer; start=\"<inner>\"\r\n" +
			"\r\n" +
			"--outer\r\n" +
			"Content-Type: multipart/related; boundary=inner\r\n" +
			"Content-ID: <inner>\r\n" +
			"\r\n" +
			"--inner\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"\r\n" +
			"<p>nested</p>\r\n" +
			"--inner\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <x>\r\n" +
			"Content-Transfer-Encoding: base64\r\n" +
			"\r\n" +
			"SU5ORVI=\r\n" +
			"--inner--\r\n" +
			"--outer\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <x>\r\n" +
			"Content-Transfer-Encoding: base64\r\n" +
			"\r\n" +
			"T1VURVI=\r\n" +
			"--outer--\r\n"

		p := readPresentation(t, strings.NewReader(msg), nil)
		if !reflect.DeepEqual(p.Path, []int{0, 0}) {
			t.Fatalf("Path = %v, want [0 0]", p.Path)
		}
		if len(p.Resources) != 1 {
			t.Fatalf("len(Resources) = %d, want 1: %+v", len(p.Resources), p.Resources)
		}
		res := p.Resources[0]
		if res.ContentID != "x" || string(res.Body) != "INNER" {
			t.Fatalf("resource = %+v, want inner shadowing outer", res)
		}
		if !reflect.DeepEqual(res.Path, []int{0, 1}) {
			t.Fatalf("resource Path = %v, want [0 1]", res.Path)
		}
	})

	// Resources from unselected alternative branches and from
	// attachments must not leak into the presentation.
	t.Run("unselected branch pollution", func(t *testing.T) {
		const msg = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/alternative; boundary=alt\r\n" +
			"\r\n" +
			"--alt\r\n" +
			"Content-Type: multipart/related; boundary=r1\r\n" +
			"\r\n" +
			"--r1\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"\r\n" +
			"<p>first</p>\r\n" +
			"--r1\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <a>\r\n" +
			"\r\n" +
			"AAAA\r\n" +
			"--r1--\r\n" +
			"--alt\r\n" +
			"Content-Type: multipart/related; boundary=r2\r\n" +
			"\r\n" +
			"--r2\r\n" +
			"Content-Type: text/html; charset=utf-8\r\n" +
			"\r\n" +
			"<p>second</p>\r\n" +
			"--r2\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <b>\r\n" +
			"\r\n" +
			"BBBB\r\n" +
			"--r2--\r\n" +
			"--alt--\r\n"

		p := readPresentation(t, strings.NewReader(msg), nil)
		if string(p.Body) != "<p>second</p>" {
			t.Fatalf("Body = %q, want second HTML part", p.Body)
		}
		if len(p.Resources) != 1 || p.Resources[0].ContentID != "b" {
			t.Fatalf("Resources = %+v, want only <b> from the selected branch", p.Resources)
		}
		if !reflect.DeepEqual(p.Resources[0].Path, []int{1, 1}) {
			t.Fatalf("resource Path = %v, want [1 1]", p.Resources[0].Path)
		}

		const withAttachment = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/mixed; boundary=mix\r\n" +
			"\r\n" +
			"--mix\r\n" +
			"Content-Type: text/plain; charset=utf-8\r\n" +
			"\r\n" +
			"hello\r\n" +
			"--mix\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <att>\r\n" +
			"Content-Disposition: attachment; filename=a.png\r\n" +
			"\r\n" +
			"AAAA\r\n" +
			"--mix--\r\n"

		p = readPresentation(t, strings.NewReader(withAttachment), nil)
		if string(p.Body) != "hello" {
			t.Fatalf("Body = %q", p.Body)
		}
		if len(p.Resources) != 0 {
			t.Fatalf("Resources = %+v, want none (attachment must not leak)", p.Resources)
		}
	})

	// A truncated message is a hard error, not a partial result.
	t.Run("truncated message", func(t *testing.T) {
		f, err := os.Open("testdata/presentation.eml")
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		raw, err := io.ReadAll(f)
		if err != nil {
			t.Fatal(err)
		}

		// Sanity check: the complete file parses fine.
		if _, err := mail.ReadPresentation(bytes.NewReader(raw), nil); err != nil {
			t.Fatalf("complete message: ReadPresentation() = %v", err)
		}

		// Truncations inside the header block can still yield a valid
		// (empty-bodied) message, so only cut at or after the blank line
		// ending the header. A prefix ending right after a final
		// boundary is still a complete message, so skip those cuts.
		bodyStart := bytes.Index(raw, []byte("\n\n")) + 2
		relFinalEnd := bytes.Index(raw, []byte("--rel--")) + len("--rel--")
		for n := bodyStart; n < len(raw)-1; n++ {
			if n == relFinalEnd {
				continue
			}
			p, err := mail.ReadPresentation(bytes.NewReader(raw[:n]), nil)
			if err == nil {
				t.Fatalf("truncated at %d: expected error, got %+v", n, p)
			}
			if p != nil {
				t.Fatalf("truncated at %d: Presentation = %+v, want nil", n, p)
			}
			if errors.Is(err, mail.ErrNoPresentation) {
				t.Fatalf("truncated at %d: error = %v, must not be ErrNoPresentation", n, err)
			}
		}
	})

	t.Run("invalid preferred", func(t *testing.T) {
		for _, preferred := range [][]string{
			{"image/png"},
			{"text/html", "text/html"},
			{"text/plain", "text/html", "text/plain"},
			{"TEXT/HTML"},
		} {
			p, err := mail.ReadPresentation(strings.NewReader("Subject: x\r\n\r\nhi\r\n"), preferred)
			if err == nil || p != nil {
				t.Fatalf("preferred %v: got (%+v, %v), want error and nil", preferred, p, err)
			}
		}
	})

	t.Run("related start resolution", func(t *testing.T) {
		const missing = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=rel; start=\"<nope>\"\r\n" +
			"\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html\r\n" +
			"Content-ID: <page>\r\n" +
			"\r\n" +
			"<p>hi</p>\r\n" +
			"--rel--\r\n"
		if _, err := mail.ReadPresentation(strings.NewReader(missing), nil); err == nil {
			t.Fatal("start matching no part: expected error")
		} else if errors.Is(err, mail.ErrNoPresentation) {
			t.Fatalf("start matching no part: error = %v, must not be ErrNoPresentation", err)
		}

		const ambiguous = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=rel; start=\"<page>\"\r\n" +
			"\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html\r\n" +
			"Content-ID: <page>\r\n" +
			"\r\n" +
			"<p>one</p>\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html\r\n" +
			"Content-ID: <page>\r\n" +
			"\r\n" +
			"<p>two</p>\r\n" +
			"--rel--\r\n"
		if _, err := mail.ReadPresentation(strings.NewReader(ambiguous), nil); err == nil {
			t.Fatal("start matching multiple parts: expected error")
		}
	})

	t.Run("duplicate resource ID in one layer", func(t *testing.T) {
		const msg = "MIME-Version: 1.0\r\n" +
			"Content-Type: multipart/related; boundary=rel\r\n" +
			"\r\n" +
			"--rel\r\n" +
			"Content-Type: text/html\r\n" +
			"\r\n" +
			"<p>hi</p>\r\n" +
			"--rel\r\n" +
			"Content-Type: image/png\r\n" +
			"Content-ID: <dup>\r\n" +
			"\r\n" +
			"AAAA\r\n" +
			"--rel\r\n" +
			"Content-Type: image/gif\r\n" +
			"Content-ID: <dup>\r\n" +
			"\r\n" +
			"BBBB\r\n" +
			"--rel--\r\n"
		if _, err := mail.ReadPresentation(strings.NewReader(msg), nil); err == nil {
			t.Fatal("duplicate Content-ID in one layer: expected error")
		}
	})

	t.Run("unknown charset is fatal", func(t *testing.T) {
		const msg = "Content-Type: text/plain; charset=x-no-such-charset\r\n" +
			"\r\n" +
			"hi\r\n"
		p, err := mail.ReadPresentation(strings.NewReader(msg), nil)
		if err == nil || p != nil {
			t.Fatalf("got (%+v, %v), want error and nil", p, err)
		}
	})

	t.Run("unknown encoding is fatal", func(t *testing.T) {
		const msg = "Content-Type: text/plain\r\n" +
			"Content-Transfer-Encoding: x-weird\r\n" +
			"\r\n" +
			"hi\r\n"
		p, err := mail.ReadPresentation(strings.NewReader(msg), nil)
		if err == nil || p != nil {
			t.Fatalf("got (%+v, %v), want error and nil", p, err)
		}
	})

	t.Run("no presentable body", func(t *testing.T) {
		const msg = "Content-Type: image/png\r\n" +
			"\r\n" +
			"AAAA\r\n"
		p, err := mail.ReadPresentation(strings.NewReader(msg), nil)
		if !errors.Is(err, mail.ErrNoPresentation) || p != nil {
			t.Fatalf("got (%+v, %v), want ErrNoPresentation and nil", p, err)
		}
	})

	t.Run("chunked reads do not change the result", func(t *testing.T) {
		f, err := os.Open("testdata/presentation.eml")
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		raw, err := io.ReadAll(f)
		if err != nil {
			t.Fatal(err)
		}

		want := readPresentation(t, bytes.NewReader(raw), nil)
		got := readPresentation(t, iotest.OneByteReader(bytes.NewReader(raw)), nil)
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("chunked read differs:\n got %+v\nwant %+v", got, want)
		}
	})
}

func ExampleReadPresentation() {
	f, err := os.Open("testdata/presentation.eml")
	if err != nil {
		panic(err)
	}
	defer f.Close()

	p, err := mail.ReadPresentation(f, nil)
	if err != nil {
		panic(err)
	}

	fmt.Println(p.MediaType)
	fmt.Println(p.Path)
	fmt.Println(strings.TrimSpace(string(p.Body)))
	for _, res := range p.Resources {
		fmt.Println(res.ContentID, res.MediaType, res.Path, len(res.Body))
	}

	// Output:
	// text/html
	// [1 1]
	// <p>article<img src="cid:logo"></p>
	// logo image/png [1 0] 3
}
