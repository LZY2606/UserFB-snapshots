package matching

import (
	"strings"

	"github.com/nbutton23/zxcvbn-go/entropy"
	"github.com/nbutton23/zxcvbn-go/match"
)

func buildDictMatcher(dictName string, rankedDict map[string]int) func(password string) []match.Match {
	return func(password string) []match.Match {
		matches := dictionaryMatch(password, dictName, rankedDict)
		for _, v := range matches {
			v.DictionaryName = dictName
		}
		return matches
	}

}

func dictionaryMatch(password string, dictionaryName string, rankedDict map[string]int) []match.Match {
	var results []match.Match
	slicer := newRuneSlicer(password)

	// lowerPart[i] is the lowercase form of original rune i. Lowercasing a
	// single rune may change its byte length (or even expand to several
	// runes), so the lowercase view is built per original rune instead of by
	// lowercasing the whole string: match coordinates stay aligned with the
	// original input one rune at a time, and every emitted token is sliced
	// from the untouched password.
	lowerPart := make([]string, slicer.len())
	for i, r := range slicer.runes {
		lowerPart[i] = strings.ToLower(string(r))
	}

	for i := 0; i < slicer.len(); i++ {
		var word strings.Builder
		for j := i; j < slicer.len(); j++ {
			word.WriteString(lowerPart[j])
			if val, ok := rankedDict[word.String()]; ok {
				matchDic := match.Match{Pattern: "dictionary",
					DictionaryName: dictionaryName,
					I:              i,
					J:              j,
					Token:          slicer.slice(password, i, j),
				}
				matchDic.Entropy = entropy.DictionaryEntropy(matchDic, float64(val))

				results = append(results, matchDic)
			}
		}
	}

	return results
}

func buildRankedDict(unrankedList []string) map[string]int {

	result := make(map[string]int)

	for i, v := range unrankedList {
		result[strings.ToLower(v)] = i + 1
	}

	return result
}
