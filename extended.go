package ssh_config

import (
	"fmt"
	"strings"
)

// criterionKind identifies the keyword of a single extended Match condition.
type criterionKind int

const (
	criterionHost criterionKind = iota
	criterionOriginalHost
	criterionUser
	criterionLocalUser
	criterionCanonical
	criterionFinal
	criterionAll
)

var matchCriterionKinds = map[string]criterionKind{
	"host":         criterionHost,
	"originalhost": criterionOriginalHost,
	"user":         criterionUser,
	"localuser":    criterionLocalUser,
	"canonical":    criterionCanonical,
	"final":        criterionFinal,
	"all":          criterionAll,
}

// matchCriterion is one AND condition of an extended Match directive.
type matchCriterion struct {
	kind     criterionKind
	negated  bool
	patterns []*Pattern
}

// extendedMatchLexer is a small whitespace/argument tokenizer for the text
// after "Match". Offsets are relative to the text passed in, which is the
// value token's contents; the parser adds that token's column to produce an
// absolute position.
type extendedMatchLexer struct {
	text string
	i    int
}

func (l *extendedMatchLexer) eof() bool { return l.i >= len(l.text) }

func (l *extendedMatchLexer) skipSpace() {
	for !l.eof() {
		c := l.text[l.i]
		if c != ' ' && c != '\t' {
			return
		}
		l.i++
	}
}

func (l *extendedMatchLexer) cur() byte { return l.text[l.i] }

// word reads a token up to whitespace, '=' or '#'. It returns the token and the
// offset it started at.
func (l *extendedMatchLexer) word() (string, int) {
	start := l.i
	for !l.eof() {
		c := l.text[l.i]
		if c == ' ' || c == '\t' || c == '=' || c == '#' {
			break
		}
		l.i++
	}
	return l.text[start:l.i], start
}

// commaList reads a comma-separated pattern list, allowing whitespace around
// commas. It stops at whitespace that is not immediately followed by a comma,
// and at a comment.
func (l *extendedMatchLexer) commaList() []string {
	var tokens []string
	var current strings.Builder
	flush := func() {
		if current.Len() > 0 {
			tokens = append(tokens, current.String())
			current.Reset()
		}
	}
	for !l.eof() {
		c := l.text[l.i]
		if c == '#' {
			break
		}
		if c == ',' {
			flush()
			l.i++
			continue
		}
		if c == ' ' || c == '\t' {
			// Whitespace ends the argument unless a comma follows.
			j := l.i
			for j < len(l.text) && (l.text[j] == ' ' || l.text[j] == '\t') {
				j++
			}
			if j < len(l.text) && l.text[j] == ',' {
				l.i = j
				continue
			}
			l.i = j
			break
		}
		current.WriteByte(c)
		l.i++
	}
	flush()
	return tokens
}

// matchCriterionError builds an error positioned at a byte offset into the
// text following "Match". parseExtendedMatch turns the offset into an
// absolute line/column.
func matchCriterionError(start int, format string, args ...interface{}) error {
	msg := fmt.Sprintf(format, args...)
	return &matchSyntaxError{offset: start, msg: msg}
}

func (l *extendedMatchLexer) errorAt(offset int, format string, args ...interface{}) error {
	return matchCriterionError(offset, format, args...)
}

type matchSyntaxError struct {
	offset int
	msg    string
}

func (e *matchSyntaxError) Error() string {
	return "ssh_config: " + e.msg
}

