package graphql

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"
)

// A hand-written recursive descent parser for the subset of GraphQL used by
// Collection: a single query operation (shorthand or named, with variable
// definitions) plus named fragments. It never uses regular expressions to
// skip over the grammar; every token is consumed explicitly.

type tokenKind int

const (
	tokEOF tokenKind = iota
	tokName
	tokInt
	tokString
	tokPunct
)

type token struct {
	kind tokenKind
	val  string
	pos  int
}

type lexer struct {
	src string
	i   int
}

func isIgnored(r rune) bool {
	switch r {
	case ' ', '\t', '\n', '\r', ',', 0xFEFF:
		return true
	}
	return false
}

func isNameStart(r rune) bool {
	return r == '_' || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z')
}

func isNameContinue(r rune) bool {
	return isNameStart(r) || (r >= '0' && r <= '9')
}

func (l *lexer) next() (token, error) {
	for l.i < len(l.src) {
		r, size := utf8.DecodeRuneInString(l.src[l.i:])
		switch {
		case r == '#':
			l.i += size
			for l.i < len(l.src) {
				r2, s2 := utf8.DecodeRuneInString(l.src[l.i:])
				l.i += s2
				if r2 == '\n' || r2 == '\r' {
					break
				}
			}
		case isIgnored(r):
			l.i += size
		default:
			return l.readToken()
		}
	}
	return token{kind: tokEOF, pos: l.i}, nil
}

func (l *lexer) readToken() (token, error) {
	start := l.i
	r, size := utf8.DecodeRuneInString(l.src[l.i:])
	switch {
	case isNameStart(r):
		l.i += size
		for l.i < len(l.src) {
			r2, s2 := utf8.DecodeRuneInString(l.src[l.i:])
			if !isNameContinue(r2) {
				break
			}
			l.i += s2
		}
		return token{kind: tokName, val: l.src[start:l.i], pos: start}, nil
	case r == '"':
		return l.readString()
	case r == '-' || (r >= '0' && r <= '9'):
		return l.readNumber()
	case r == '.':
		if strings.HasPrefix(l.src[l.i:], "...") {
			l.i += 3
			return token{kind: tokPunct, val: "...", pos: start}, nil
		}
		return token{}, fmt.Errorf("graphql: syntax error at offset %d: unexpected '.'", start)
	case strings.ContainsRune("!$():=@[]{}|", r):
		l.i += size
		return token{kind: tokPunct, val: string(r), pos: start}, nil
	default:
		return token{}, fmt.Errorf("graphql: syntax error at offset %d: unexpected character %q", start, r)
	}
}

func (l *lexer) readNumber() (token, error) {
	start := l.i
	if l.src[l.i] == '-' {
		l.i++
		if l.i >= len(l.src) || l.src[l.i] < '0' || l.src[l.i] > '9' {
			return token{}, fmt.Errorf("graphql: syntax error at offset %d: malformed integer", start)
		}
	}
	if l.src[l.i] == '0' {
		l.i++
	} else {
		for l.i < len(l.src) && l.src[l.i] >= '0' && l.src[l.i] <= '9' {
			l.i++
		}
	}
	if l.i < len(l.src) {
		switch l.src[l.i] {
		case '.', 'e', 'E':
			return token{}, fmt.Errorf("graphql: syntax error at offset %d: float arguments are not supported", start)
		}
	}
	return token{kind: tokInt, val: l.src[start:l.i], pos: start}, nil
}

