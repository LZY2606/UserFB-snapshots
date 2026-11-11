package grab

import (
	"fmt"
	"mime"
	"net/http"
	"strconv"
	"strings"
)

// contentRangeError marks any failure to interpret the Content-Range header of
// a resume response. Every such failure wraps ErrBadContentRange so callers can
// identify a range contract violation with errors.Is.
type contentRangeError struct {
	reason string
}

func (e *contentRangeError) Error() string {
	return ErrBadContentRange.Error() + ": " + e.reason
}

func (e *contentRangeError) Unwrap() error {
	return ErrBadContentRange
}

// parseDecimalUint parses s as a non-negative decimal integer consisting
// solely of decimal digits, with no sign, surrounding whitespace or any other
// character. It rejects values that overflow int64. The HTTP whitespace
// permitted around field elements is handled by the caller via trimOWS, so
// this function is intentionally strict.
func parseDecimalUint(s string, field string) (int64, error) {
	if s == "" {
		return 0, &contentRangeError{fmt.Sprintf("missing %s", field)}
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, &contentRangeError{fmt.Sprintf("malformed %s %q", field, s)}
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, &contentRangeError{fmt.Sprintf("%s %q out of range", field, s)}
	}
	return n, nil
}

// trimOWS removes the optional whitespace (RFC 9110 §5.5: SP and HTAB) that
// HTTP allows around a field element.
func trimOWS(s string) string {
	return strings.Trim(s, " \t")
}

// parseSingleByteRange validates the Content-Range header of a response to a
// ranged GET and returns the absolute byte range and the complete resource
// size. Only a single part "bytes start-end/total" response is accepted:
//
//   - the header must be present exactly once;
//   - the range unit must be "bytes";
//   - exactly one range with an explicit start and end must be present, so
//     unsatisfied ("bytes */total") and multipart responses are rejected;
//   - the total length must be explicit (a "*" total is rejected);
//   - start must equal the local resume offset and end must be within the
//     resource;
//   - a known Content-Length must equal the range length, and a known HEAD or
//     Request.Size total must match the advertised total.
//
// It does not read the response body and is safe to run before the
// destination file is opened.
func parseSingleByteRange(h http.Header, offset, headTotal, reqSize, contentLength int64) (start, end, total int64, err error) {
	values := h.Values("Content-Range")
	if len(values) == 0 {
		return 0, 0, 0, &contentRangeError{"missing Content-Range header"}
	}
	if len(values) > 1 {
		return 0, 0, 0, &contentRangeError{"Content-Range header appears more than once"}
	}

	// A multipart/byteranges response delivers several ranges in one body and
	// can never be appended to a single partially downloaded file.
	if mediaType, _, mtErr := mime.ParseMediaType(h.Get("Content-Type")); mtErr == nil &&
		mediaType == "multipart/byteranges" {
		return 0, 0, 0, &contentRangeError{"multipart/byteranges responses are not supported"}
	}

	// RFC 9110 §14.4: Content-Range = unit SP ( range-resp /
	// unsatisfied-range ). Only boundary OWS is permitted; the range-resp
	// grammar itself contains no whitespace.
	value := trimOWS(values[0])
	unit, rest, ok := strings.Cut(value, " ")
	if !ok || unit != "bytes" {
		return 0, 0, 0, &contentRangeError{fmt.Sprintf("unsupported or malformed Content-Range %q", values[0])}
	}
	rest = trimOWS(rest)
	if rest == "" || strings.ContainsAny(rest, " \t") {
		return 0, 0, 0, &contentRangeError{fmt.Sprintf("malformed Content-Range %q", values[0])}
	}

	rangePart, totalPart, ok := strings.Cut(rest, "/")
	if !ok {
		return 0, 0, 0, &contentRangeError{fmt.Sprintf("malformed Content-Range %q", values[0])}
	}
	rangePart = trimOWS(rangePart)
	totalPart = trimOWS(totalPart)
	if rangePart == "" || totalPart == "" {
		return 0, 0, 0, &contentRangeError{fmt.Sprintf("malformed Content-Range %q", values[0])}
	}

	// A "*" range-resp marks an unsatisfied range and carries no bytes.
	if rangePart == "*" {
		return 0, 0, 0, &contentRangeError{"unsatisfied-range response has no byte range"}
	}
	startPart, endPart, ok := strings.Cut(rangePart, "-")
	if !ok {
		return 0, 0, 0, &contentRangeError{fmt.Sprintf("malformed byte range %q", rangePart)}
	}
	start, err = parseDecimalUint(trimOWS(startPart), "range start")
	if err != nil {
		return 0, 0, 0, err
	}
	end, err = parseDecimalUint(trimOWS(endPart), "range end")
	if err != nil {
		return 0, 0, 0, err
	}

	if totalPart == "*" {
		return 0, 0, 0, &contentRangeError{"Content-Range with unknown total length is not supported"}
	}
	total, err = parseDecimalUint(totalPart, "total length")
	if err != nil {
		return 0, 0, 0, err
	}

	if start != offset {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("range start %d does not match resume offset %d", start, offset)}
	}
	if end < start {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("range end %d precedes range start %d", end, start)}
	}
	if total <= end {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("range end %d is not within total length %d", end, total)}
	}
	if headTotal > 0 && headTotal != total {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("total length %d does not match HEAD content length %d", total, headTotal)}
	}
	if reqSize > 0 && reqSize != total {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("total length %d does not match Request.Size %d", total, reqSize)}
	}
	if contentLength >= 0 && contentLength != end-start+1 {
		return 0, 0, 0, &contentRangeError{
			fmt.Sprintf("Content-Length %d does not match range length %d", contentLength, end-start+1)}
	}
	return start, end, total, nil
}