// parseExtendedMatchCriteria parses the text following "Match" using the
// extended condition syntax. Conditions are combined with AND.
/*
	Match host = pattern,otherpattern originalhost = pattern user = name final
	Match all
*/
func parseExtendedMatchCriteria(text string) ([]matchCriterion, error) {
	if strings.TrimSpace(text) == "" {
		return nil, &matchSyntaxError{offset: 0, msg: "Match directive requires at least one criterion"}
	}
	lx := &extendedMatchLexer{text: text}
	var criteria []matchCriterion
	for {
		lx.skipSpace()
		if lx.eof() {
			break
		}
		if lx.cur() == '#' {
			// Attached comment. The raw Match text (including the comment) is
			// retained by the caller for round-tripping; the condition list
			// stops here.
			break
		}

		token, start := lx.word()
		if token == "" {
			return nil, lx.errorAt(start, "missing condition")
		}
		negated := false
		if token[0] == '!' {
			negated = true
			token = token[1:]
			if token == "" {
				return nil, lx.errorAt(start, "missing condition after \"!\"")
			}
		}
		lower := strings.ToLower(token)
		if lower == "exec" {
			return nil, lx.errorAt(start, "Match Exec is not supported")
		}
		kind, ok := matchCriterionKinds[lower]
		if !ok {
			return nil, lx.errorAt(start, "unknown condition %q", token)
		}

		// A single Match directive may not mix "all" with anything other than
		// canonical/final before it, and "all" must be the final condition.
		if kind == criterionAll {
			if negated {
				return nil, lx.errorAt(start, "Match all cannot be negated")
			}
			if len(criteria) > 0 {
				prev := criteria[len(criteria)-1].kind
				if prev != criterionCanonical && prev != criterionFinal {
					return nil, lx.errorAt(start, "Match all may only appear alone or directly after canonical/final")
				}
			}
			lx.skipSpace()
			if !lx.eof() && lx.cur() != '#' {
				return nil, lx.errorAt(lx.i, "Match all must be the final condition")
			}
			criteria = append(criteria, matchCriterion{kind: criterionAll})
			continue
		}

		if isFlagKind(kind) {
			lx.skipSpace()
			if !lx.eof() && lx.cur() == '=' {
				return nil, lx.errorAt(lx.i, "%s does not take an argument", lower)
			}
			criteria = append(criteria, matchCriterion{kind: kind, negated: negated})
			continue
		}

		// canonical and final must be the last conditions on the line.
		if criteriaHaveFlag(criteria) {
			return nil, lx.errorAt(start, "no conditions may follow canonical/final except all")
		}

		// Pattern condition. The argument is a comma-separated list of
		// wildcard patterns. "= a,b" is the canonical spelling; a negative
		// pattern is written "!pattern". For compatibility with the
		// historical Match Host syntax, a whitespace-separated list without
		// "=" is also accepted.
		lx.skipSpace()
		var tokens []string
		if !lx.eof() && lx.cur() == '=' {
			lx.i++
			lx.skipSpace()
			tokens = lx.commaList()
		} else {
			for {
				lx.skipSpace()
				if lx.eof() || lx.cur() == '#' {
					break
				}
				tok, tokStart := lx.word()
				if tok == "" {
					return nil, lx.errorAt(tokStart, "unexpected character %q", string(lx.cur()))
				}
				if len(tokens) > 0 && isMatchCriterionWord(tok) {
					// Do not consume; it starts the next condition.
					lx.i = tokStart
					break
				}
				tokens = append(tokens, tok)
			}
		}
		if len(tokens) == 0 {
			return nil, lx.errorAt(start, "%s requires at least one pattern", lower)
		}
		var patterns []*Pattern
		for _, tok := range tokens {
			for _, piece := range strings.Split(tok, ",") {
				piece = strings.TrimSpace(piece)
				if piece == "" {
					continue
				}
				pat, err := NewPattern(piece)
				if err != nil {
					return nil, lx.errorAt(start, "invalid %s pattern %q: %v", lower, piece, err)
				}
				patterns = append(patterns, pat)
			}
		}
		if len(patterns) == 0 {
			return nil, lx.errorAt(start, "%s requires at least one pattern", lower)
		}
		criteria = append(criteria, matchCriterion{kind: kind, negated: negated, patterns: patterns})
	}
	if len(criteria) == 0 {
		return nil, &matchSyntaxError{offset: 0, msg: "Match directive requires at least one criterion"}
	}
	return criteria, nil
}

