// Managed downloads from a remote Selenium Grid node.
//
// Selenium Grid can be started with the "se:downloadsEnabled" capability, in
// which case files downloaded by the remote browser are exposed through the
// JSON "se/files" endpoint of the session. See
// https://www.selenium.dev/documentation/grid/configuration/cli_options/#manage-downloads-in-a-grid .
package selenium

import (
	"archive/zip"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path"
	"path/filepath"
	"strings"
)

// DownloadLimits bounds a DownloadSnapshot call. Every field must be positive.
type DownloadLimits struct {
	// MaxArchiveBytes is the maximum accepted size, in bytes, of a single
	// downloaded ZIP archive after base64 decoding.
	MaxArchiveBytes int64

	// MaxExpandedBytes is the maximum total number of bytes written from the
	// contents of all regular file entries in all downloaded archives.
	MaxExpandedBytes int64

	// MaxFiles is the maximum total number of regular file entries accepted
	// across all downloaded archives. Directory entries do not count.
	MaxFiles int
}

// DownloadedFile describes one regular file published by DownloadSnapshot.
type DownloadedFile struct {
	// RemoteName is the opaque identifier of the remote download the file was
	// delivered in. It is never interpreted as a local path.
	RemoteName string

	// Path is the absolute local path of the file after the snapshot has been
	// published to the destination directory.
	Path string

	// Size is the number of bytes actually extracted from the archive for this
	// file, verified against the archive's CRC checksum.
	Size int64
}

// downloadTempDirPrefix prefixes the private staging directory created next to
// the destination. The leading dot keeps it out of the way of normal listings.
const downloadTempDirPrefix = ".download-snapshot-"

// DownloadSnapshot retrieves in one batch every download the remote end
// currently lists for the session and publishes them under destination.
//
// The remote end is queried exactly once for the list of download names; each
// name is then fetched in order from the session's "se/files" endpoint. Each
// response carries a base64-encoded ZIP archive. The archives are validated and
// extracted into a private staging directory next to destination, and the
// staging directory is renamed to destination only after every archive and
// file has been processed successfully. On any error the staging directory is
// removed, no partial result is returned and destination is never created.
//
// A snapshot is one list selection plus one local publication: the remote
// contents are not assumed to be frozen, and this method does not decide when
// the browser has finished downloading. It never polls, replays the export
// action, or deletes remote files.
func (wd *remoteWD) DownloadSnapshot(destination string, limits DownloadLimits) ([]DownloadedFile, error) {
	if err := validateDownloadLimits(limits); err != nil {
		return nil, err
	}
	if err := validateDownloadDestination(destination); err != nil {
		return nil, err
	}
	destination = filepath.Clean(destination)

	filesURL := wd.requestURL("/session/%s/se/files", wd.id)

	// The list is fetched exactly once, before anything is created locally, so
	// an empty list leaves no directories behind.
	raw, err := wd.execute("GET", filesURL, nil)
	if err != nil {
		return nil, err
	}
	names, err := parseDownloadNames(raw)
	if err != nil {
		return nil, err
	}
	if len(names) == 0 {
		return nil, nil
	}
	seen := make(map[string]bool, len(names))
	for _, name := range names {
		if name == "" {
			return nil, errors.New("remote reported an empty download name")
		}
		if seen[name] {
			return nil, fmt.Errorf("remote reported duplicate download name %q", name)
		}
		seen[name] = true
	}

	parent := filepath.Dir(destination)
	staging, err := ioutil.TempDir(parent, downloadTempDirPrefix)
	if err != nil {
		return nil, err
	}
	published := false
	defer func() {
		if !published {
			os.RemoveAll(staging)
		}
	}()

	pub := &downloadPublisher{
		destination: destination,
		staging:     staging,
		limits:      limits,
		dirs:        make(map[string]bool),
		files:       make(map[string]bool),
	}

	for _, name := range names {
		requestData, err := json.Marshal(map[string]string{"name": name})
		if err != nil {
			return nil, err
		}
		raw, err := wd.execute("POST", filesURL, requestData)
		if err != nil {
			return nil, err
		}

		var reply struct {
			Value struct {
				Filename string `json:"filename"`
				Contents string `json:"contents"`
			} `json:"value"`
		}
		if err := json.Unmarshal(raw, &reply); err != nil {
			return nil, err
		}
		if reply.Value.Filename != name {
			return nil, fmt.Errorf("download reply filename %q does not match requested name %q", reply.Value.Filename, name)
		}
		archive, err := base64.StdEncoding.DecodeString(reply.Value.Contents)
		if err != nil {
			return nil, fmt.Errorf("decoding download %q: %w", name, err)
		}

		if err := pub.processArchive(name, archive); err != nil {
			return nil, err
		}
	}

	if err := os.Rename(staging, destination); err != nil {
		return nil, err
	}
	published = true
	return pub.results, nil
}

