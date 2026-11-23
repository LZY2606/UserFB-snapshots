package browser

import (
	"bytes"
	"mime"
	"net/url"
	"sort"
	"strings"
	"unicode/utf8"

	"golang.org/x/net/html"
	"golang.org/x/text/encoding"
	"golang.org/x/text/encoding/charmap"
	"golang.org/x/text/encoding/japanese"
)

// Canonical character set names used by Surf. DocumentCharset and the
// _charset_ form control always report one of these names.
const (
	charsetUTF8        = "utf-8"
	charsetWindows1252 = "windows-1252"
	charsetShiftJIS    = "shift_jis"
)

// utf8BOM is the byte order mark of a UTF-8 document.
var utf8BOM = []byte{0xef, 0xbb, 0xbf}

// metaPrescanLimit is the number of raw entity bytes scanned for charset
// declarations, matching the HTML prescan algorithm.
const metaPrescanLimit = 1024

// supportedCharset maps a canonical name to the encoding Surf supports for
// both document decoding and form submission. Only the encodings listed here
// are recognised; unknown labels are ignored and the search continues.
var supportedCharset = map[string]encoding.Encoding{
	charsetUTF8:        encoding.Nop,
	charsetWindows1252: charmap.Windows1252,
	charsetShiftJIS:    japanese.ShiftJIS,
}

// charsetAliases maps the extra labels accepted by Surf to their canonical
// names. Lookup is case-insensitive after ASCII lower casing.
var charsetAliases = map[string]string{
	"utf-8":        charsetUTF8,
	"utf8":         charsetUTF8,
	"windows-1252": charsetWindows1252,
	"latin1":       charsetWindows1252,
	"iso-8859-1":   charsetWindows1252,
	"shift_jis":    charsetShiftJIS,
	"sjis":         charsetShiftJIS,
}

// resolveCharsetLabel normalises a charset label the way the HTML encoding
// label algorithm does: ASCII lower case, leading and trailing whitespace
// removed. It returns the canonical name and true when the label names one of
// the encodings Surf supports.
func resolveCharsetLabel(label string) (string, bool) {
	name, ok := charsetAliases[strings.ToLower(strings.TrimSpace(label))]
	return name, ok
}

// determineCharset picks the document character set following this order:
// UTF-8 BOM, a valid charset in the HTTP Content-Type header, a valid meta
// charset or http-equiv/content declaration within the first 1024 raw entity
// bytes, and finally UTF-8. Declarations that name an unsupported charset are
// skipped instead of stopping the search.
func determineCharset(body []byte, contentType string) string {
	if bytes.HasPrefix(body, utf8BOM) {
		return charsetUTF8
	}

	if _, params, err := mime.ParseMediaType(contentType); err == nil {
		if label, ok := params["charset"]; ok {
			if name, ok := resolveCharsetLabel(label); ok {
				return name
			}
		}
	}

	if name, ok := prescanMetaCharset(body); ok {
		return name
	}

	return charsetUTF8
}

// prescanMetaCharset scans the leading meta tags, up to the first 1024 entity
// bytes, for a charset declaration. Both <meta charset="..."> and
// <meta http-equiv="content-type" content="text/html; charset=..."> are
// honoured. The first valid, supported declaration wins.
func prescanMetaCharset(body []byte) (string, bool) {
	limit := len(body)
	if limit > metaPrescanLimit {
		limit = metaPrescanLimit
	}
	tokenizer := html.NewTokenizer(bytes.NewReader(body[:limit]))
	for {
		switch tokenizer.Next() {
		case html.ErrorToken:
			return "", false

		case html.StartTagToken, html.SelfClosingTagToken:
			tagName, hasAttr := tokenizer.TagName()
			if !bytes.Equal(tagName, []byte("meta")) {
				continue
			}

			gotHTTPEquivContentType := false
			var metaCharset, contentCharset string
			metaCharsetOK := false
			for hasAttr {
				key, val, more := tokenizer.TagAttr()
				hasAttr = more
				switch string(key) {
				case "charset":
					if name, ok := resolveCharsetLabel(string(val)); ok {
						metaCharset, metaCharsetOK = name, true
					}
				case "http-equiv":
					if strings.EqualFold(string(val), "content-type") {
						gotHTTPEquivContentType = true
					}
				case "content":
					if label, ok := charsetFromContentMeta(string(val)); ok {
						contentCharset = label
					}
				}
			}

			if metaCharsetOK {
				return metaCharset, true
			}
			if gotHTTPEquivContentType && contentCharset != "" {
				return contentCharset, true
			}
		}
	}
}

// charsetFromContentMeta extracts the charset parameter from a
// http-equiv="content-type" meta content value, for example
// "text/html; charset=windows-1252".
func charsetFromContentMeta(value string) (string, bool) {
	if _, params, err := mime.ParseMediaType(value); err == nil {
		if label, ok := params["charset"]; ok {
			return resolveCharsetLabel(label)
		}
	}
	return "", false
}