func (l *lexer) readString() (token, error) {
	start := l.i
	if strings.HasPrefix(l.src[l.i:], `"""`) {
		return token{}, fmt.Errorf("graphql: syntax error at offset %d: block strings are not supported", start)
	}
	l.i++ // opening quote
	var b strings.Builder
	for {
		if l.i >= len(l.src) {
			return token{}, fmt.Errorf("graphql: syntax error at offset %d: unterminated string", start)
		}
		r, size := utf8.DecodeRuneInString(l.src[l.i:])
		if r == '\n' || r == '\r' {
			return token{}, fmt.Errorf("graphql: syntax error at offset %d: unterminated string", start)
		}
		if r == '"' {
			l.i += size
			return token{kind: tokString, val: b.String(), pos: start}, nil
		}
		if r == '\\' {
			l.i += size
			if l.i >= len(l.src) {
				return token{}, fmt.Errorf("graphql: syntax error at offset %d: unterminated escape", start)
			}
			esc := l.src[l.i]
			l.i++
			switch esc {
			case '"', '\\', '/':
				b.WriteByte(esc)
			case 'b':
				b.WriteByte('\b')
			case 'f':
				b.WriteByte('\f')
			case 'n':
				b.WriteByte('\n')
			case 'r':
				b.WriteByte('\r')
			case 't':
				b.WriteByte('\t')
			case 'u':
				code, err := l.readUnicodeEscape()
				if err != nil {
					return token{}, err
				}
				if code >= 0xD800 && code <= 0xDBFF {
					// high surrogate; require a following low surrogate
					if l.i+2 <= len(l.src) && l.src[l.i] == '\\' && l.src[l.i+1] == 'u' {
						saved := l.i
						l.i += 2
						low, err := l.readUnicodeEscape()
						if err == nil && low >= 0xDC00 && low <= 0xDFFF {
							code = 0x10000 + (code-0xD800)<<10 + (low - 0xDC00)
						} else {
							l.i = saved
						}
					}
				}
				b.WriteRune(code)
			default:
				return token{}, fmt.Errorf("graphql: syntax error at offset %d: invalid escape sequence", start)
			}
			continue
		}
		l.i += size
		b.WriteRune(r)
	}
}

func (l *lexer) readUnicodeEscape() (rune, error) {
	if l.i+4 > len(l.src) {
		return 0, fmt.Errorf("graphql: syntax error at offset %d: truncated unicode escape", l.i)
	}
	hex := l.src[l.i : l.i+4]
	n, err := strconv.ParseUint(hex, 16, 32)
	if err != nil {
		return 0, fmt.Errorf("graphql: syntax error at offset %d: invalid unicode escape", l.i)
	}
	l.i += 4
	return rune(n), nil
}

// AST node types.

type valKind int

const (
	valVar valKind = iota
	valInt
	valString
	valBool
	valNull
	valEnum
	valList
	valObject
)

type gqlValue struct {
	kind valKind
	s    string
	i    int64
	b    bool
	list []*gqlValue
	obj  []arg
}

type arg struct {
	name string
	val  *gqlValue
}

type gqlType struct {
	name    string // empty for list types
	elem    *gqlType
	nonNull bool
}

type directive struct {
	name string
	args []arg
}

type selection interface {
	selectionNode()
}

type field struct {
	alias string
	name  string
	args  []arg
	dirs  []*directive
	sel   []selection
}

func (*field) selectionNode() {}

type fragmentSpread struct {
	name string
	dirs []*directive
}

func (*fragmentSpread) selectionNode() {}

type inlineFragment struct {
	typeCond string // empty when there is no type condition
	dirs     []*directive
	sel      []selection
}

func (*inlineFragment) selectionNode() {}

type varDef struct {
	name string
	typ  *gqlType
	def  *gqlValue // optional default, always a literal
}

type operationDef struct {
	name string
	vars []*varDef
	dirs []*directive
	sel  []selection
}

type fragmentDef struct {
	name     string
	typeCond string
	dirs     []*directive
	sel      []selection
}

type document struct {
	op    *operationDef
	frags map[string]*fragmentDef
}

type parser struct {
	lex *lexer
	cur token
}

