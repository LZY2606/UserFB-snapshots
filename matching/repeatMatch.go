package matching

import (
	"strings"

	"github.com/nbutton23/zxcvbn-go/entropy"
	"github.com/nbutton23/zxcvbn-go/match"
)

const repeatMatcherName = "REPEAT"

//FilterRepeatMatcher can be pass to zxcvbn-go.PasswordStrength to skip that matcher
func FilterRepeatMatcher(m match.Matcher) bool {
	return m.ID == repeatMatcherName
}

func repeatMatch(password string) []match.Match {
	var matches []match.Match
	slicer := newRuneSlicer(password)
	if slicer.len() == 0 {
		return matches
	}

	//Loop through password. if current == prev currentStreak++ else if currentStreak > 2 {buildMatch; currentStreak = 1} prev = current
	//Positions are rune indices; the token is always the original substring.
	currentStreak := 1
	streakStart := 0
	prev := string(slicer.runes[0])
	for idx := 1; idx < slicer.len(); idx++ {
		current := string(slicer.runes[idx])

		if strings.ToLower(current) == strings.ToLower(prev) {
			currentStreak++
			continue
		}

		if currentStreak > 2 {
			iPos := streakStart
			jPos := idx - 1
			matchRepeat := match.Match{
				Pattern:        "repeat",
				I:              iPos,
				J:              jPos,
				Token:          slicer.slice(password, iPos, jPos),
				DictionaryName: prev}
			matchRepeat.Entropy = entropy.RepeatEntropy(matchRepeat)
			matches = append(matches, matchRepeat)
		}
		currentStreak = 1
		streakStart = idx
		prev = current
	}

	if slicer.len() > 0 && currentStreak > 2 {
		iPos := streakStart
		jPos := slicer.len() - 1
		matchRepeat := match.Match{
			Pattern:        "repeat",
			I:              iPos,
			J:              jPos,
			Token:          slicer.slice(password, iPos, jPos),
			DictionaryName: prev}
		matchRepeat.Entropy = entropy.RepeatEntropy(matchRepeat)
		matches = append(matches, matchRepeat)
	}
	return matches
}
