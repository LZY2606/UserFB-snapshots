package links

import (
	"io"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/tdewolff/parse/v2"
	"github.com/tdewolff/parse/v2/css"
)

// CSSReference is a single reference found in a css source.
//
// Import is true for a top-level @import target, and false for any other
// resource address (an url() token).
type CSSReference struct {
	URL    string
	Import bool
}

// ExtractCSSReferences extracts top-level @import targets and url() resources
// from css source. Returned addresses are not resolved against any base.
//
// Only the first string or url() of an @import prelude is considered; its
// layer/supports/media conditions are not evaluated nor treated as resource
// sources. Ordinary resources keep their source order and duplicates, imports
// are reported once per statement in their source order, and @import
// statements nested in blocks are ignored entirely. Empty addresses are not
// reported.
func ExtractCSSReferences(r io.Reader, h func(CSSReference)) {
	var (
		l          = css.NewLexer(parse.NewInput(r))
		depth      int
		pending    bool // an @import prelude after @import itself
		importDone bool // the first target candidate of the prelude was seen
		nested     bool // inside an @import nested in a block
		parens     int  // parenthesis nesting inside the current prelude
	)

	// reset drops the current statement state.
	reset := func() {
		pending, importDone, nested, parens = false, false, false, 0
	}

	for {
		tt, text := l.Next()
		switch tt {
		case css.ErrorToken:
			return

		case css.SemicolonToken:
			// Semicolons only terminate a statement at the top parenthesis
			// level; a media condition may contain them in nested parens.
			if parens == 0 {
				reset()
			}

		case css.LeftBraceToken:
			reset()
			depth++

		case css.RightBraceToken:
			reset()

			if depth > 0 {
				depth--
			}

		case css.LeftParenthesisToken:
			if pending || nested {
				parens++
			}

		case css.RightParenthesisToken:
			if parens > 0 {
				parens--
			}

		case css.AtKeywordToken:
			if !isImportKeyword(text) {
				continue
			}

			reset()

			if depth > 0 {
				// An @import nested in a block is ignored entirely.
				nested = true
			} else {
				pending = true
			}

		case css.StringToken:
			if nested {
				continue
			}

			if pending && !importDone {
				if res, ok := cssStringValue(text); ok {
					importDone = true

					h(CSSReference{URL: res, Import: true})
				}
			}

		case css.URLToken:
			switch {
			case nested:
			case pending:
				if !importDone {
					if res, ok := cssURLValue(text); ok {
						importDone = true

						h(CSSReference{URL: res, Import: true})
					}
				}
			default:
				if res, ok := cssURLValue(text); ok {
					h(CSSReference{URL: res, Import: false})
				}
			}
		}
	}
}

func isImportKeyword(v []byte) bool {
	if len(v) < 2 || v[0] != '@' {
		return false
	}

	return asciiEqualFold(string(v[1:]), "import")
}

// asciiEqualFold compares two strings ASCII case-insensitively.
func asciiEqualFold(a, b string) bool {
	if len(a) != len(b) {
		return false
	}

	for i := 0; i < len(a); i++ {
		x, y := a[i], b[i]

		if 'A' <= x && x <= 'Z' {
			x += 'a' - 'A'
		}

		if 'A' <= y && y <= 'Z' {
			y += 'a' - 'A'
		}

		if x != y {
			return false
		}
	}

	return true
}

func cssStringValue(v []byte) (rv string, ok bool) {
	rv = unescapeCSS(dropQuotes(string(v)))
	ok = rv != ""

	return rv, ok
}

func cssURLValue(v []byte) (rv string, ok bool) {
	o := strings.IndexByte(string(v), braceOpen)
	c := strings.LastIndexByte(string(v), braceClose)

	if o == -1 || c <= o {
		return "", false
	}

	rv = unescapeCSS(dropQuotes(strings.Trim(string(v[o+1:c]), cssSpaceChars)))
	ok = rv != ""

	return rv, ok
}

const cssSpaceChars = " \t\n\r\f"

func dropQuotes(s string) (rv string) {
	if len(s) < 2 {
		return s
	}

	if q := s[0]; (q == '"' || q == '\'') && s[len(s)-1] == q {
		return s[1 : len(s)-1]
	}

	return s
}

func isCSSSpace(c byte) bool {
	switch c {
	case ' ', '\t', '\n', '\r', '\f':
		return true
	}

	return false
}

func isHexDigit(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

const (
	maxHexEscape = 6
	maxCodePoint = 0x10FFFF
)

// unescapeCSS decodes css escape sequences in a string or url token body.
func unescapeCSS(s string) (rv string) {
	if !strings.ContainsRune(s, '\\') {
		return s
	}

	var sb strings.Builder

	sb.Grow(len(s))

	for i := 0; i < len(s); {
		if s[i] != '\\' {
			sb.WriteByte(s[i])
			i++

			continue
		}

		i++ // consume '\'

		if i >= len(s) {
			break
		}

		c := s[i]
		switch {
		case c == '\n' || c == '\f':
			i++ // escaped newline is a line continuation

			continue

		case c == '\r':
			i++
			if i < len(s) && s[i] == '\n' {
				i++
			}

			continue

		case isHexDigit(c):
			j, n := i, 0

			for j < len(s) && n < maxHexEscape && isHexDigit(s[j]) {
				j++
				n++
			}

			v, _ := strconv.ParseUint(s[i:j], 16, 32)

			i = j

			// A single whitespace character terminates the escape.
			if i < len(s) {
				switch s[i] {
				case '\r':
					i++
					if i < len(s) && s[i] == '\n' {
						i++
					}
				default:
					if isCSSSpace(s[i]) {
						i++
					}
				}
			}

			r := rune(v)
			if v == 0 || v > maxCodePoint || (v >= 0xD800 && v <= 0xDFFF) {
				r = utf8.RuneError
			}

			sb.WriteRune(r)

			continue

		case c >= utf8.RuneSelf:
			r, size := utf8.DecodeRuneInString(s[i:])
			sb.WriteRune(r)
			i += size

			continue
		}

		sb.WriteByte(c)
		i++
	}

	return sb.String()
}