func validateDownloadLimits(limits DownloadLimits) error {
	if limits.MaxArchiveBytes <= 0 {
		return errors.New("DownloadLimits.MaxArchiveBytes must be positive")
	}
	if limits.MaxExpandedBytes <= 0 {
		return errors.New("DownloadLimits.MaxExpandedBytes must be positive")
	}
	if limits.MaxFiles <= 0 {
		return errors.New("DownloadLimits.MaxFiles must be positive")
	}
	return nil
}

// validateDownloadDestination checks all local preconditions without sending
// any request.
func validateDownloadDestination(destination string) error {
	if destination == "" {
		return errors.New("destination must not be empty")
	}
	if !filepath.IsAbs(destination) {
		return fmt.Errorf("destination %q must be an absolute path", destination)
	}
	destination = filepath.Clean(destination)

	parent := filepath.Dir(destination)
	info, err := os.Stat(parent)
	if err != nil {
		return fmt.Errorf("destination parent directory: %w", err)
	}
	if !info.IsDir() {
		return fmt.Errorf("destination parent %q is not an existing directory", parent)
	}

	// Lstat so that an existing symlink is rejected even when its target is
	// missing.
	switch _, err := os.Lstat(destination); {
	case err == nil:
		return fmt.Errorf("destination %q already exists", destination)
	case !os.IsNotExist(err):
		return fmt.Errorf("inspecting destination %q: %w", destination, err)
	}
	return nil
}

func parseDownloadNames(raw json.RawMessage) ([]string, error) {
	var reply struct {
		Value struct {
			Names []string `json:"names"`
		} `json:"value"`
	}
	if err := json.Unmarshal(raw, &reply); err != nil {
		return nil, err
	}
	return reply.Value.Names, nil
}

// downloadPublisher owns the shared path namespace and budgets of one snapshot.
type downloadPublisher struct {
	destination string
	staging     string
	limits      DownloadLimits

	// dirs and files key on cleaned slash-separated entry paths, shared by all
	// archives of the batch. dirs includes directories created implicitly.
	dirs  map[string]bool
	files map[string]bool

	fileCount int
	expanded  int64
	results   []DownloadedFile
}

// processArchive validates one ZIP archive and extracts it into the staging
// directory, enforcing every per-archive and batch limit.
func (pub *downloadPublisher) processArchive(remoteName string, archive []byte) error {
	if int64(len(archive)) > pub.limits.MaxArchiveBytes {
		return fmt.Errorf("download %q archive is %d bytes, exceeding MaxArchiveBytes of %d",
			remoteName, len(archive), pub.limits.MaxArchiveBytes)
	}

	reader, err := zip.NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		return fmt.Errorf("opening download %q as a ZIP archive: %w", remoteName, err)
	}

	// Files are visited in central directory order.
	for _, entry := range reader.File {
		mode := entry.Mode()
		switch {
		case mode.IsDir():
			clean, err := cleanZipEntryName(entry.Name, true)
			if err != nil {
				return err
			}
			if err := pub.addDirectory(clean); err != nil {
				return err
			}
		case mode.IsRegular():
			clean, err := cleanZipEntryName(entry.Name, false)
			if err != nil {
				return err
			}
			readCloser, err := entry.Open()
			if err != nil {
				return fmt.Errorf("opening archive entry %q: %w", entry.Name, err)
			}
			err = pub.addFile(remoteName, clean, readCloser)
			if closeErr := readCloser.Close(); err == nil {
				// Surface the CRC verification error from Close when addFile
				// did not already fail.
				err = closeErr
			}
			if err != nil {
				return err
			}
		default:
			return fmt.Errorf("archive entry %q has unsupported type %s; only regular files and directories are accepted",
				entry.Name, mode)
		}
	}
	return nil
}

func (pub *downloadPublisher) addDirectory(clean string) error {
	if pub.files[clean] {
		return fmt.Errorf("archive directory %q conflicts with a file of the same name", clean)
	}
	// An explicit directory entry may coincide with an implicitly created one.
	pub.dirs[clean] = true
	return os.MkdirAll(filepath.Join(pub.staging, filepath.FromSlash(clean)), 0700)
}

