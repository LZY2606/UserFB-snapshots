package unicode_regression

import (
	"math"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/nbutton23/zxcvbn-go"
	"github.com/nbutton23/zxcvbn-go/entropy"
	"github.com/nbutton23/zxcvbn-go/match"
	"github.com/nbutton23/zxcvbn-go/matching"
	"github.com/nbutton23/zxcvbn-go/scoring"
)

// assertOriginalIntervals checks that every candidate uses zero-based,
// inclusive Unicode rune coordinates whose token is exactly the corresponding
// slice of the untouched original password.
func assertOriginalIntervals(t *testing.T, password string, candidates []match.Match) {
	t.Helper()
	runes := []rune(password)
	for _, m := range candidates {
		if m.I < 0 || m.J < m.I || m.J >= len(runes) {
			t.Fatalf("candidate %q has out-of-range rune interval I=%d J=%d (password has %d runes)",
				m.Pattern, m.I, m.J, len(runes))
		}
		if !utf8.ValidString(m.Token) {
			t.Fatalf("candidate %q token %q is not valid UTF-8", m.Pattern, m.Token)
		}
		if want := string(runes[m.I : m.J+1]); m.Token != want {
			t.Fatalf("candidate %q at [%d:%d] has token %q, want original slice %q",
				m.Pattern, m.I, m.J, m.Token, want)
		}
	}
}

// assertSequenceReconstructs checks the final minimum-entropy sequence tiles
// the password exactly: ordered, non-overlapping, no gaps or leftovers.
func assertSequenceReconstructs(t *testing.T, password string, result scoring.MinEntropyMatch) {
	t.Helper()
	runes := []rune(password)
	var parts []string
	covered := 0
	for _, m := range result.MatchSequence {
		if m.I != covered {
			t.Fatalf("sequence gap/overlap before pattern %q: next I=%d, expected %d", m.Pattern, m.I, covered)
		}
		if m.J < m.I || m.J >= len(runes) {
			t.Fatalf("pattern %q has bad interval [%d:%d]", m.Pattern, m.I, m.J)
		}
		if want := string(runes[m.I : m.J+1]); m.Token != want {
			t.Fatalf("pattern %q token %q does not match original slice %q", m.Pattern, m.Token, want)
		}
		parts = append(parts, m.Token)
		covered = m.J + 1
	}
	if joined := strings.Join(parts, ""); joined != password {
		t.Fatalf("token concatenation %q != password %q", joined, password)
	}
	if covered != len(runes) {
		t.Fatalf("sequence covers %d runes, password has %d", covered, len(runes))
	}
	if result.Password != password {
		t.Fatalf("result password %q != input %q", result.Password, password)
	}
}

func findCandidates(candidates []match.Match, predicate func(match.Match) bool) []match.Match {
	var out []match.Match
	for _, m := range candidates {
		if predicate(m) {
			out = append(out, m)
		}
	}
	return out
}