func parseDocument(src string) (*document, error) {
	p := &parser{lex: &lexer{src: src}}
	if err := p.advance(); err != nil {
		return nil, err
	}
	doc := &document{frags: map[string]*fragmentDef{}}
	for p.cur.kind != tokEOF {
		switch {
		case p.cur.kind == tokPunct && p.cur.val == "{":
			if doc.op != nil {
				return nil, fmt.Errorf("graphql: only a single query operation is supported")
			}
			op, err := p.parseOperation("", true)
			if err != nil {
				return nil, err
			}
			doc.op = op
		case p.cur.kind == tokName && (p.cur.val == "query" || p.cur.val == "mutation" || p.cur.val == "subscription"):
			kind := p.cur.val
			if kind != "query" {
				return nil, fmt.Errorf("graphql: %s operations are not supported", kind)
			}
			if doc.op != nil {
				return nil, fmt.Errorf("graphql: only a single query operation is supported")
			}
			if err := p.advance(); err != nil {
				return nil, err
			}
			op, err := p.parseOperation("", false)
			if err != nil {
				return nil, err
			}
			doc.op = op
		case p.cur.kind == tokName && p.cur.val == "fragment":
			f, err := p.parseFragment()
			if err != nil {
				return nil, err
			}
			if _, exists := doc.frags[f.name]; exists {
				return nil, fmt.Errorf("graphql: fragment %q is defined more than once", f.name)
			}
			doc.frags[f.name] = f
		default:
			return nil, fmt.Errorf("graphql: syntax error at offset %d: expected a query or fragment definition", p.cur.pos)
		}
	}
	if doc.op == nil {
		return nil, fmt.Errorf("graphql: document contains no query operation")
	}
	if err := validateDocument(doc); err != nil {
		return nil, err
	}
	return doc, nil
}

func (p *parser) advance() error {
	t, err := p.lex.next()
	if err != nil {
		return err
	}
	p.cur = t
	return nil
}

func (p *parser) expectPunct(val string) error {
	if p.cur.kind != tokPunct || p.cur.val != val {
		return fmt.Errorf("graphql: syntax error at offset %d: expected %q", p.cur.pos, val)
	}
	return p.advance()
}

func (p *parser) expectName() (string, error) {
	if p.cur.kind != tokName {
		return "", fmt.Errorf("graphql: syntax error at offset %d: expected a name", p.cur.pos)
	}
	val := p.cur.val
	if err := p.advance(); err != nil {
		return "", err
	}
	return val, nil
}

func (p *parser) parseOperation(name string, shorthand bool) (*operationDef, error) {
	op := &operationDef{name: name}
	if !shorthand {
		if p.cur.kind == tokName {
			op.name = p.cur.val
			if err := p.advance(); err != nil {
				return nil, err
			}
		}
		if p.cur.kind == tokPunct && p.cur.val == "(" {
			vars, err := p.parseVarDefs()
			if err != nil {
				return nil, err
			}
			op.vars = vars
		}
		// Operation directives are validated against the supported set when
		// the request is compiled.
		dirs, err := p.parseDirectives()
		if err != nil {
			return nil, err
		}
		op.dirs = dirs
	}
	sel, err := p.parseSelectionSet()
	if err != nil {
		return nil, err
	}
	op.sel = sel
	return op, nil
}

func (p *parser) parseVarDefs() ([]*varDef, error) {
	if err := p.expectPunct("("); err != nil {
		return nil, err
	}
	var defs []*varDef
	for !(p.cur.kind == tokPunct && p.cur.val == ")") {
		if p.cur.kind != tokPunct || p.cur.val != "$" {
			return nil, fmt.Errorf("graphql: syntax error at offset %d: expected a variable definition", p.cur.pos)
		}
		if err := p.advance(); err != nil {
			return nil, err
		}
		name, err := p.expectName()
		if err != nil {
			return nil, err
		}
		if err := p.expectPunct(":"); err != nil {
			return nil, err
		}
		typ, err := p.parseType()
		if err != nil {
			return nil, err
		}
		var def *gqlValue
		if p.cur.kind == tokPunct && p.cur.val == "=" {
			if err := p.advance(); err != nil {
				return nil, err
			}
			def, err = p.parseValue(true)
			if err != nil {
				return nil, err
			}
		}
		defs = append(defs, &varDef{name: name, typ: typ, def: def})
	}
	if err := p.expectPunct(")"); err != nil {
		return nil, err
	}
	return defs, nil
}