// decodeDocument converts the raw entity bytes to UTF-8 using the chosen
// character set. The UTF-8 BOM, when present, is stripped and never becomes
// part of the DOM.
func decodeDocument(body []byte, charsetName string) []byte {
	if bytes.HasPrefix(body, utf8BOM) {
		body = body[len(utf8BOM):]
	}
	enc, ok := supportedCharset[charsetName]
	if !ok || charsetName == charsetUTF8 {
		return body
	}
	decoded, err := enc.NewDecoder().Bytes(body)
	if err != nil {
		return body
	}
	return decoded
}

// formWireCharset chooses the submission charset from the whitespace
// separated accept-charset list, taking the first supported encoding. When
// the list contains nothing usable the document charset captured when the
// form was created is used instead.
func formWireCharset(acceptCharset, documentCharset string) string {
	for _, label := range strings.Fields(acceptCharset) {
		if name, ok := resolveCharsetLabel(label); ok {
			return name
		}
	}
	return documentCharset
}

// normalizeLineEndings turns bare CR and LF line breaks into CRLF, matching
// the HTML form submission rules.
func normalizeLineEndings(value string) string {
	if !strings.ContainsAny(value, "\r\n") {
		return value
	}
	var builder strings.Builder
	builder.Grow(len(value) + 8)
	for i := 0; i < len(value); i++ {
		switch value[i] {
		case '\r':
			builder.WriteString("\r\n")
			if i+1 < len(value) && value[i+1] == '\n' {
				i++
			}
		case '\n':
			builder.WriteString("\r\n")
		default:
			builder.WriteByte(value[i])
		}
	}
	return builder.String()
}

// encodeFormText prepares a single field name or value for the wire: line
// endings are normalized to CRLF first, then the text is converted to the
// submission charset. Runes the target charset cannot represent are written
// as decimal numeric character references, e.g. "😀" becomes "&#128512;".
func encodeFormText(value, charsetName string) ([]byte, error) {
	value = normalizeLineEndings(value)
	if charsetName == charsetUTF8 {
		if !utf8.ValidString(value) {
			value = strings.ToValidUTF8(value, "�")
		}
		return []byte(value), nil
	}
	enc, ok := supportedCharset[charsetName]
	if !ok {
		return []byte(value), nil
	}
	// HTMLEscapeUnsupported turns unrepresentable runes into &#NNNN; instead
	// of terminating the transform with an error.
	encoder := encoding.HTMLEscapeUnsupported(enc.NewEncoder())
	return encoder.Bytes([]byte(value))
}

// percentEncodeField transcodes a name or value and percent-encodes every
// resulting byte that is not an unreserved character (RFC 3986). Spaces are
// emitted as "+", matching application/x-www-form-urlencoded and the output
// of url.Values.Encode.
func percentEncodeField(value, charsetName string) (string, error) {
	b, err := encodeFormText(value, charsetName)
	if err != nil {
		return "", err
	}
	const upperHex = "0123456789ABCDEF"
	var builder strings.Builder
	for _, c := range b {
		switch {
		case c == ' ':
			builder.WriteByte('+')
		case isUnreservedByte(c):
			builder.WriteByte(c)
		default:
			builder.WriteByte('%')
			builder.WriteByte(upperHex[c>>4])
			builder.WriteByte(upperHex[c&0x0f])
		}
	}
	return builder.String(), nil
}

// isUnreservedByte reports whether the byte may appear unescaped in a query
// or application/x-www-form-urlencoded body.
func isUnreservedByte(c byte) bool {
	switch {
	case 'A' <= c && c <= 'Z', 'a' <= c && c <= 'z', '0' <= c && c <= '9':
		return true
	}
	switch c {
	case '-', '_', '.', '~':
		return true
	}
	return false
}

// encodeFormValues serializes form fields as
// application/x-www-form-urlencoded text, sorting field names the same way
// url.Values.Encode does while preserving each field's value order. Charset
// conversion and percent-encoding are performed on the transcoded bytes,
// never on the Unicode strings directly.
func encodeFormValues(values url.Values, charsetName string) (string, error) {
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)

	var builder strings.Builder
	first := true
	for _, name := range names {
		for _, value := range values[name] {
			if !first {
				builder.WriteByte('&')
			}
			encodedName, err := percentEncodeField(name, charsetName)
			if err != nil {
				return "", err
			}
			encodedValue, err := percentEncodeField(value, charsetName)
			if err != nil {
				return "", err
			}
			builder.WriteString(encodedName)
			builder.WriteByte('=')
			builder.WriteString(encodedValue)
			first = false
		}
	}
	return builder.String(), nil
}