// TestUnicodeCoordinates covers the coordinate convention at both the
// candidate layer and the final tiled result: multibyte prefixes and
// suffixes, combining marks, ZWJ, repeats, sequences, spatial chains, leet
// candidates, repeated dates, filters, user inputs, the empty string and
// plain ASCII.
func TestUnicodeCoordinates(t *testing.T) {
	// Minimal reported repro: a four-rune password with a 4-byte emoji.
	minimal := "🙂abc"
	minimalCandidates := matching.Omnimatch(minimal, []string{"abc"})
	assertOriginalIntervals(t, minimal, minimalCandidates)
	if got := findCandidates(minimalCandidates, func(m match.Match) bool {
		return m.DictionaryName == "user_inputs"
	}); len(got) != 1 || got[0].I != 1 || got[0].J != 3 || got[0].Token != "abc" {
		t.Fatalf("user_inputs candidate wrong: %+v", got)
	}
	minimalResult := zxcvbn.PasswordStrength(minimal, []string{"abc"})
	assertSequenceReconstructs(t, minimal, minimalResult)
	if minimalResult.Entropy != 5.883 {
		t.Fatalf("minimal repro entropy = %.3f, want 5.883", minimalResult.Entropy)
	}

	// Multibyte prefix followed by an ASCII keyboard sequence.
	emojiSeq := "🙂abcd"
	seqCandidates := matching.Omnimatch(emojiSeq, nil)
	assertOriginalIntervals(t, emojiSeq, seqCandidates)
	if got := findCandidates(seqCandidates, func(m match.Match) bool {
		return m.Pattern == "sequence" && m.Token == "abcd"
	}); len(got) != 1 || got[0].I != 1 || got[0].J != 4 {
		t.Fatalf("sequence after emoji prefix wrong: %+v", got)
	}
	assertSequenceReconstructs(t, emojiSeq, zxcvbn.PasswordStrength(emojiSeq, nil))

	// Multibyte prefix followed by a spatial chain.
	emojiSpatial := "🙂qwerty"
	spatialCandidates := matching.Omnimatch(emojiSpatial, nil)
	assertOriginalIntervals(t, emojiSpatial, spatialCandidates)
	if got := findCandidates(spatialCandidates, func(m match.Match) bool {
		return m.Pattern == "spatial" && m.Token == "qwerty"
	}); len(got) != 1 || got[0].I != 1 || got[0].J != 6 {
		t.Fatalf("spatial chain after emoji prefix wrong: %+v", got)
	}
	assertSequenceReconstructs(t, emojiSpatial, zxcvbn.PasswordStrength(emojiSpatial, nil))

	// Repeats of multibyte Han runes and accented letters must use rune
	// positions instead of subtracting byte offsets.
	for _, pw := range []string{"中中中", "ééé", "a中中中b"} {
		candidates := matching.Omnimatch(pw, nil)
		assertOriginalIntervals(t, pw, candidates)
		repeat := findCandidates(candidates, func(m match.Match) bool { return m.Pattern == "repeat" })
		if len(repeat) == 0 {
			t.Fatalf("no repeat candidate found in %q", pw)
		}
		assertSequenceReconstructs(t, pw, zxcvbn.PasswordStrength(pw, nil))
	}

	// Combining acute accent: the base letter and the mark keep separate
	// rune positions; the following ASCII repeat starts after both.
	combining := "éaaa" // e, combining acute (U+0301), a, a, a
	combiningCandidates := matching.Omnimatch(combining, nil)
	assertOriginalIntervals(t, combining, combiningCandidates)
	repeat := findCandidates(combiningCandidates, func(m match.Match) bool { return m.Pattern == "repeat" })
	if len(repeat) != 1 || repeat[0].I != 2 || repeat[0].J != 4 || repeat[0].Token != "aaa" {
		t.Fatalf("combining-mark repeat wrong: %+v", repeat)
	}
	assertSequenceReconstructs(t, combining, zxcvbn.PasswordStrength(combining, nil))

	// ZWJ occupies its own rune position.
	zwj := "a‍b" // a, ZWJ, b: the ZWJ keeps its own rune position
	assertOriginalIntervals(t, zwj, matching.Omnimatch(zwj, nil))
	assertSequenceReconstructs(t, zwj, zxcvbn.PasswordStrength(zwj, nil))

	// Leet candidates next to an emoji: tokens stay on the original text and
	// never borrow the emoji bytes.
	leet := "🙂1337"
	leetCandidates := matching.Omnimatch(leet, nil)
	assertOriginalIntervals(t, leet, leetCandidates)
	leetMatches := findCandidates(leetCandidates, func(m match.Match) bool {
		return strings.HasSuffix(m.DictionaryName, "_3117")
	})
	if len(leetMatches) == 0 {
		t.Fatalf("no leet candidates found in %q", leet)
	}
	for _, m := range leetMatches {
		if m.I < 1 || strings.ContainsRune(m.Token, '🙂') {
			t.Fatalf("leet candidate stole emoji bytes: %+v", m)
		}
	}
	assertSequenceReconstructs(t, leet, zxcvbn.PasswordStrength(leet, nil))

	// The same date text appearing twice keeps two distinct real positions
	// instead of both pointing at the first textual occurrence.
	datesTwice := "11.9.1991x11.9.1991"
	dateCandidates := matching.Omnimatch(datesTwice, nil)
	assertOriginalIntervals(t, datesTwice, dateCandidates)
	fullDates := findCandidates(dateCandidates, func(m match.Match) bool {
		return m.Pattern == "date" && m.Token == "11.9.1991"
	})
	if len(fullDates) != 2 {
		t.Fatalf("expected two full date candidates, got %+v", fullDates)
	}
	if fullDates[0].I != 0 || fullDates[0].J != 8 ||
		fullDates[1].I != 10 || fullDates[1].J != 18 {
		t.Fatalf("repeated date positions wrong: %+v", fullDates)
	}
	datesResult := zxcvbn.PasswordStrength(datesTwice, nil)
	assertSequenceReconstructs(t, datesTwice, datesResult)
	dateSegments := findCandidates(datesResult.MatchSequence, func(m match.Match) bool { return m.Pattern == "date" })
	if len(dateSegments) != 2 {
		t.Fatalf("expected two date segments, got %+v", dateSegments)
	}

	// Filtering every built-in matcher leaves only the user_inputs matcher.
	onlyUserInputs := matching.Omnimatch(minimal, []string{"abc"}, func(match.Matcher) bool { return true })
	assertOriginalIntervals(t, minimal, onlyUserInputs)
	if len(onlyUserInputs) != 1 || onlyUserInputs[0].DictionaryName != "user_inputs" ||
		onlyUserInputs[0].I != 1 || onlyUserInputs[0].J != 3 || onlyUserInputs[0].Token != "abc" {
		t.Fatalf("user_inputs-only filter wrong: %+v", onlyUserInputs)
	}
	onlyUserInputsResult := zxcvbn.PasswordStrength(minimal, []string{"abc"}, func(match.Matcher) bool { return true })
	assertSequenceReconstructs(t, minimal, onlyUserInputsResult)

	// A specific filter removes its matcher while the others stay usable.
	filteredRepeat := matching.Omnimatch("a中中中b", nil, matching.FilterRepeatMatcher)
	assertOriginalIntervals(t, "a中中中b", filteredRepeat)
	if got := findCandidates(filteredRepeat, func(m match.Match) bool { return m.Pattern == "repeat" }); len(got) != 0 {
		t.Fatalf("repeat filter did not remove repeat candidates: %+v", got)
	}

	// Empty input: no candidates, empty sequence, zero entropy.
	if got := matching.Omnimatch("", nil); len(got) != 0 {
		t.Fatalf("empty password produced candidates: %+v", got)
	}
	emptyResult := zxcvbn.PasswordStrength("", nil)
	if len(emptyResult.MatchSequence) != 0 || emptyResult.Entropy != 0 || emptyResult.Score != 0 {
		t.Fatalf("empty password result wrong: %+v", emptyResult)
	}

	// Plain ASCII keeps the same rune-coordinate convention.
	ascii := "abc"
	asciiCandidates := matching.Omnimatch(ascii, []string{"abc"})
	assertOriginalIntervals(t, ascii, asciiCandidates)
	if got := findCandidates(asciiCandidates, func(m match.Match) bool {
		return m.DictionaryName == "user_inputs"
	}); len(got) != 1 || got[0].I != 0 || got[0].J != 2 || got[0].Token != "abc" {
		t.Fatalf("ASCII user_inputs candidate wrong: %+v", got)
	}
	assertSequenceReconstructs(t, ascii, zxcvbn.PasswordStrength(ascii, []string{"abc"}))
}

