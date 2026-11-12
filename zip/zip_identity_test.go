// Copyright 2024 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package zip

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// extraRecord is one parsed tag/length/payload triple from a ZIP extra field.
type extraRecord struct {
	tag     uint16
	payload []byte
}

func extraRecordBytes(tag uint16, payload []byte) []byte {
	out := make([]byte, 4+len(payload))
	binary.LittleEndian.PutUint16(out[0:2], tag)
	binary.LittleEndian.PutUint16(out[2:4], uint16(len(payload)))
	copy(out[4:], payload)
	return out
}

// parseExtra parses a well-formed extra field into its records.
func parseExtra(t *testing.T, extra []byte) []extraRecord {
	t.Helper()
	var records []extraRecord
	for pos := 0; pos < len(extra); {
		if len(extra)-pos < 4 {
			t.Fatalf("truncated extra header at offset %d: %x", pos, extra)
		}
		tag := binary.LittleEndian.Uint16(extra[pos:])
		size := int(binary.LittleEndian.Uint16(extra[pos+2:]))
		end := pos + 4 + size
		if end > len(extra) {
			t.Fatalf("extra record tag %#04x at offset %d overruns the field", tag, pos)
		}
		records = append(records, extraRecord{tag: tag, payload: append([]byte(nil), extra[pos+4:end]...)})
		pos = end
	}
	return records
}

func containsExtraTag(records []extraRecord, tag uint16) bool {
	for _, r := range records {
		if r.tag == tag {
			return true
		}
	}
	return false
}

func extraTags(records []extraRecord) []uint16 {
	tags := make([]uint16, 0, len(records))
	for _, r := range records {
		tags = append(tags, r.tag)
	}
	return tags
}

// identityTags are the five tag classes that must be removed, in the order used when building
// the rich fixture.
var identityTags = []uint16{0x000d, 0x5855, 0x7855, 0x7875, 0x756e}

// preservedTags are records that must survive byte for byte and in order.
var preservedTags = []uint16{0x0001, 0x5455, 0x7075}

// buildRichExtra contains all five Unix identity tag classes (0x756e twice, to cover
// duplicates) interleaved with ZIP64, extended timestamp and Unicode path records.
func buildRichExtra() []byte {
	var out []byte
	out = append(out, extraRecordBytes(0x0001, []byte{0x01})...)
	out = append(out, extraRecordBytes(0x000d, []byte{0xaa, 0xbb})...)
	out = append(out, extraRecordBytes(0x5455, []byte{0x05, 0x01, 0x02, 0x03, 0x04})...)
	out = append(out, extraRecordBytes(0x5855, []byte{1, 2, 3})...)
	out = append(out, extraRecordBytes(0x7075, []byte{0x09, 0x00, 0x42, 0x00, 'u', 't', 'f', '8', '-', 'n', 'a', 'm', 'e'})...)
	out = append(out, extraRecordBytes(0x7855, []byte{4, 5, 6})...)
	out = append(out, extraRecordBytes(0x7875, []byte{1, 2, 0, 0, 0, 0, 7, 0, 0, 0, 8})...)
	out = append(out, extraRecordBytes(0x756e, []byte{7, 0, 0xb1, 4, 2, 0xb2, 4})...)
	out = append(out, extraRecordBytes(0x756e, []byte{9, 0, 0xc1, 4, 2, 0xc2, 4})...)
	return out
}

// expectedPreservedExtra are the exact bytes that must remain after dropping identity.
func expectedPreservedExtra() []byte {
	var out []byte
	out = append(out, extraRecordBytes(0x0001, []byte{0x01})...)
	out = append(out, extraRecordBytes(0x5455, []byte{0x05, 0x01, 0x02, 0x03, 0x04})...)
	out = append(out, extraRecordBytes(0x7075, []byte{0x09, 0x00, 0x42, 0x00, 'u', 't', 'f', '8', '-', 'n', 'a', 'm', 'e'})...)
	return out
}