func (pub *downloadPublisher) addFile(remoteName, clean string, contents io.Reader) error {
	if pub.files[clean] {
		return fmt.Errorf("duplicate file entry %q", clean)
	}
	if pub.dirs[clean] {
		return fmt.Errorf("file entry %q conflicts with a directory of the same name", clean)
	}
	segments := strings.Split(clean, "/")
	for i := 1; i < len(segments); i++ {
		ancestor := strings.Join(segments[:i], "/")
		if pub.files[ancestor] {
			return fmt.Errorf("file entry %q is an ancestor of another file entry %q", ancestor, clean)
		}
		pub.dirs[ancestor] = true
	}
	if pub.fileCount >= pub.limits.MaxFiles {
		return fmt.Errorf("download batch exceeds MaxFiles of %d", pub.limits.MaxFiles)
	}

	localPath := filepath.Join(pub.staging, filepath.FromSlash(clean))
	// Defense in depth: a validated name must always stay within staging.
	if !isWithinDir(pub.staging, localPath) {
		return fmt.Errorf("archive entry %q escapes the staging directory", clean)
	}
	if err := os.MkdirAll(filepath.Dir(localPath), 0700); err != nil {
		return err
	}

	out, err := os.OpenFile(localPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}

	// Stream the entry to disk: only io.Copy's bounded buffer is held in
	// memory, and the running batch total is checked against actual bytes.
	writer := &limitedWriter{
		w:         out,
		remaining: pub.limits.MaxExpandedBytes - pub.expanded,
		limit:     pub.limits.MaxExpandedBytes,
	}
	copied, copyErr := io.Copy(writer, contents)
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	if closeErr != nil {
		return closeErr
	}

	pub.expanded += copied
	pub.fileCount++
	pub.files[clean] = true
	pub.results = append(pub.results, DownloadedFile{
		RemoteName: remoteName,
		Path:       filepath.Join(pub.destination, filepath.FromSlash(clean)),
		Size:       copied,
	})
	return nil
}

// limitedWriter fails as soon as writing another byte would cross the batch's
// MaxExpandedBytes budget.
type limitedWriter struct {
	w         io.Writer
	remaining int64
	limit     int64
}

func (l *limitedWriter) Write(p []byte) (int, error) {
	if int64(len(p)) > l.remaining {
		return 0, fmt.Errorf("download batch exceeds MaxExpandedBytes of %d", l.limit)
	}
	n, err := l.w.Write(p)
	l.remaining -= int64(n)
	return n, err
}

// cleanZipEntryName validates an archive entry name and returns its normalized
// slash-separated form. Names are interpreted strictly as ZIP slash paths and
// are never passed to the shell.
func cleanZipEntryName(name string, isDir bool) (string, error) {
	if name == "" {
		return "", errors.New("archive contains an entry with an empty name")
	}
	if strings.ContainsRune(name, 0) {
		return "", fmt.Errorf("archive entry %q contains a NUL byte", name)
	}
	if strings.ContainsRune(name, '\\') {
		return "", fmt.Errorf("archive entry %q contains a backslash", name)
	}
	// Windows-style volume prefix such as "C:".
	if len(name) >= 2 && isASCIILetter(name[0]) && name[1] == ':' {
		return "", fmt.Errorf("archive entry %q has a volume prefix", name)
	}
	if path.IsAbs(name) {
		return "", fmt.Errorf("archive entry %q is an absolute path", name)
	}

	trailingSlash := strings.HasSuffix(name, "/")
	if trailingSlash && !isDir {
		return "", fmt.Errorf("archive entry %q has a trailing slash but is not a directory", name)
	}

	parts := strings.Split(name, "/")
	if trailingSlash {
		parts = parts[:len(parts)-1]
	}
	for _, part := range parts {
		if part == "" || part == "." || part == ".." {
			return "", fmt.Errorf("archive entry %q contains an empty, '.' or '..' segment", name)
		}
	}
	clean := strings.Join(parts, "/")
	if clean == "" {
		return "", fmt.Errorf("archive entry %q does not name a path", name)
	}
	return clean, nil
}

func isASCIILetter(b byte) bool {
	return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
}

func isWithinDir(dir, target string) bool {
	rel, err := filepath.Rel(dir, target)
	if err != nil {
		return false
	}
	return rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}