func (p *parser) parseType() (*gqlType, error) {
	var t *gqlType
	if p.cur.kind == tokPunct && p.cur.val == "[" {
		if err := p.advance(); err != nil {
			return nil, err
		}
		elem, err := p.parseType()
		if err != nil {
			return nil, err
		}
		if err := p.expectPunct("]"); err != nil {
			return nil, err
		}
		t = &gqlType{elem: elem}
	} else {
		name, err := p.expectName()
		if err != nil {
			return nil, err
		}
		t = &gqlType{name: name}
	}
	if p.cur.kind == tokPunct && p.cur.val == "!" {
		t.nonNull = true
		if err := p.advance(); err != nil {
			return nil, err
		}
	}
	return t, nil
}

func (p *parser) parseFragment() (*fragmentDef, error) {
	if err := p.advance(); // consume "fragment"
	err != nil {
		return nil, err
	}
	name, err := p.expectName()
	if err != nil {
		return nil, err
	}
	if name == "on" {
		return nil, fmt.Errorf("graphql: syntax error: fragment cannot be named %q", name)
	}
	on, err := p.expectName()
	if err != nil {
		return nil, err
	}
	if on != "on" {
		return nil, fmt.Errorf("graphql: syntax error: expected %q in fragment definition", "on")
	}
	typeCond, err := p.expectName()
	if err != nil {
		return nil, err
	}
	dirs, err := p.parseDirectives()
	if err != nil {
		return nil, err
	}
	sel, err := p.parseSelectionSet()
	if err != nil {
		return nil, err
	}
	return &fragmentDef{name: name, typeCond: typeCond, dirs: dirs, sel: sel}, nil
}

func (p *parser) parseSelectionSet() ([]selection, error) {
	if err := p.expectPunct("{"); err != nil {
		return nil, err
	}
	var sels []selection
	for !(p.cur.kind == tokPunct && p.cur.val == "}") {
		if p.cur.kind == tokEOF {
			return nil, fmt.Errorf("graphql: syntax error: unterminated selection set")
		}
		if p.cur.kind == tokPunct && p.cur.val == "..." {
			s, err := p.parseSpread()
			if err != nil {
				return nil, err
			}
			sels = append(sels, s)
			continue
		}
		f, err := p.parseField()
		if err != nil {
			return nil, err
		}
		sels = append(sels, f)
	}
	if err := p.expectPunct("}"); err != nil {
		return nil, err
	}
	return sels, nil
}

func (p *parser) parseSpread() (selection, error) {
	if err := p.advance(); // consume "..."
	err != nil {
		return nil, err
	}
	if p.cur.kind == tokName && p.cur.val == "on" {
		if err := p.advance(); err != nil {
			return nil, err
		}
		typeCond, err := p.expectName()
		if err != nil {
			return nil, err
		}
		dirs, err := p.parseDirectives()
		if err != nil {
			return nil, err
		}
		sel, err := p.parseSelectionSet()
		if err != nil {
			return nil, err
		}
		return &inlineFragment{typeCond: typeCond, dirs: dirs, sel: sel}, nil
	}
	if p.cur.kind == tokName {
		name, err := p.expectName()
		if err != nil {
			return nil, err
		}
		dirs, err := p.parseDirectives()
		if err != nil {
			return nil, err
		}
		return &fragmentSpread{name: name, dirs: dirs}, nil
	}
	// Inline fragment without a type condition.
	dirs, err := p.parseDirectives()
	if err != nil {
		return nil, err
	}
	if !(p.cur.kind == tokPunct && p.cur.val == "{") {
		return nil, fmt.Errorf("graphql: syntax error at offset %d: inline fragment needs a selection set", p.cur.pos)
	}
	sel, err := p.parseSelectionSet()
	if err != nil {
		return nil, err
	}
	return &inlineFragment{dirs: dirs, sel: sel}, nil
}

func (p *parser) parseField() (*field, error) {
	first, err := p.expectName()
	if err != nil {
		return nil, err
	}
	f := &field{name: first, alias: first}
	if p.cur.kind == tokPunct && p.cur.val == ":" {
		if err := p.advance(); err != nil {
			return nil, err
		}
		f.name, err = p.expectName()
		if err != nil {
			return nil, err
		}
	}
	if p.cur.kind == tokPunct && p.cur.val == "(" {
		args, err := p.parseArgs()
		if err != nil {
			return nil, err
		}
		f.args = args
	}
	dirs, err := p.parseDirectives()
	if err != nil {
		return nil, err
	}
	f.dirs = dirs
	if p.cur.kind == tokPunct && p.cur.val == "{" {
		sel, err := p.parseSelectionSet()
		if err != nil {
			return nil, err
		}
		f.sel = sel
	}
	return f, nil
}

