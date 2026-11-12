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

package tar

import (
	"archive/tar"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"testing"
	"time"
)

// identityPaxKeys mirrors the list used by the implementation.
var testIdentityPaxKeys = []string{"uid", "gid", "uname", "gname"}

func digest(t *testing.T, r io.Reader) string {
	t.Helper()
	h := sha256.New()
	if _, err := io.Copy(h, r); err != nil {
		t.Fatalf("digesting content: %v", err)
	}
	return hex.EncodeToString(h.Sum(nil))
}

// writeStdTar builds an archive with the standard library, exercising both structured identity
// fields and PAX records (including PAX values that override the structured fields).
func writeStdTar(t *testing.T, headers []*tar.Header, bodies map[string]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	for _, h := range headers {
		if err := tw.WriteHeader(h); err != nil {
			t.Fatalf("tar.Writer.WriteHeader(%q): %v", h.Name, err)
		}
		if body := bodies[h.Name]; body != "" {
			if _, err := tw.Write([]byte(body)); err != nil {
				t.Fatalf("tar.Writer.Write(%q): %v", h.Name, err)
			}
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatalf("tar.Writer.Close(): %v", err)
	}
	return buf.Bytes()
}

func assertNoIdentity(t *testing.T, h *tar.Header) {
	t.Helper()
	if h.Uid != 0 {
		t.Errorf("Uid = %d, want 0", h.Uid)
	}
	if h.Gid != 0 {
		t.Errorf("Gid = %d, want 0", h.Gid)
	}
	if h.Uname != "" {
		t.Errorf("Uname = %q, want empty", h.Uname)
	}
	if h.Gname != "" {
		t.Errorf("Gname = %q, want empty", h.Gname)
	}
	for _, key := range testIdentityPaxKeys {
		if _, ok := h.PAXRecords[key]; ok {
			t.Errorf("PAXRecords still contains identity key %q", key)
		}
	}
}

func assertIdentityPresent(t *testing.T, h *tar.Header, uid, gid int, uname, gname string) {
	t.Helper()
	if h.Uid != uid {
		t.Errorf("Uid = %d, want %d", h.Uid, uid)
	}
	if h.Gid != gid {
		t.Errorf("Gid = %d, want %d", h.Gid, gid)
	}
	if h.Uname != uname {
		t.Errorf("Uname = %q, want %q", h.Uname, uname)
	}
	if h.Gname != gname {
		t.Errorf("Gname = %q, want %q", h.Gname, gname)
	}
}

func assertPaxIdentityPresent(t *testing.T, h *tar.Header) {
	t.Helper()
	for _, key := range testIdentityPaxKeys {
		if _, ok := h.PAXRecords[key]; !ok {
			t.Errorf("PAXRecords is missing identity key %q: %v", key, h.PAXRecords)
		}
	}
}

func TestDropUserIdentityTarHeader(t *testing.T) {
	modTime := time.Date(2024, time.May, 1, 12, 0, 0, 0, time.UTC)
	headers := []*tar.Header{
		{
			Name: "regular.txt", Mode: 0640, Size: 9, ModTime: modTime,
			Uid: 1201, Gid: 1202, Uname: "private-user", Gname: "private-group",
			Format: tar.FormatPAX,
			// PAX override values take precedence over the structured fields on read.
			PAXRecords: map[string]string{
				"uid": "9999", "gid": "8888", "uname": "pax-user", "gname": "pax-group",
				"mtime":   "1714564800.0",
				"comment": "unrelated-pax-record",
			},
		},
		{
			Name: "link", Typeflag: tar.TypeSymlink, Linkname: "regular.txt", Mode: 0777,
			Uid: 33, Gid: 44, Uname: "www-data", Gname: "www-group", Format: tar.FormatPAX,
			PAXRecords: map[string]string{"uname": "pax-user"},
		},
	}
	bodies := map[string]string{"regular.txt": "file-body"}
	archive := writeStdTar(t, headers, bodies)
	wantDigest := digest(t, bytes.NewReader([]byte("file-body")))

	tr := NewReader(bytes.NewReader(archive))
	tr.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)

	h, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertNoIdentity(t, h)
	// Non-identity fields and content keep their original behavior.
	if h.Name != "regular.txt" || h.Mode != 0640 {
		t.Errorf("name/mode changed: %q %o", h.Name, h.Mode)
	}
	if !h.ModTime.Equal(modTime) {
		t.Errorf("ModTime = %v, want %v", h.ModTime, modTime)
	}
	if h.PAXRecords["comment"] != "unrelated-pax-record" {
		t.Errorf("unrelated PAX record lost: %v", h.PAXRecords)
	}
	if got := digest(t, tr); got != wantDigest {
		t.Errorf("content digest = %s, want %s", got, wantDigest)
	}

	h, err = tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertNoIdentity(t, h)
	if h.Typeflag != tar.TypeSymlink || h.Linkname != "regular.txt" {
		t.Errorf("symlink target changed: type=%d linkname=%q", h.Typeflag, h.Linkname)
	}
}