func writeStdZip(t *testing.T, name string, extra []byte, body string, modified time.Time) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	w, err := zw.CreateHeader(&zip.FileHeader{
		Name: name, Method: zip.Store, Extra: append([]byte(nil), extra...), Modified: modified,
	})
	if err != nil {
		t.Fatalf("zip.Writer.CreateHeader(%q): %v", name, err)
	}
	if _, err := w.Write([]byte(body)); err != nil {
		t.Fatalf("zip.Writer.Write(%q): %v", name, err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("zip.Writer.Close(): %v", err)
	}
	return buf.Bytes()
}

func zipDigest(t *testing.T, r io.Reader) string {
	t.Helper()
	h := sha256.New()
	if _, err := io.Copy(h, r); err != nil {
		t.Fatalf("digesting zip member: %v", err)
	}
	return hex.EncodeToString(h.Sum(nil))
}

func assertIdentityExtraDropped(t *testing.T, extra []byte) {
	t.Helper()
	records := parseExtra(t, extra)
	for _, tag := range identityTags {
		if containsExtraTag(records, tag) {
			t.Errorf("identity tag %#04x still present in extra %x", tag, extra)
		}
	}
}

func TestDropUserIdentityZipExtraRecords(t *testing.T) {
	const body = "file-body"
	rawExtra := buildRichExtra()
	archive := writeStdZip(t, "file", rawExtra, body, time.Time{})
	wantDigest := zipDigest(t, bytes.NewReader([]byte(body)))

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}

	// The default mode does not touch the extra field.
	if !bytes.Equal(r.File[0].Extra, rawExtra) {
		t.Fatalf("default mode changed extra field:\n got %x\nwant %x", r.File[0].Extra, rawExtra)
	}

	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	got := r.File[0].Extra
	if want := expectedPreservedExtra(); !bytes.Equal(got, want) {
		t.Fatalf("cleaned extra mismatch:\n got %x\nwant %x", got, want)
	}
	records := parseExtra(t, got)
	if tags := extraTags(records); !equalTags(tags, preservedTags) {
		t.Errorf("preserved record order = %#x, want %#x", tags, preservedTags)
	}
	assertIdentityExtraDropped(t, got)

	// Member content is still served from the archived bytes through both open paths.
	rc, err := r.File[0].Open()
	if err != nil {
		t.Fatalf("File.Open() error = %v", err)
	}
	if got := zipDigest(t, rc); got != wantDigest {
		t.Errorf("Open() digest = %s, want %s", got, wantDigest)
	}
	raw, err := r.File[0].OpenRaw()
	if err != nil {
		t.Fatalf("File.OpenRaw() error = %v", err)
	}
	if got := zipDigest(t, raw); got != wantDigest {
		t.Errorf("OpenRaw() digest = %s, want %s", got, wantDigest)
	}
}

func equalTags(got, want []uint16) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func TestDropUserIdentityZipRewriteRoundtrip(t *testing.T) {
	const body = "roundtrip-body"
	archive := writeStdZip(t, "file", buildRichExtra(), body, time.Time{})

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}
	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	cleanedHeader := r.File[0].FileHeader

	// Rewrite the sanitized header with the standard library and read it back.
	var out bytes.Buffer
	zw := zip.NewWriter(&out)
	w, err := zw.CreateHeader(&cleanedHeader)
	if err != nil {
		t.Fatalf("rewriting cleaned header: %v", err)
	}
	if _, err := w.Write([]byte(body)); err != nil {
		t.Fatalf("rewriting body: %v", err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("closing rewritten archive: %v", err)
	}

	std, err := zip.NewReader(bytes.NewReader(out.Bytes()), int64(out.Len()))
	if err != nil {
		t.Fatalf("re-reading rewritten archive: %v", err)
	}
	extra := std.File[0].Extra
	records := parseExtra(t, extra)
	assertIdentityExtraDropped(t, extra)

	// The standard library writer may regenerate the extended timestamp (0x5455) record from
	// FileHeader.Modified, so the whole block need not be byte-identical. The other preserved
	// records must survive byte for byte, and no Unix identity record may reappear.
	wantPreserved := map[uint16][]byte{
		0x0001: {0x01},
		0x7075: {0x09, 0x00, 0x42, 0x00, 'u', 't', 'f', '8', '-', 'n', 'a', 'm', 'e'},
	}
	for tag, payload := range wantPreserved {
		var found bool
		for _, rec := range records {
			if rec.tag == tag {
				found = true
				if !bytes.Equal(rec.payload, payload) {
					t.Errorf("record %#04x payload changed on rewrite: got %x, want %x", tag, rec.payload, payload)
				}
			}
		}
		if !found {
			t.Errorf("preserved record %#04x lost on rewrite: %x", tag, extra)
		}
	}
	rc, err := std.File[0].Open()
	if err != nil {
		t.Fatalf("opening rewritten member: %v", err)
	}
	if got := zipDigest(t, rc); got != zipDigest(t, bytes.NewReader([]byte(body))) {
		t.Errorf("rewritten content digest mismatch")
	}
}