func (p *parser) parseArgs() ([]arg, error) {
	if err := p.expectPunct("("); err != nil {
		return nil, err
	}
	var args []arg
	seen := map[string]bool{}
	for !(p.cur.kind == tokPunct && p.cur.val == ")") {
		name, err := p.expectName()
		if err != nil {
			return nil, err
		}
		if seen[name] {
			return nil, fmt.Errorf("graphql: argument %q is provided more than once", name)
		}
		seen[name] = true
		if err := p.expectPunct(":"); err != nil {
			return nil, err
		}
		v, err := p.parseValue(false)
		if err != nil {
			return nil, err
		}
		args = append(args, arg{name: name, val: v})
	}
	if err := p.expectPunct(")"); err != nil {
		return nil, err
	}
	return args, nil
}

func (p *parser) parseDirectives() ([]*directive, error) {
	var dirs []*directive
	seen := map[string]bool{}
	for p.cur.kind == tokPunct && p.cur.val == "@" {
		if err := p.advance(); err != nil {
			return nil, err
		}
		name, err := p.expectName()
		if err != nil {
			return nil, err
		}
		if seen[name] {
			return nil, fmt.Errorf("graphql: directive @%s is repeated", name)
		}
		seen[name] = true
		d := &directive{name: name}
		if p.cur.kind == tokPunct && p.cur.val == "(" {
			args, err := p.parseArgs()
			if err != nil {
				return nil, err
			}
			d.args = args
		}
		dirs = append(dirs, d)
	}
	return dirs, nil
}

func (p *parser) parseValue(literal bool) (*gqlValue, error) {
	switch p.cur.kind {
	case tokInt:
		n, err := strconv.ParseInt(p.cur.val, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("graphql: integer %q is out of range", p.cur.val)
		}
		if err := p.advance(); err != nil {
			return nil, err
		}
		return &gqlValue{kind: valInt, i: n}, nil
	case tokString:
		s := p.cur.val
		if err := p.advance(); err != nil {
			return nil, err
		}
		return &gqlValue{kind: valString, s: s}, nil
	case tokPunct:
		switch p.cur.val {
		case "$":
			if literal {
				return nil, fmt.Errorf("graphql: syntax error at offset %d: default values cannot reference variables", p.cur.pos)
			}
			if err := p.advance(); err != nil {
				return nil, err
			}
			name, err := p.expectName()
			if err != nil {
				return nil, err
			}
			return &gqlValue{kind: valVar, s: name}, nil
		case "[":
			if err := p.advance(); err != nil {
				return nil, err
			}
			v := &gqlValue{kind: valList}
			for !(p.cur.kind == tokPunct && p.cur.val == "]") {
				if p.cur.kind == tokEOF {
					return nil, fmt.Errorf("graphql: syntax error: unterminated list value")
				}
				elem, err := p.parseValue(literal)
				if err != nil {
					return nil, err
				}
				v.list = append(v.list, elem)
			}
			if err := p.expectPunct("]"); err != nil {
				return nil, err
			}
			return v, nil
		case "{":
			if err := p.advance(); err != nil {
				return nil, err
			}
			v := &gqlValue{kind: valObject}
			seen := map[string]bool{}
			for !(p.cur.kind == tokPunct && p.cur.val == "}") {
				if p.cur.kind == tokEOF {
					return nil, fmt.Errorf("graphql: syntax error: unterminated object value")
				}
				name, err := p.expectName()
				if err != nil {
					return nil, err
				}
				if seen[name] {
					return nil, fmt.Errorf("graphql: input object field %q is repeated", name)
				}
				seen[name] = true
				if err := p.expectPunct(":"); err != nil {
					return nil, err
				}
				elem, err := p.parseValue(literal)
				if err != nil {
					return nil, err
				}
				v.obj = append(v.obj, arg{name: name, val: elem})
			}
			if err := p.expectPunct("}"); err != nil {
				return nil, err
			}
			return v, nil
		}
	case tokName:
		val := p.cur.val
		if err := p.advance(); err != nil {
			return nil, err
		}
		switch val {
		case "true":
			return &gqlValue{kind: valBool, b: true}, nil
		case "false":
			return &gqlValue{kind: valBool, b: false}, nil
		case "null":
			return &gqlValue{kind: valNull}, nil
		default:
			return &gqlValue{kind: valEnum, s: val}, nil
		}
	}
	return nil, fmt.Errorf("graphql: syntax error at offset %d: expected an argument value", p.cur.pos)
}

