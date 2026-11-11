// Copyright 2021 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package demangle

import (
	"math"
	"math/bits"
	"strings"
)

// oldRustToString demangles a Rust symbol using the old demangling.
// The second result reports whether this is a valid Rust mangled name.
//
// This code path is intentionally untouched by the Rust v0 AST work:
// legacy _ZN symbols never go through ToRustAST/RustASTToString.
func oldRustToString(name string, options []Option) (string, bool) {
	max := 0
	for _, o := range options {
		if isMaxLength(o) {
			max = maxLength(o)
		}
	}

	// We know that the string starts with _ZN.
	name = name[3:]

	hexDigit := func(c byte) (byte, bool) {
		switch {
		case c >= '0' && c <= '9':
			return c - '0', true
		case c >= 'a' && c <= 'f':
			return c - 'a' + 10, true
		default:
			return 0, false
		}
	}

	// We know that the strings end with "17h" followed by 16 characters
	// followed by "E". We check that the 16 characters are all hex digits.
	// Also the hex digits must contain at least 5 distinct digits.
	seen := uint16(0)
	for i := len(name) - 17; i < len(name)-1; i++ {
		digit, ok := hexDigit(name[i])
		if !ok {
			return "", false
		}
		seen |= 1 << digit
	}
	if bits.OnesCount16(seen) < 5 {
		return "", false
	}
	name = name[:len(name)-20]

	// The name is a sequence of length-preceded identifiers.
	var sb strings.Builder
	for len(name) > 0 {
		if max > 0 && sb.Len() > max {
			break
		}

		if !isDigit(name[0]) {
			return "", false
		}

		val := 0
		for len(name) > 0 && isDigit(name[0]) {
			add := int(name[0] - '0')
			if val >= math.MaxInt32/10-add {
				return "", false
			}
			val *= 10
			val += add
			name = name[1:]
		}

		if val == 0 {
			return "", false
		}

		// An optional trailing underscore can separate the
		// length from the identifier.
		if len(name) > 0 && name[0] == '_' {
			name = name[1:]
			val--
		}

		if len(name) < val {
			return "", false
		}

		id := name[:val]
		name = name[val:]

		if sb.Len() > 0 {
			sb.WriteString("::")
		}

		// Ignore leading underscores preceding escape sequences.
		if strings.HasPrefix(id, "_$") {
			id = id[1:]
		}

		// The identifier can have escape sequences.
	escape:
		for len(id) > 0 {
			switch c := id[0]; c {
			case '$':
				codes := map[string]byte{
					"SP": '@',
					"BP": '*',
					"RF": '&',
					"LT": '<',
					"GT": '>',
					"LP": '(',
					"RP": ')',
				}

				valid := true
				if len(id) > 2 && id[1] == 'C' && id[2] == '$' {
					sb.WriteByte(',')
					id = id[3:]
				} else if len(id) > 4 && id[1] == 'u' && id[4] == '$' {
					dig1, ok1 := hexDigit(id[2])
					dig2, ok2 := hexDigit(id[3])
					val := (dig1 << 4) | dig2
					if !ok1 || !ok2 || dig1 > 7 || val < ' ' {
						valid = false
					} else {
						sb.WriteByte(val)
						id = id[5:]
					}
				} else if len(id) > 3 && id[3] == '$' {
					if code, ok := codes[id[1:3]]; !ok {
						valid = false
					} else {
						sb.WriteByte(code)
						id = id[4:]
					}
				} else {
					valid = false
				}
				if !valid {
					sb.WriteString(id)
					break escape
				}
			case '.':
				if strings.HasPrefix(id, "..") {
					sb.WriteString("::")
					id = id[2:]
				} else {
					sb.WriteByte(c)
					id = id[1:]
				}
			default:
				sb.WriteByte(c)
				id = id[1:]
			}
		}
	}

	s := sb.String()
	if max > 0 && len(s) > max {
		s = s[:max]
	}
	return s, true
}