// independentMinimum enumerates every non-overlapping subset of the handmade
// candidates and prices gaps with the same per-rune brute-force cost the
// scorer uses. It is deliberately independent of the scorer's dynamic
// program.
func independentMinimum(password string, candidates []match.Match) float64 {
	cardinality := entropy.CalcBruteForceCardinality(password)
	perRune := math.Log2(cardinality)
	n := utf8.RuneCountInString(password)
	best := float64(n) * perRune // all-brute-force tiling

	for mask := 0; mask < (1 << len(candidates)); mask++ {
		occupied := make([]bool, n)
		cost := 0.0
		valid := true
		for bit := 0; bit < len(candidates); bit++ {
			if mask&(1<<bit) == 0 {
				continue
			}
			m := candidates[bit]
			for r := m.I; r <= m.J; r++ {
				if occupied[r] {
					valid = false
					break
				}
				occupied[r] = true
			}
			if !valid {
				break
			}
			cost += m.Entropy
		}
		if !valid {
			continue
		}
		for r := 0; r < n; r++ {
			if !occupied[r] {
				cost += perRune
			}
		}
		if cost < best {
			best = cost
		}
	}
	return best
}

// TestUnicodeScoring checks the minimum-entropy dynamic program against an
// exhaustive subset enumeration over hand-written rune intervals and costs,
// including gaps that span multibyte runes.
func TestUnicodeScoring(t *testing.T) {
	// Six runes: emoji, a, b, emoji, c, d. Brute-force gaps must be priced
	// per rune, not per UTF-8 byte.
	password := "🙂ab🙂cd"
	handmade := []match.Match{
		{Pattern: "dictionary", I: 1, J: 2, Token: "ab", Entropy: 1.0},
		{Pattern: "dictionary", I: 4, J: 5, Token: "cd", Entropy: 2.0},
		{Pattern: "spatial", I: 0, J: 3, Token: "🙂ab🙂", Entropy: 30.0},
		{Pattern: "repeat", I: 1, J: 5, Token: "ab🙂cd", Entropy: 4.0},
	}
	for _, m := range handmade {
		if want := string([]rune(password)[m.I : m.J+1]); m.Token != want {
			t.Fatalf("test setup error: token %q != %q", m.Token, want)
		}
	}

	want := independentMinimum(password, handmade)
	result := scoring.MinimumEntropyMatchSequence(password, handmade)
	if math.Abs(result.Entropy-want) > 0.0011 {
		t.Fatalf("scorer entropy %.4f != exhaustive minimum %.4f", result.Entropy, want)
	}
	assertSequenceReconstructs(t, password, result)

	// All-brute-force password: two emoji runes must be counted as two
	// guesses-worth of length, not eight bytes.
	emojiOnly := "🙂🙂"
	emojiResult := scoring.MinimumEntropyMatchSequence(emojiOnly, nil)
	emojiWant := 2 * math.Log2(entropy.CalcBruteForceCardinality(emojiOnly))
	if math.Abs(emojiResult.Entropy-emojiWant) > 0.0011 {
		t.Fatalf("brute-force emoji entropy %.4f != %.4f", emojiResult.Entropy, emojiWant)
	}
	assertSequenceReconstructs(t, emojiOnly, emojiResult)
	if len(emojiResult.MatchSequence) != 1 ||
		emojiResult.MatchSequence[0].Pattern != "bruteforce" ||
		emojiResult.MatchSequence[0].I != 0 || emojiResult.MatchSequence[0].J != 1 ||
		emojiResult.MatchSequence[0].Token != emojiOnly {
		t.Fatalf("emoji brute-force tiling wrong: %+v", emojiResult.MatchSequence)
	}

	// Empty password stays zero entropy with an empty tiling.
	emptyResult := scoring.MinimumEntropyMatchSequence("", nil)
	if emptyResult.Entropy != 0 || len(emptyResult.MatchSequence) != 0 {
		t.Fatalf("empty scoring result wrong: %+v", emptyResult)
	}
}