func TestDropUserIdentityTarRewriteRoundtrip(t *testing.T) {
	modTime := time.Date(2024, time.May, 1, 12, 0, 0, 0, time.UTC)
	headers := []*tar.Header{{
		Name: "pax.txt", Mode: 0640, Size: 4, ModTime: modTime,
		Uid: 1201, Gid: 1202, Uname: "alice", Gname: "staff",
		Format: tar.FormatPAX,
		PAXRecords: map[string]string{
			"uid": "9999", "gid": "8888", "uname": "pax-user", "gname": "pax-group",
			"mtime":                 "1714564800.0",
			"comment":               "unrelated-pax-record",
			"SCHILY.xattr.user.foo": "bar",
		},
	}}
	archive := writeStdTar(t, headers, map[string]string{"pax.txt": "data"})

	safe := NewReader(bytes.NewReader(archive))
	safe.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	h, err := safe.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	body, err := io.ReadAll(safe)
	if err != nil {
		t.Fatalf("reading body: %v", err)
	}

	// Hand the sanitized header back to the standard library writer and read the result
	// back with the standard library: no identity may reappear through PAXRecords.
	var out bytes.Buffer
	tw := tar.NewWriter(&out)
	if err := tw.WriteHeader(h); err != nil {
		t.Fatalf("rewriting header: %v", err)
	}
	if _, err := tw.Write(body); err != nil {
		t.Fatalf("rewriting body: %v", err)
	}
	if err := tw.Close(); err != nil {
		t.Fatalf("closing rewritten archive: %v", err)
	}

	std := tar.NewReader(bytes.NewReader(out.Bytes()))
	rh, err := std.Next()
	if err != nil {
		t.Fatalf("re-reading rewritten archive: %v", err)
	}
	assertNoIdentity(t, rh)
	if !rh.ModTime.Equal(modTime) {
		t.Errorf("ModTime not preserved on rewrite: %v, want %v", rh.ModTime, modTime)
	}
	if rh.PAXRecords["comment"] != "unrelated-pax-record" {
		t.Errorf("unrelated PAX record lost on rewrite: %v", rh.PAXRecords)
	}
	if rh.PAXRecords["SCHILY.xattr.user.foo"] != "bar" {
		t.Errorf("unrelated PAX record lost on rewrite: %v", rh.PAXRecords)
	}
	if got := digest(t, std); got != digest(t, bytes.NewReader([]byte("data"))) {
		t.Errorf("rewritten content digest mismatch")
	}
}