// validateDocument checks fragment references, fragment cycles and variable
// references without consulting a schema.
func validateDocument(doc *document) error {
	for name, frag := range doc.frags {
		visited := map[string]bool{}
		if err := checkFragment(doc, name, frag, visited); err != nil {
			return err
		}
	}
	// Fragment spreads used directly by the operation need the same checks.
	if err := checkSpreads(doc, doc.op.sel, map[string]bool{}); err != nil {
		return err
	}
	declared := map[string]bool{}
	for _, vd := range doc.op.vars {
		if declared[vd.name] {
			return fmt.Errorf("graphql: variable $%s is declared more than once", vd.name)
		}
		declared[vd.name] = true
	}
	refs := map[string]bool{}
	collectVarRefs(doc.op.sel, refs)
	for _, frag := range doc.frags {
		collectVarRefs(frag.sel, refs)
		collectDirVarRefs(frag.dirs, refs)
	}
	for ref := range refs {
		if !declared[ref] {
			return fmt.Errorf("graphql: variable $%s is used but not declared", ref)
		}
	}
	return nil
}

func checkFragment(doc *document, start string, frag *fragmentDef, visited map[string]bool) error {
	if visited[frag.name] {
		return fmt.Errorf("graphql: fragment %q is part of a fragment cycle", start)
	}
	visited[frag.name] = true
	if err := checkSpreads(doc, frag.sel, visited); err != nil {
		return err
	}
	delete(visited, frag.name)
	return nil
}

// checkSpreads walks a selection set, resolving every named fragment spread
// against the document so undefined or cyclic fragments fail at parse time.
func checkSpreads(doc *document, sels []selection, visited map[string]bool) error {
	for _, s := range sels {
		switch n := s.(type) {
		case *field:
			if n.sel != nil {
				if err := checkSpreads(doc, n.sel, visited); err != nil {
					return err
				}
			}
		case *fragmentSpread:
			next, ok := doc.frags[n.name]
			if !ok {
				return fmt.Errorf("graphql: fragment %q is not defined", n.name)
			}
			if err := checkFragment(doc, n.name, next, visited); err != nil {
				return err
			}
		case *inlineFragment:
			if err := checkSpreads(doc, n.sel, visited); err != nil {
				return err
			}
		}
	}
	return nil
}

func collectVarRefs(sels []selection, out map[string]bool) {
	for _, s := range sels {
		switch n := s.(type) {
		case *field:
			for _, a := range n.args {
				collectValueVarRefs(a.val, out)
			}
			collectDirVarRefs(n.dirs, out)
			if n.sel != nil {
				collectVarRefs(n.sel, out)
			}
		case *fragmentSpread:
			collectDirVarRefs(n.dirs, out)
		case *inlineFragment:
			collectDirVarRefs(n.dirs, out)
			collectVarRefs(n.sel, out)
		}
	}
}

func collectDirVarRefs(dirs []*directive, out map[string]bool) {
	for _, d := range dirs {
		for _, a := range d.args {
			collectValueVarRefs(a.val, out)
		}
	}
}

func collectValueVarRefs(v *gqlValue, out map[string]bool) {
	if v == nil {
		return
	}
	switch v.kind {
	case valVar:
		out[v.s] = true
	case valList:
		for _, elem := range v.list {
			collectValueVarRefs(elem, out)
		}
	case valObject:
		for _, a := range v.obj {
			collectValueVarRefs(a.val, out)
		}
	}
}