func TestDropUserIdentityZipMalformedExtra(t *testing.T) {
	good := extraRecordBytes(0x5455, []byte{0x05, 0x01, 0x02, 0x03, 0x04})
	cases := map[string][]byte{
		"truncated trailing header": append(append([]byte(nil), good...), 0x01),
		"declared length overruns":  {0x6e, 0x75, 10, 0, 1, 2, 3},
		"truncated payload":         {0x6e, 0x75, 3, 0, 1, 2},
	}
	for name, malformed := range cases {
		t.Run(name, func(t *testing.T) {
			archive := writeStdZip(t, "f", malformed, "x", time.Time{})
			r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
			if err != nil {
				t.Fatalf("NewReader() error = %v", err)
			}

			// With the option on, unparseable boundaries mean the whole field must go.
			r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
			if len(r.File[0].Extra) != 0 {
				t.Errorf("malformed extra not cleared: %x", r.File[0].Extra)
			}

			// With the option off, the original raw bytes remain exposed.
			r.SetSecurityMode(DefaultSecurityMode)
			if !bytes.Equal(r.File[0].Extra, malformed) {
				t.Errorf("original extra not restored:\n got %x\nwant %x", r.File[0].Extra, malformed)
			}
		})
	}
}

func TestDropUserIdentityZipModeToggle(t *testing.T) {
	rawExtra := buildRichExtra()
	archive := writeStdZip(t, "file", rawExtra, "data", time.Time{})

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}

	// on -> off -> on must recompute the projection from the archived metadata every time.
	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	first := r.File[0].Extra
	if want := expectedPreservedExtra(); !bytes.Equal(first, want) {
		t.Fatalf("first clean view mismatch:\n got %x\nwant %x", first, want)
	}

	// Mutating the sanitized view must not damage the underlying archived bytes.
	first[0] = 0xAB

	r.SetSecurityMode(DefaultSecurityMode)
	if !bytes.Equal(r.File[0].Extra, rawExtra) {
		t.Fatalf("disabling the mode did not restore the original extra:\n got %x\nwant %x",
			r.File[0].Extra, rawExtra)
	}

	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	if want := expectedPreservedExtra(); !bytes.Equal(r.File[0].Extra, want) {
		t.Fatalf("second clean view mismatch:\n got %x\nwant %x", r.File[0].Extra, want)
	}
}

