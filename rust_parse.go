// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package demangle

import (
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

// rustBasicTypes maps the basic-type letters to their spelling.
var rustBasicTypes = map[byte]string{
	'a': "i8",
	'b': "bool",
	'c': "char",
	'd': "f64",
	'e': "str",
	'f': "f32",
	'h': "u8",
	'i': "isize",
	'j': "usize",
	'l': "i32",
	'm': "u32",
	'n': "i128",
	'o': "u128",
	'p': "_",
	's': "i16",
	't': "u16",
	'u': "()",
	'v': "...",
	'x': "i64",
	'y': "u64",
	'z': "!",
}

// formatInt/formatUint/formatHex render numbers for node Values.
func formatInt(v int64) string   { return strconv.FormatInt(v, 10) }
func formatUint(v uint64) string { return strconv.FormatUint(v, 10) }
func formatHex(v uint64) string  { return strconv.FormatUint(v, 16) }

// rustParser consumes a Rust v0 mangled name and builds a shared
// syntax graph of RustNode values.
//
// The parser performs no printing and holds no option state: every
// production, including hidden generic arguments and the instantiating
// crate, is always parsed and checked. A single parser instance is used
// per call, so no state can leak between calls.
type rustParser struct {
	source string // complete input, including the "_R" prefix
	str    string // unparsed suffix of source
	off    int    // offset of str within source

	// nodes maps a node's start offset to the node. Back-reference
	// targets are resolved through this map; offsets that fall inside
	// an identifier or any other non-node position are absent.
	nodes map[int]*RustNode

	// lifetimes is the number of bound lifetimes currently in scope.
	// It mirrors the nesting of F and D binders and is used to reject
	// lifetime indices that no binder introduced.
	lifetimes int64
}

// parseRustAST parses a string known to start with "_R".
func parseRustAST(name string) (ret *RustAST, err error) {
	// Parsing routines panic with demangleErr on malformed input.
	defer func() {
		if r := recover(); r != nil {
			if de, ok := r.(demangleErr); ok {
				ret = nil
				err = de
				return
			}
			panic(r)
		}
	}()

	suffix := ""
	if dot := strings.Index(name, "."); dot >= 0 {
		suffix = name[dot:]
		name = name[:dot]
	}

	p := &rustParser{
		source: name,
		str:    name[2:],
		off:    0,
		nodes:  make(map[int]*RustNode),
	}

	ast := &RustAST{Suffix: suffix}

	if len(p.str) < 1 {
		p.fail("expected symbol-name")
	}
	if isDigit(p.str[0]) {
		p.fail("unsupported Rust encoding version")
	}

	ast.Root = p.parsePath()

	// The optional instantiating crate is a full path: it must be
	// parsed and validated even though it is never printed.
	if len(p.str) > 0 {
		ast.InstantiatingCrate = p.parsePath()
	}

	if len(p.str) > 0 {
		p.fail("unparsed characters at end of mangled name")
	}

	return ast, nil
}

// fail panics with a demangleErr positioned at the current offset in
// the complete input string.
func (p *rustParser) fail(msg string) {
	panic(demangleErr{err: msg, off: p.off + 2})
}

// failAt panics with a demangleErr at an explicit stripped offset.
func (p *rustParser) failAt(off int, msg string) {
	panic(demangleErr{err: msg, off: off + 2})
}

// advance consumes add bytes, failing on truncated input.
func (p *rustParser) advance(add int) {
	if len(p.str) < add {
		p.fail("unexpected end of mangled name")
	}
	p.str = p.str[add:]
	p.off += add
}

// checkChar requires the next byte to be c and consumes it.
func (p *rustParser) checkChar(c byte) {
	if len(p.str) == 0 || p.str[0] != c {
		p.fail("expected " + string(c))
	}
	p.advance(1)
}

// newNode creates a node of kind spanning [start, current offset) in
// the stripped encoding. Span is reported against the complete input,
// hence the two-byte "_R" adjustment.
func (p *rustParser) newNode(kind string, start int) *RustNode {
	return &RustNode{Kind: kind, Span: [2]int{start + 2, p.off + 2}}
}

// register records n at its stripped start offset so back-references
// can find it. Only nodes at production start offsets are registered,
// which guarantees that an offset inside an identifier or a number
// encoding never resolves.
func (p *rustParser) register(n *RustNode) *RustNode {
	p.nodes[n.Span[0]-2] = n
	return n
}

// finish sets n's end to the current offset and returns n. Nodes whose
// children are parsed after the node is created must be finished to
// span their whole encoding.
func (p *rustParser) finish(n *RustNode) *RustNode {
	n.Span[1] = p.off + 2
	return n
}

// parsePath parses a <path> production.
func (p *rustParser) parsePath() *RustNode {
	start := p.off
	if len(p.str) < 1 {
		p.fail("expected path")
	}
	switch c := p.str[0]; c {
	case 'C':
		p.advance(1)
		n := p.newNode(RustNodePath, start)
		n.Value = "crate"
		n.Children = []*RustNode{p.parseIdentifier()}
		return p.register(p.finish(n))
	case 'M', 'X':
		p.advance(1)
		n := p.newNode(RustNodePath, start)
		if c == 'M' {
			n.Value = "inherent-impl"
		} else {
			n.Value = "trait-impl"
		}
		n.Children = append(n.Children, p.parseImplPath())
		n.Children = append(n.Children, p.parseType())
		if c == 'X' {
			n.Children = append(n.Children, p.parsePath())
		}
		return p.register(p.finish(n))
	case 'Y':
		p.advance(1)
		n := p.newNode(RustNodePath, start)
		n.Value = "trait-definition"
		n.Children = append(n.Children, p.parseType())
		n.Children = append(n.Children, p.parsePath())
		return p.register(p.finish(n))
	case 'N':
		p.advance(1)
		if len(p.str) < 1 {
			p.fail("expected namespace")
		}
		ns := p.str[0]
		if !((ns >= 'a' && ns <= 'z') || (ns >= 'A' && ns <= 'Z')) {
			p.fail("invalid namespace character")
		}
		nsNode := p.newNode(RustNodeNamespace, p.off)
		nsNode.Value = string(ns)
		p.advance(1)
		p.finish(nsNode)

		n := p.newNode(RustNodePath, start)
		n.Children = []*RustNode{nsNode, p.parsePath(), p.parseIdentifier()}
		return p.register(p.finish(n))
	case 'I':
		p.advance(1)
		n := p.newNode(RustNodePath, start)
		n.Children = []*RustNode{p.parsePath(), p.parseGenericArgs()}
		p.checkChar('E')
		return p.register(p.finish(n))
	case 'B':
		return p.parseBackref(RustNodePath)
	default:
		p.fail("unrecognized letter in path")
		return nil
	}
}

// parseImplPath parses [<disambiguator>] <path>. The subtree is kept
// in the graph but is hidden from the printed name.
func (p *rustParser) parseImplPath() *RustNode {
	start := p.off
	n := p.newNode(RustNodeImplPath, start)
	if d, ok := p.tryDisambiguator(); ok {
		n.Children = append(n.Children, d)
	}
	n.Children = append(n.Children, p.parsePath())
	return p.finish(n)
}

// parseIdentifier parses [<disambiguator>] <undisambiguated-identifier>.
func (p *rustParser) parseIdentifier() *RustNode {
	start := p.off
	n := p.newNode(RustNodeIdentifier, start)
	if d, ok := p.tryDisambiguator(); ok {
		n.Children = []*RustNode{d}
	}
	n.Value = p.parseUndisambiguatedIdentifier()
	return p.finish(n)
}

// tryDisambiguator parses an optional "s" <base-62-number>.
func (p *rustParser) tryDisambiguator() (*RustNode, bool) {
	if len(p.str) == 0 || p.str[0] != 's' {
		return nil, false
	}
	start := p.off
	p.advance(1)
	val := p.base62Number() + 1
	n := p.newNode(RustNodeDisambiguator, start)
	n.Value = formatInt(val)
	return p.finish(n), true
}

// parseUndisambiguatedIdentifier parses ["u"] <decimal-number> ["_"]
// <bytes> and returns the decoded identifier.
func (p *rustParser) parseUndisambiguatedIdentifier() string {
	isPunycode := false
	if len(p.str) > 0 && p.str[0] == 'u' {
		p.advance(1)
		isPunycode = true
	}

	val := p.decimalNumber()

	if len(p.str) > 0 && p.str[0] == '_' {
		p.advance(1)
	}

	if len(p.str) < val {
		p.fail("not enough characters for identifier")
	}
	id := p.str[:val]
	p.advance(val)

	for i := 0; i < len(id); i++ {
		c := id[i]
		switch {
		case c >= '0' && c <= '9':
		case c >= 'A' && c <= 'Z':
		case c >= 'a' && c <= 'z':
		case c == '_':
		default:
			p.fail("invalid character in identifier")
		}
	}

	if isPunycode {
		id = p.expandPunycode(id)
	}
	return id
}

// expandPunycode decodes the Rust variant of punycode (RFC 3492 6.2).
func (p *rustParser) expandPunycode(s string) string {
	const (
		base        = 36
		tmin        = 1
		tmax        = 26
		skew        = 38
		damp        = 700
		initialBias = 72
		initialN    = 128
	)

	var (
		output   []rune
		encoding string
	)
	if idx := strings.LastIndex(s, "_"); idx >= 0 {
		output = []rune(s[:idx])
		encoding = s[idx+1:]
	} else {
		encoding = s
	}

	i := 0
	n := initialN
	bias := initialBias

	pos := 0
	for pos < len(encoding) {
		oldI := i
		w := 1
		for k := base; ; k += base {
			if pos == len(encoding) {
				p.fail("unterminated punycode")
			}

			var digit byte
			d := encoding[pos]
			pos++
			switch {
			case '0' <= d && d <= '9':
				digit = d - '0' + 26
			case 'A' <= d && d <= 'Z':
				digit = d - 'A'
			case 'a' <= d && d <= 'z':
				digit = d - 'a'
			default:
				p.fail("invalid punycode digit")
			}

			i += int(digit) * w
			if i < 0 {
				p.fail("punycode number overflow")
			}

			var t int
			if k <= bias {
				t = tmin
			} else if k > bias+tmax {
				t = tmax
			} else {
				t = k - bias
			}

			if int(digit) < t {
				break
			}

			if w >= math.MaxInt32/base {
				p.fail("punycode number overflow")
			}
			w *= base - t
		}

		delta := i - oldI
		numPoints := len(output) + 1
		if oldI == 0 {
			delta /= damp
		} else {
			delta /= 2
		}
		delta += delta / numPoints
		k := 0
		for delta > ((base-tmin)*tmax)/2 {
			delta /= base - tmin
			k += base
		}
		bias = k + ((base-tmin+1)*delta)/(delta+skew)

		n += i / (len(output) + 1)
		if n > utf8.MaxRune {
			p.fail("punycode rune overflow")
		} else if !utf8.ValidRune(rune(n)) {
			p.fail("punycode invalid code point")
		}
		i %= len(output) + 1
		output = append(output, 0)
		copy(output[i+1:], output[i:])
		output[i] = rune(n)
		i++
	}

	return string(output)
}

// parseGenericArgs parses the {<generic-arg>} list of an I path.
func (p *rustParser) parseGenericArgs() *RustNode {
	start := p.off
	n := p.newNode(RustNodeGenericArgs, start)
	for len(p.str) > 0 && p.str[0] != 'E' {
		argStart := p.off
		arg := p.newNode(RustNodeGenericArg, argStart)
		if len(p.str) < 1 {
			p.fail("expected generic-arg")
		}
		switch p.str[0] {
		case 'L':
			arg.Children = []*RustNode{p.parseLifetime()}
		case 'K':
			p.advance(1)
			arg.Children = []*RustNode{p.parseConst()}
		default:
			arg.Children = []*RustNode{p.parseType()}
		}
		n.Children = append(n.Children, p.finish(arg))
	}
	return p.finish(n)
}

// parseLifetime parses "L" <base-62-number>. The raw index is kept;
// the printable name is recovered by the printer.
func (p *rustParser) parseLifetime() *RustNode {
	start := p.off
	p.checkChar('L')
	idx := p.base62Number()
	if idx > 0 && idx > p.lifetimes {
		// No binder in scope introduced this lifetime.
		p.failAt(start, "invalid lifetime")
	}
	n := p.newNode(RustNodeLifetime, start)
	n.Value = formatInt(idx)
	return p.finish(n)
}

// parseBinder parses an optional "G" <base-62-number> and extends the
// current lifetime scope for as long as the enclosing F or D
// production is being parsed.
func (p *rustParser) parseBinder() *RustNode {
	if len(p.str) < 1 || p.str[0] != 'G' {
		return nil
	}
	start := p.off
	p.advance(1)
	binderLifetimes := p.base62Number() + 1

	// Keep the historical sanity check: a binder cannot introduce
	// more lifetimes than the rest of the encoding could reference.
	if binderLifetimes >= int64(len(p.str))-p.lifetimes {
		p.fail("binder lifetimes overflow")
	}

	// Extend the scope immediately, so every lifetime the binder
	// governs is resolvable while the following production is parsed.
	p.lifetimes += binderLifetimes

	n := p.newNode(RustNodeBinder, start)
	n.Value = formatInt(binderLifetimes)
	return p.finish(n)
}

// parseType parses a <type> production.
func (p *rustParser) parseType() *RustNode {
	start := p.off
	if len(p.str) < 1 {
		p.fail("expected type")
	}
	c := p.str[0]
	if c >= 'a' && c <= 'z' {
		name, ok := rustBasicTypes[c]
		if !ok {
			p.fail("unrecognized basic type character")
		}
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = name
		return p.register(n)
	}
	switch c {
	case 'C', 'M', 'X', 'Y', 'N', 'I':
		// Named type: the path node itself stands in as the type.
		return p.parsePath()
	case 'A', 'S':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		if c == 'A' {
			n.Value = "array"
			n.Children = []*RustNode{p.parseType(), p.parseConst()}
		} else {
			n.Value = "slice"
			n.Children = []*RustNode{p.parseType()}
		}
		return p.register(p.finish(n))
	case 'T':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = "tuple"
		for len(p.str) > 0 && p.str[0] != 'E' {
			n.Children = append(n.Children, p.parseType())
		}
		p.checkChar('E')
		return p.register(p.finish(n))
	case 'R', 'Q':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		if c == 'R' {
			n.Value = "ref"
		} else {
			n.Value = "ref-mut"
		}
		if len(p.str) > 0 && p.str[0] == 'L' {
			n.Children = append(n.Children, p.parseLifetime())
		}
		n.Children = append(n.Children, p.parseType())
		return p.register(p.finish(n))
	case 'P':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = "ptr-const"
		n.Children = []*RustNode{p.parseType()}
		return p.register(p.finish(n))
	case 'O':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = "ptr-mut"
		n.Children = []*RustNode{p.parseType()}
		return p.register(p.finish(n))
	case 'F':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = "fn"
		hold := p.lifetimes
		n.Children = []*RustNode{p.parseFnSig()}
		p.lifetimes = hold
		return p.register(p.finish(n))
	case 'D':
		p.advance(1)
		n := p.newNode(RustNodeType, start)
		n.Value = "dyn"
		hold := p.lifetimes
		n.Children = []*RustNode{p.parseDynBounds()}
		p.lifetimes = hold
		if len(p.str) == 0 || p.str[0] != 'L' {
			p.fail("expected L")
		}
		n.Children = append(n.Children, p.parseLifetime())
		return p.register(p.finish(n))
	case 'B':
		return p.parseBackref(RustNodeType)
	default:
		p.fail("unrecognized character in type")
		return nil
	}
}

// parseFnSig parses [<binder>] ["U"] ["K" <abi>] {<type>} "E" <type>.
func (p *rustParser) parseFnSig() *RustNode {
	start := p.off
	n := p.newNode(RustNodeFnSig, start)

	if b := p.parseBinder(); b != nil {
		n.Children = append(n.Children, b)
	}

	if len(p.str) > 0 && p.str[0] == 'U' {
		p.advance(1)
		n.Value = "unsafe"
	}

	if len(p.str) > 0 && p.str[0] == 'K' {
		abiStart := p.off
		p.advance(1)
		abi := p.newNode(RustNodeABI, abiStart)
		if len(p.str) > 0 && p.str[0] == 'C' {
			p.advance(1)
			abi.Value = "C"
		} else {
			idStart := p.off
			idNode := p.newNode(RustNodeIdentifier, idStart)
			id, isPunycode := p.parseUndisambiguatedIdentifierRaw()
			if isPunycode {
				p.fail("punycode used in ABI string")
			}
			idNode.Value = strings.ReplaceAll(id, "_", "-")
			p.finish(idNode)
			abi.Value = idNode.Value
			abi.Children = []*RustNode{idNode}
		}
		n.Children = append(n.Children, p.finish(abi))
	}

	for len(p.str) > 0 && p.str[0] != 'E' {
		n.Children = append(n.Children, p.parseType())
	}
	p.checkChar('E')

	if len(p.str) > 0 && p.str[0] == 'u' {
		// Unit return type: nothing follows and nothing is printed.
		p.advance(1)
	} else {
		retStart := p.off
		ret := p.newNode(RustNodeReturnType, retStart)
		ret.Children = []*RustNode{p.parseType()}
		n.Children = append(n.Children, p.finish(ret))
	}

	return p.finish(n)
}

// parseDynBounds parses [<binder>] {<dyn-trait>} "E".
func (p *rustParser) parseDynBounds() *RustNode {
	start := p.off
	n := p.newNode(RustNodeDynBounds, start)

	if b := p.parseBinder(); b != nil {
		n.Children = append(n.Children, b)
	}

	for len(p.str) > 0 && p.str[0] != 'E' {
		n.Children = append(n.Children, p.parseDynTrait())
	}
	p.checkChar('E')
	return p.finish(n)
}

// parseDynTrait parses <path> {<dyn-trait-assoc-binding>}.
func (p *rustParser) parseDynTrait() *RustNode {
	start := p.off
	n := p.newNode(RustNodeDynTrait, start)

	n.Children = append(n.Children, p.parsePathStartGenerics())
	for len(p.str) > 0 && p.str[0] == 'p' {
		bStart := p.off
		p.advance(1)
		b := p.newNode(RustNodeAssocBinding, bStart)
		idStart := p.off
		idNode := p.newNode(RustNodeIdentifier, idStart)
		idNode.Value, _ = p.parseUndisambiguatedIdentifierRaw()
		p.finish(idNode)
		b.Children = []*RustNode{idNode, p.parseType()}
		n.Children = append(n.Children, p.finish(b))
	}
	return p.finish(n)
}

// parsePathStartGenerics parses a dyn-trait path whose opening I path
// leaves the angle bracket open for associated-item bindings.
func (p *rustParser) parsePathStartGenerics() *RustNode {
	start := p.off
	if len(p.str) < 1 {
		p.fail("expected path")
	}
	switch p.str[0] {
	case 'I':
		p.advance(1)
		n := p.newNode(RustNodePath, start)
		n.Children = []*RustNode{p.parsePath(), p.parseGenericArgs()}
		p.checkChar('E')
		return p.register(p.finish(n))
	case 'B':
		return p.parseBackref(RustNodePath)
	default:
		return p.parsePath()
	}
}

// parseConst parses a <const> production. The leading "K" has already
// been consumed by the caller when the const is a generic argument.
func (p *rustParser) parseConst() *RustNode {
	start := p.off
	if len(p.str) < 1 {
		p.fail("expected constant")
	}
	if p.str[0] == 'B' {
		return p.parseBackref(RustNodeConst)
	}

	n := p.newNode(RustNodeConst, start)
	if p.str[0] == 'p' {
		p.advance(1)
		n.Value = "placeholder"
		return p.register(p.finish(n))
	}

	typ := p.str[0]
	const (
		invalid = iota
		signedInt
		unsignedInt
		boolean
		character
	)
	var kind int
	switch typ {
	case 'a', 's', 'l', 'x', 'n', 'i':
		kind = signedInt
	case 'h', 't', 'm', 'y', 'o', 'j':
		kind = unsignedInt
	case 'b':
		kind = boolean
	case 'c':
		kind = character
	default:
		p.fail("unrecognized constant type")
	}

	typeStart := p.off
	p.advance(1)
	typeNode := p.newNode(RustNodeType, typeStart)
	typeNode.Value = rustBasicTypes[typ]
	p.finish(typeNode)

	negative := false
	if kind == signedInt && len(p.str) > 0 && p.str[0] == 'n' {
		p.advance(1)
		negative = true
	}

	dataStart := p.off
	digits := 0
	val := uint64(0)
digitLoop:
	for len(p.str) > 0 {
		c := p.str[0]
		var digit uint64
		switch {
		case c >= '0' && c <= '9':
			digit = uint64(c - '0')
		case c >= 'a' && c <= 'f':
			digit = uint64(c-'a') + 10
		case c == '_':
			p.advance(1)
			break digitLoop
		default:
			p.fail("expected hex digit or _")
		}
		p.advance(1)
		if val == 0 && digit == 0 && (len(p.str) == 0 || p.str[0] != '_') {
			p.fail("invalid leading 0 in constant")
		}
		val *= 16
		val += digit
		digits++
	}

	if digits == 0 {
		p.fail("expected constant")
	}

	dataNode := p.newNode(RustNodeConstData, dataStart)
	switch kind {
	case signedInt, unsignedInt:
		if digits > 16 {
			// Value too big to render as an integer; keep the hex text.
			dataNode.Value = "0x" + p.source[dataStart+2:p.off-1+2]
			if negative {
				dataNode.Value = "-" + dataNode.Value
			}
		} else {
			dataNode.Value = formatUint(val)
			if negative {
				dataNode.Value = "-" + dataNode.Value
			}
		}
	case boolean:
		if digits > 1 {
			p.fail("boolean value too large")
		}
		switch val {
		case 0:
			dataNode.Value = "false"
		case 1:
			dataNode.Value = "true"
		default:
			p.fail("invalid boolean value")
		}
	case character:
		if digits > 6 {
			p.fail("character value too large")
		}
		dataNode.Value = decodeRustChar(val)
	default:
		panic("internal error")
	}
	p.finish(dataNode)

	n.Children = []*RustNode{typeNode, dataNode}
	return p.register(p.finish(n))
}

// decodeRustChar renders a character constant the way the legacy
// printer did.
func decodeRustChar(val uint64) string {
	var b strings.Builder
	b.WriteByte('\'')
	switch val {
	case '\t':
		b.WriteString(`\t`)
	case '\r':
		b.WriteString(`\r`)
	case '\n':
		b.WriteString(`\n`)
	case '\\':
		b.WriteString(`\\`)
	case '\'':
		b.WriteString(`\'`)
	default:
		if val >= ' ' && val <= '~' {
			b.WriteByte(byte(val))
		} else {
			b.WriteString(`\u{`)
			b.WriteString(formatHex(val))
			b.WriteByte('}')
		}
	}
	b.WriteByte('\'')
	return b.String()
}

// parseBackref parses "B" <base-62-number> and resolves the target
// node. wantKind is the syntactic role of the occurrence:
// paths, types and constants each interpret the referenced encoding in
// their own grammar.
//
// Only completed production-start nodes are registered, so an offset
// inside an identifier or a number cannot resolve, and an ancestor
// still being parsed (a self reference) is absent from the table.
// Forward references fail the strict index bound.
func (p *rustParser) parseBackref(wantKind string) *RustNode {
	start := p.off
	p.checkChar('B')
	idx64 := p.base62Number()
	idx := int(idx64)
	if int64(idx) != idx64 {
		p.failAt(start, "backref index overflow")
	}
	if idx < 0 || idx >= start {
		// This rejects forward references and self references.
		p.failAt(start, "invalid backref index")
	}

	target, ok := p.nodes[idx]
	if !ok {
		// The offset does not start a completed node: it points inside
		// an identifier, inside a number encoding, or at a production
		// that is still open.
		p.failAt(start, "invalid backref index")
	}

	// A target that is itself a back-reference is followed to its
	// target. Referenced indices strictly decrease, so this loop can
	// only terminate or hit a nil Ref on malformed input.
	seen := map[*RustNode]bool{}
	for target.Kind == RustNodeBackref {
		if seen[target] || target.Ref == nil {
			p.failAt(start, "invalid backref index")
		}
		seen[target] = true
		target = target.Ref
	}

	switch wantKind {
	case RustNodePath:
		if target.Kind != RustNodePath {
			p.failAt(start, "backref does not point at a path")
		}
	case RustNodeType:
		// A named type is encoded as a path.
		if target.Kind != RustNodeType && target.Kind != RustNodePath {
			p.failAt(start, "backref does not point at a type")
		}
	case RustNodeConst:
		if target.Kind != RustNodeConst {
			p.failAt(start, "backref does not point at a constant")
		}
	default:
		panic("internal error")
	}

	n := p.newNode(RustNodeBackref, start)
	n.Ref = target
	// Register the occurrence too: a later back-reference may target
	// this "B" encoding, matching the legacy re-parse semantics.
	p.register(n)
	return n
}

// base62Number parses {<0-9a-zA-Z>} "_".
func (p *rustParser) base62Number() int64 {
	if len(p.str) > 0 && p.str[0] == '_' {
		p.advance(1)
		return 0
	}
	val := int64(0)
	for len(p.str) > 0 {
		c := p.str[0]
		p.advance(1)
		if c == '_' {
			return val + 1
		}
		val *= 62
		switch {
		case c >= '0' && c <= '9':
			val += int64(c - '0')
		case c >= 'a' && c <= 'z':
			val += int64(c-'a') + 10
		case c >= 'A' && c <= 'Z':
			val += int64(c-'A') + 36
		default:
			p.fail("invalid digit in base 62 number")
		}
	}
	p.fail("expected _ after base 62 number")
	return 0
}

// decimalNumber parses a run of decimal digits.
func (p *rustParser) decimalNumber() int {
	if len(p.str) == 0 {
		p.fail("expected number")
	}
	val := 0
	for len(p.str) > 0 && isDigit(p.str[0]) {
		add := int(p.str[0] - '0')
		if val >= math.MaxInt32/10-add {
			p.fail("decimal number overflow")
		}
		val *= 10
		val += add
		p.advance(1)
	}
	return val
}

// parseUndisambiguatedIdentifierRaw parses an identifier without the
// optional disambiguator (ABI names, associated-item names) and
// reports whether the punycode marker was present.
func (p *rustParser) parseUndisambiguatedIdentifierRaw() (string, bool) {
	isPunycode := false
	if len(p.str) > 0 && p.str[0] == 'u' {
		p.advance(1)
		isPunycode = true
	}
	val := p.decimalNumber()
	if len(p.str) > 0 && p.str[0] == '_' {
		p.advance(1)
	}
	if len(p.str) < val {
		p.fail("not enough characters for identifier")
	}
	id := p.str[:val]
	p.advance(val)
	for i := 0; i < len(id); i++ {
		c := id[i]
		switch {
		case c >= '0' && c <= '9':
		case c >= 'A' && c <= 'Z':
		case c >= 'a' && c <= 'z':
		case c == '_':
		default:
			p.fail("invalid character in identifier")
		}
	}
	if isPunycode {
		id = p.expandPunycode(id)
	}
	return id, isPunycode
}