func TestDropUserIdentityTarCombinedModes(t *testing.T) {
	headers := []*tar.Header{{
		Name: "x.txt", Mode: 04750, Size: 2,
		Uid: 1201, Gid: 1202, Uname: "alice", Gname: "staff",
		Xattrs: map[string]string{"user.hello": "world"},
		Format: tar.FormatPAX,
		PAXRecords: map[string]string{
			"uid": "9999", "gid": "8888", "uname": "pax-user", "gname": "pax-group",
			"mtime":                   "1714564800.0",
			"comment":                 "unrelated-pax-record",
			"SCHILY.xattr.user.hello": "world",
		},
	}}
	archive := writeStdTar(t, headers, map[string]string{"x.txt": "ab"})

	tr := NewReader(bytes.NewReader(archive))
	tr.SetSecurityMode(DefaultSecurityMode | DropUserIdentity | SanitizeFileMode | DropXattrs)
	h, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}

	assertNoIdentity(t, h)
	if h.Mode != 0750 {
		t.Errorf("SanitizeFileMode not applied: mode = %o", h.Mode)
	}
	if h.Xattrs != nil {
		t.Errorf("DropXattrs not applied, Xattrs = %v", h.Xattrs)
	}
	// With DropXattrs active only the time keys may remain, and identity keys must be gone.
	for key := range h.PAXRecords {
		if key == "comment" || key == "SCHILY.xattr.user.hello" {
			t.Errorf("DropXattrs failed to remove %q: %v", key, h.PAXRecords)
		}
		if key != "ctime" && key != "mtime" && key != "atime" {
			t.Errorf("unexpected PAX record %q after combined filtering: %v", key, h.PAXRecords)
		}
	}
}

func TestDropUserIdentityTarModeSwitch(t *testing.T) {
	headers := []*tar.Header{
		{
			Name: "first", Mode: 0640, Size: 1, Uid: 11, Gid: 12, Uname: "u1", Gname: "g1",
			Format:     tar.FormatPAX,
			PAXRecords: map[string]string{"uid": "11", "gid": "12", "uname": "u1", "gname": "g1"},
		},
		{
			Name: "second", Mode: 0640, Size: 1, Uid: 21, Gid: 22, Uname: "u2", Gname: "g2",
			Format:     tar.FormatPAX,
			PAXRecords: map[string]string{"uid": "21", "gid": "22", "uname": "u2", "gname": "g2"},
		},
	}
	archive := writeStdTar(t, headers, map[string]string{"first": "a", "second": "b"})

	tr := NewReader(bytes.NewReader(archive)) // identity dropping is off by default
	first, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertIdentityPresent(t, first, 11, 12, "u1", "g1")
	assertPaxIdentityPresent(t, first)
	if _, err := io.ReadAll(tr); err != nil {
		t.Fatalf("draining first entry: %v", err)
	}

	// Switching after an entry was returned must only affect subsequent Next calls.
	tr.SetSecurityMode(DefaultSecurityMode | DropUserIdentity)
	second, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertNoIdentity(t, second)

	// The header already handed out is not rewritten retroactively.
	assertIdentityPresent(t, first, 11, 12, "u1", "g1")
	assertPaxIdentityPresent(t, first)
}

func TestDropUserIdentityTarDefaultKeepsIdentity(t *testing.T) {
	if DefaultSecurityMode&DropUserIdentity != 0 {
		t.Fatal("DropUserIdentity must not be part of DefaultSecurityMode")
	}
	headers := []*tar.Header{{
		Name: "f", Mode: 0640, Size: 1,
		Uid: 1201, Gid: 1202, Uname: "alice", Gname: "staff",
		Format:     tar.FormatPAX,
		PAXRecords: map[string]string{"uid": "1201", "gid": "1202", "uname": "alice", "gname": "staff"},
	}}
	archive := writeStdTar(t, headers, map[string]string{"f": "x"})

	tr := NewReader(bytes.NewReader(archive))
	h, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertIdentityPresent(t, h, 1201, 1202, "alice", "staff")
	assertPaxIdentityPresent(t, h)
}

func TestDropUserIdentityTarMaximumMode(t *testing.T) {
	if MaximumSecurityMode&DropUserIdentity == 0 {
		t.Fatal("DropUserIdentity must be part of MaximumSecurityMode")
	}
	headers := []*tar.Header{{
		Name: "f", Mode: 0640, Size: 1,
		Uid: 1201, Gid: 1202, Uname: "alice", Gname: "staff", Format: tar.FormatPAX,
		PAXRecords: map[string]string{"uid": "9999", "gname": "pax-group"},
	}}
	archive := writeStdTar(t, headers, map[string]string{"f": "x"})

	tr := NewReader(bytes.NewReader(archive))
	tr.SetSecurityMode(MaximumSecurityMode)
	h, err := tr.Next()
	if err != nil {
		t.Fatalf("Next() error = %v", err)
	}
	assertNoIdentity(t, h)
}