func TestDropUserIdentityZipOpenReader(t *testing.T) {
	const body = "open-reader-body"
	rawExtra := buildRichExtra()
	archive := writeStdZip(t, "file", rawExtra, body, time.Time{})
	wantDigest := zipDigest(t, bytes.NewReader([]byte(body)))

	path := filepath.Join(t.TempDir(), "tmp.zip")
	if err := os.WriteFile(path, archive, 0o640); err != nil {
		t.Fatalf("writing fixture: %v", err)
	}

	r, err := OpenReader(path)
	if err != nil {
		t.Fatalf("OpenReader() error = %v", err)
	}
	defer r.Close()

	if !bytes.Equal(r.File[0].Extra, rawExtra) {
		t.Fatalf("default mode changed extra field")
	}

	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	assertIdentityExtraDropped(t, r.File[0].Extra)
	if want := expectedPreservedExtra(); !bytes.Equal(r.File[0].Extra, want) {
		t.Fatalf("cleaned extra mismatch:\n got %x\nwant %x", r.File[0].Extra, want)
	}
	rc, err := r.File[0].Open()
	if err != nil {
		t.Fatalf("File.Open() error = %v", err)
	}
	if got := zipDigest(t, rc); got != wantDigest {
		t.Errorf("content digest = %s, want %s", got, wantDigest)
	}

	r.SetSecurityMode(DefaultSecurityMode)
	if !bytes.Equal(r.File[0].Extra, rawExtra) {
		t.Errorf("disabling the mode did not restore the original extra")
	}
}

func TestDropUserIdentityZipCombinedWithPathProtection(t *testing.T) {
	const body = "escaped-body"
	archive := writeStdZip(t, "../escape.txt", buildRichExtra(), body, time.Time{})

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}
	r.SetSecurityMode(MaximumSecurityMode)

	if len(r.File) != 1 {
		t.Fatalf("unexpected member count: %d", len(r.File))
	}
	f := r.File[0]
	if f.Name != "escape.txt" {
		t.Errorf("path sanitization not applied: Name = %q", f.Name)
	}
	assertIdentityExtraDropped(t, f.Extra)
	if want := expectedPreservedExtra(); !bytes.Equal(f.Extra, want) {
		t.Errorf("cleaned extra mismatch:\n got %x\nwant %x", f.Extra, want)
	}
	rc, err := f.Open()
	if err != nil {
		t.Fatalf("File.Open() error = %v", err)
	}
	if got := zipDigest(t, rc); got != zipDigest(t, bytes.NewReader([]byte(body))) {
		t.Errorf("content digest mismatch after combined sanitization")
	}
}

func TestDropUserIdentityZipPreservesModified(t *testing.T) {
	modified := time.Date(2024, time.May, 1, 12, 0, 0, 0, time.UTC)
	archive := writeStdZip(t, "file", extraRecordBytes(0x756e, []byte{7, 0, 0xb1, 4, 2, 0xb2, 4}), "x", modified)

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}
	before := r.File[0].Modified
	r.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	if !r.File[0].Modified.Equal(before) {
		t.Errorf("Modified changed: got %v, want %v", r.File[0].Modified, before)
	}
	// The standard library writer also stores an extended timestamp record for Modified; only
	// the identity record must be gone, FileHeader.Modified itself is never rewritten.
	assertIdentityExtraDropped(t, r.File[0].Extra)
	for _, rec := range parseExtra(t, r.File[0].Extra) {
		if rec.tag != 0x5455 {
			t.Errorf("unexpected surviving record tag %#04x: %x", rec.tag, r.File[0].Extra)
		}
	}
}

func TestDropUserIdentityZipDefaultAndMaximumModes(t *testing.T) {
	if DefaultSecurityMode&DropUserIdentity != 0 {
		t.Fatal("DropUserIdentity must not be part of DefaultSecurityMode")
	}
	if MaximumSecurityMode&DropUserIdentity == 0 {
		t.Fatal("DropUserIdentity must be part of MaximumSecurityMode")
	}

	rawExtra := buildRichExtra()
	archive := writeStdZip(t, "file", rawExtra, "x", time.Time{})

	r, err := NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatalf("NewReader() error = %v", err)
	}
	if !bytes.Equal(r.File[0].Extra, rawExtra) {
		t.Errorf("DefaultSecurityMode must expose the original extra bytes")
	}

	r.SetSecurityMode(MaximumSecurityMode)
	assertIdentityExtraDropped(t, r.File[0].Extra)
}
