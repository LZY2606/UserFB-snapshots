package matching

import "unicode/utf8"

// runeSlicer keeps the byte offset of every rune in a valid UTF-8 password so
// that matcher coordinates (zero-based, inclusive Unicode rune indices) can
// always be mapped back to a substring of the original input. The original
// string is never normalized, case-folded or rewritten: derived views such as
// the lowercased dictionary view or a leet permutation are lookup-only.
type runeSlicer struct {
	runes []rune
	// start[i] is the byte offset of rune i; start[n] equals len(password).
	start []int
}

func newRuneSlicer(password string) runeSlicer {
	n := utf8.RuneCountInString(password)
	rs := runeSlicer{
		runes: make([]rune, 0, n),
		start: make([]int, 0, n+1),
	}
	for i := 0; i < len(password); {
		rs.start = append(rs.start, i)
		r, size := utf8.DecodeRuneInString(password[i:])
		rs.runes = append(rs.runes, r)
		i += size
	}
	rs.start = append(rs.start, len(password))
	return rs
}

func (s runeSlicer) len() int {
	return len(s.runes)
}

// slice returns the original input substring covering runes i through j
// (inclusive).
func (s runeSlicer) slice(password string, i, j int) string {
	return password[s.start[i]:s.start[j+1]]
}

// runeIndex converts a byte offset that points at a rune boundary into the
// index of that rune.
func (s runeSlicer) runeIndex(bytePos int) int {
	lo, hi := 0, len(s.start)
	for lo < hi {
		mid := (lo + hi) / 2
		if s.start[mid] < bytePos {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo
}