// TestUnicodeCompatibility pins the observable behavior of ASCII inputs that
// already had correct coordinates, and checks the Unicode fixes end to end.
func TestUnicodeCompatibility(t *testing.T) {
	cases := []struct {
		password   string
		userInputs []string
		entropy    float64
		score      int
		segments   int
		firstToken string
	}{
		{"abc", []string{"abc"}, 0.0, 0, 1, "abc"},
		{"zxcvbn", nil, 6.845, 0, 1, "zxcvbn"},
		{"12345678", nil, 1.585, 0, 1, "12345678"},
		{"rosebud", nil, 7.937, 0, 1, "rosebud"},
		{"correcthorsebatterystaple", nil, 45.212, 4, 4, "correct"},
	}
	for _, c := range cases {
		result := zxcvbn.PasswordStrength(c.password, c.userInputs)
		assertSequenceReconstructs(t, c.password, result)
		if result.Entropy != c.entropy {
			t.Fatalf("%q entropy = %.3f, want %.3f", c.password, result.Entropy, c.entropy)
		}
		if result.Score != c.score {
			t.Fatalf("%q score = %d, want %d", c.password, result.Score, c.score)
		}
		if len(result.MatchSequence) != c.segments {
			t.Fatalf("%q segment count = %d, want %d", c.password, len(result.MatchSequence), c.segments)
		}
		if result.MatchSequence[0].Token != c.firstToken {
			t.Fatalf("%q first token = %q, want %q", c.password, result.MatchSequence[0].Token, c.firstToken)
		}
	}

	// Date recognition rules are unchanged, but the date segment now carries
	// its real interval and the date type end to end.
	datePW := "neverforget13/3/1997"
	dateResult := zxcvbn.PasswordStrength(datePW, nil)
	assertSequenceReconstructs(t, datePW, dateResult)
	if dateResult.Entropy != 32.628 || dateResult.Score != 2 {
		t.Fatalf("date password entropy/score wrong: %.3f/%d", dateResult.Entropy, dateResult.Score)
	}
	dateSegments := findCandidates(dateResult.MatchSequence, func(m match.Match) bool { return m.Pattern == "date" })
	if len(dateSegments) != 1 {
		t.Fatalf("expected one date segment, got %+v", dateSegments)
	}
	if dateSegments[0].I != 11 || dateSegments[0].J != 19 || dateSegments[0].Token != "13/3/1997" {
		t.Fatalf("date segment interval wrong: %+v", dateSegments[0])
	}

	// The Unicode minimal repro end to end.
	minimal := zxcvbn.PasswordStrength("🙂abc", []string{"abc"})
	assertSequenceReconstructs(t, "🙂abc", minimal)
	if minimal.Entropy != 5.883 {
		t.Fatalf("minimal Unicode entropy = %.3f, want 5.883", minimal.Entropy)
	}

	// Empty input remains a defined zero result.
	empty := zxcvbn.PasswordStrength("", nil)
	if empty.Entropy != 0 || empty.Score != 0 || len(empty.MatchSequence) != 0 {
		t.Fatalf("empty password result wrong: %+v", empty)
	}
}
