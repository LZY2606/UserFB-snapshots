package grab

import (
	"errors"
	"fmt"
	"net/http"
)

var (
	// ErrBadLength indicates that the server response or an existing file does
	// not match the expected content length.
	ErrBadLength = errors.New("bad content length")

	// ErrBadChecksum indicates that a downloaded file failed to pass checksum
	// validation.
	ErrBadChecksum = errors.New("checksum mismatch")

	// ErrNoFilename indicates that a reasonable filename could not be
	// automatically determined using the URL or response headers from a server.
	ErrNoFilename = errors.New("no filename could be determined")

	// ErrNoTimestamp indicates that a timestamp could not be automatically
	// determined using the response headers from the remote server.
	ErrNoTimestamp = errors.New("no timestamp could be determined for the remote file")

	// ErrFileExists indicates that the destination path already exists.
	ErrFileExists = errors.New("file exists")

	// ErrNotADirectory indicates that the given destination path exists but is
	// not a directory.
	ErrNotADirectory = errors.New("destination is not a directory")

	// ErrBadContentRange indicates that a response to a ranged (resume)
	// request did not carry a single, well-formed byte Content-Range matching
	// the offset from which the transfer was asked to resume. Appending such
	// a response to the partially downloaded file would corrupt it, so the
	// transfer is failed before the destination is opened for writing.
	ErrBadContentRange = errors.New("grab: invalid or mismatched Content-Range in resume response")
)

// StatusCodeError indicates that the server response had a status code that
// was not in the 200-299 range (after following any redirects).
type StatusCodeError int

func (err StatusCodeError) Error() string {
	return fmt.Sprintf("server returned %d %s", err, http.StatusText(int(err)))
}

// IsStatusCodeError returns true if the given error is of type StatusCodeError.
func IsStatusCodeError(err error) bool {
	var codeErr StatusCodeError
	return errors.As(err, &codeErr)
}