// qualifyExtendedError positions error text relative to the Match directive
// whose raw text was parsed. Errors reported by NewInclude are already
// positioned; Match syntax errors carry a byte offset into the directive text.
func (p *sshParser) qualifyExtendedError(val *token, err error) (Position, string) {
	msg := err.Error()
	if strings.HasPrefix(msg, "ssh_config: ") {
		msg = strings.TrimPrefix(msg, "ssh_config: ")
	}
	if syn, ok := err.(*matchSyntaxError); ok {
		col := val.Position.Col + syn.offset
		return Position{Line: val.Position.Line, Col: col}, msg
	}
	return val.Position, msg
}
func isFlagKind(kind criterionKind) bool {
	switch kind {
	case criterionCanonical, criterionFinal, criterionAll:
		return true
	}
	return false
}

func criteriaHaveFlag(criteria []matchCriterion) bool {
	for _, c := range criteria {
		if isFlagKind(c.kind) {
			return true
		}
	}
	return false
}

// isMatchCriterionWord reports whether token (possibly negated) is a condition
// keyword and thus terminates the preceding pattern list.
func isMatchCriterionWord(token string) bool {
	token = strings.TrimPrefix(token, "!")
	_, ok := matchCriterionKinds[strings.ToLower(token)]
	return ok
}

// parseExtendedMatch is the extended-syntax counterpart of parseMatch.
func (p *sshParser) parseExtendedMatch(val *token, hasEquals bool, comment string) sshParserStateFn {
	criteria, err := parseExtendedMatchCriteria(val.val)
	if err != nil {
		pos, msg := p.qualifyExtendedError(val, err)
		p.raiseErrorf(&token{Position: pos}, msg)
		return nil
	}
	p.config.Hosts = append(p.config.Hosts, &Host{
		Patterns:      []*Pattern{matchAll},
		Nodes:         make([]Node, 0),
		isMatch:       true,
		extendedMatch: true,
		// matchText keeps the original text after "Match" (comments included)
		// so String round-trips the directive exactly. Comments on extended
		// Match lines are folded into this text, so EOLComment stays empty.
		matchText:     val.val,
		matchCriteria: criteria,
		hasEquals:     hasEquals,
	})
	return p.parseStart
}

// matchPatternList applies the negated-pattern list rules: a negated pattern
// that matches rejects the whole list, and a list consisting only of negated
// patterns never produces a positive match on its own.
func matchPatternList(patterns []*Pattern, subject string) bool {
	positive := false
	for _, p := range patterns {
		if !p.regex.MatchString(subject) {
			continue
		}
		if p.not {
			return false
		}
		positive = true
	}
	return positive
}

// matchCriteria evaluates the AND-combined conditions of an extended Match
// directive. host is the host name used for the "host" condition (it is the
// canonical name during the final round); canonical reports whether a
// canonical host name is available; final reports whether the final round is
// active.
func matchCriteria(criteria []matchCriterion, rctx ResolveContext, host string, round resolveRound) bool {
	for _, c := range criteria {
		result := false
		switch c.kind {
		case criterionHost:
			result = matchPatternList(c.patterns, host)
		case criterionOriginalHost:
			result = matchPatternList(c.patterns, rctx.OriginalHost)
		case criterionUser:
			result = matchPatternList(c.patterns, rctx.RemoteUser)
		case criterionLocalUser:
			result = matchPatternList(c.patterns, rctx.LocalUser)
		case criterionCanonical:
			result = round.canonical
		case criterionFinal:
			result = round.final
		case criterionAll:
			result = true
		}
		if c.negated {
			result = !result
		}
		if !result {
			return false
		}
	}
	return true
}

// hasFinalCriterion reports whether any Match block in the tree, including
// included files, uses the final condition. A final condition requests the
// final resolution round even when no canonical host name was supplied.
func (c *Config) hasFinalCriterion() bool {
	if c == nil {
		return false
	}
	for _, h := range c.Hosts {
		for _, crit := range h.matchCriteria {
			if crit.kind == criterionFinal {
				return true
			}
		}
		for _, node := range h.Nodes {
			if inc, ok := node.(*Include); ok && inc.hasFinalCriterion() {
				return true
			}
		}
	}
	return false
}

func (inc *Include) hasFinalCriterion() bool {
	inc.mu.Lock()
	defer inc.mu.Unlock()
	for i := range inc.matches {
		if inc.files[inc.matches[i]].hasFinalCriterion() {
			return true
		}
	}
	return false
}
