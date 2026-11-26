package demangle

import (
	"fmt"
	"math/rand"
	"strconv"
	"strings"
	"testing"
)

// This file contains an independent structural C++ type model, a small
// Itanium encoder, and projection-based checks.  The model is deliberately
// not made from demangler AST nodes, and expected strings are printed by the
// model rather than copied from ToString.

type imType interface {
	imKey() string
}

type imBuiltin struct{ name string }

func (t *imBuiltin) imKey() string { return "B(" + t.name + ")" }

type imArg struct {
	typ    imType
	intVal int
	isInt  bool
}

func imTypeArg(t imType) imArg { return imArg{typ: t} }
func imIntArg(v int) imArg     { return imArg{intVal: v, isInt: true} }

type imPart struct {
	name string
	args []imArg // nil means an ordinary unqualified name component
}

func imNamePart(name string) imPart { return imPart{name: name} }
func imTplPart(name string, args ...imArg) imPart {
	return imPart{name: name, args: args}
}

type imNamed struct{ parts []imPart }

func (t *imNamed) imKey() string {
	// Use the same layer-explicit shape as the structural projection.  In
	// particular a Template(Name, Args) is not merged with its plain Name
	// candidate merely because both print as the same prefix text.
	return imProjNamed(t)
}

type imCV struct {
	base      imType
	constant  bool
	volatile_ bool
}

func (t *imCV) imKey() string {
	return fmt.Sprintf("CV(c=%v,v=%v,%s)", t.constant, t.volatile_, t.base.imKey())
}

type imPtr struct{ base imType }

func (t *imPtr) imKey() string { return "P(" + t.base.imKey() + ")" }

type imRef struct {
	base imType
	rv   bool
}

func (t *imRef) imKey() string {
	return fmt.Sprintf("R(rv=%v,%s)", t.rv, t.base.imKey())
}

type imArray struct {
	elem imType
	dim  int
}

func (t *imArray) imKey() string {
	return fmt.Sprintf("A(%d,%s)", t.dim, t.elem.imKey())
}

type imFunc struct {
	ret    imType
	params []imType
}

func (t *imFunc) imKey() string {
	var b strings.Builder
	b.WriteString("F(ret=")
	if t.ret == nil {
		b.WriteString("void")
	} else {
		b.WriteString(t.ret.imKey())
	}
	b.WriteString(",args=[")
	for i, p := range t.params {
		if i > 0 {
			b.WriteString(",")
		}
		b.WriteString(p.imKey())
	}
	b.WriteString("])")
	return b.String()
}

func ib(name string) *imBuiltin { return &imBuiltin{name: name} }
func in(names ...string) *imNamed {
	parts := make([]imPart, len(names))
	for i, n := range names {
		parts[i] = imNamePart(n)
	}
	return &imNamed{parts: parts}
}
func ip(base imType) *imPtr  { return &imPtr{base: base} }
func ir(base imType) *imRef  { return &imRef{base: base} }
func irr(base imType) *imRef { return &imRef{base: base, rv: true} }
func ic(base imType) *imCV   { return &imCV{base: base, constant: true} }
func iv(base imType) *imCV   { return &imCV{base: base, volatile_: true} }
func icv(base imType) *imCV {
	return &imCV{base: base, constant: true, volatile_: true}
}
func ia(elem imType, dim int) *imArray { return &imArray{elem: elem, dim: dim} }
func ifn(ret imType, params ...imType) *imFunc {
	return &imFunc{ret: ret, params: params}
}
func ifp(ret imType, params ...imType) *imPtr {
	return &imPtr{base: &imFunc{ret: ret, params: params}}
}

type imModel struct {
	id     int
	group  string
	fn     string
	result imType // used by the model's own C++ declaration; not in a mangled function name
	params []imType
	note   string
}

// ---------------------------------------------------------------------------
// Independent C++ printing and structural expectation.
// ---------------------------------------------------------------------------

func imArgCPP(a imArg) string {
	if a.isInt {
		return strconv.Itoa(a.intVal)
	}
	return imCPPType(a.typ)
}

func imNamedCPP(n *imNamed) string {
	var b strings.Builder
	var prefix string
	for i, p := range n.parts {
		name := p.name
		if p.args != nil {
			var args strings.Builder
			for j, a := range p.args {
				if j > 0 {
					args.WriteString(", ")
				}
				args.WriteString(imArgCPP(a))
			}
			name = p.name + "<" + args.String() + ">"
		}
		if i == 0 {
			prefix = name
		} else {
			prefix += "::" + name
		}
	}
	b.WriteString(prefix)
	return b.String()
}

func imDeclNeedsParens(t imType) bool {
	switch v := t.(type) {
	case *imArray, *imFunc:
		return true
	case *imCV:
		return imDeclNeedsParens(v.base)
	default:
		return false
	}
}

// imPattern returns a declaration pattern with one '@' marking the declared
// name.  The empty abstract declarator is obtained by deleting '@'.
func imPattern(t imType) string {
	switch v := t.(type) {
	case *imBuiltin:
		return v.name + " @"
	case *imNamed:
		return imNamedCPP(v) + " @"
	case *imCV:
		p := imPattern(v.base)
		i := strings.IndexByte(p, '@')
		pre := strings.TrimRight(p[:i], " ")
		quals := ""
		if v.constant {
			quals = "const"
		}
		if v.volatile_ {
			if quals != "" {
				quals += " "
			}
			quals += "volatile"
		}
		return pre + " " + quals + " @" + p[i+1:]
	case *imPtr:
		p := imPattern(v.base)
		i := strings.IndexByte(p, '@')
		pre := strings.TrimRight(p[:i], " ")
		parens := imDeclNeedsParens(v.base)
		if parens {
			return pre + " (*@)" + p[i+1:]
		}
		return pre + "*@" + p[i+1:]
	case *imRef:
		p := imPattern(v.base)
		i := strings.IndexByte(p, '@')
		pre := strings.TrimRight(p[:i], " ")
		op := "&@"
		if v.rv {
			op = "&&@"
		}
		if imDeclNeedsParens(v.base) {
			return pre + " (" + op + ")" + p[i+1:]
		}
		return pre + op + p[i+1:]
	case *imArray:
		p := imPattern(v.elem)
		i := strings.IndexByte(p, '@')
		return p[:i] + "@[" + strconv.Itoa(v.dim) + "]" + p[i+1:]
	case *imFunc:
		ret := imPattern(v.ret)
		i := strings.IndexByte(ret, '@')
		var args strings.Builder
		for j, a := range v.params {
			if j > 0 {
				args.WriteString(", ")
			}
			args.WriteString(imCPPType(a))
		}
		return ret[:i] + "@(" + args.String() + ")" + ret[i+1:]
	default:
		return "UNKNOWN_TYPE @"
	}
}

func imNormalizeAbstract(s string) string {
	s = strings.ReplaceAll(s, "@", "")
	// Collapse runs of spaces introduced around the marker.
	for strings.Contains(s, "  ") {
		s = strings.ReplaceAll(s, "  ", " ")
	}
	// An empty abstract declarator must not leave "const )" behind when the
	// marker was immediately before the closing declarator parenthesis.
	s = strings.ReplaceAll(s, " )", ")")
	// The demangler prints a space before the first bracket of a contiguous
	// array dimension chain, but not between adjacent dimensions.
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '[' {
			for b.Len() > 0 && b.String()[b.Len()-1] == ' ' {
				// Remove any existing whitespace; decide below.
				bs := b.String()
				b.Reset()
				b.WriteString(strings.TrimRight(bs, " "))
			}
			if b.Len() > 0 {
				last := b.String()[b.Len()-1]
				if last != ']' {
					b.WriteByte(' ')
				}
			}
		}
		b.WriteByte(s[i])
	}
	return strings.TrimSpace(b.String())
}

func imCPPType(t imType) string {
	return imNormalizeAbstract(imPattern(t))
}

func (m *imModel) wantDemangled() string {
	ps := make([]string, len(m.params))
	for i, p := range m.params {
		ps[i] = imCPPType(p)
	}
	return m.fn + "(" + strings.Join(ps, ", ") + ")"
}

// cppDeclaration is the model's independent legal C++ declaration.  Ordinary
// mangled function names do not encode the function's return type, so this is
// intentionally richer than wantDemangled.
func (m *imModel) cppDeclaration() string {
	ret := "void"
	if m.result != nil {
		ret = imPattern(m.result)
		ret = strings.ReplaceAll(ret, "@", m.fn)
		ret = strings.TrimSpace(ret)
	}
	ps := make([]string, len(m.params))
	for i, p := range m.params {
		pat := imPattern(p)
		name := m.fn + "_arg" + strconv.Itoa(i)
		ps[i] = strings.TrimSpace(strings.ReplaceAll(pat, "@", name))
	}
	return ret + " " + m.fn + "(" + strings.Join(ps, ", ") + ");"
}

func imProjArg(a imArg) string {
	if a.isInt {
		return "Lit(" + strconv.Itoa(a.intVal) + ")"
	}
	return imProjType(a.typ)
}

func imProjNamed(n *imNamed) string {
	var p string
	for _, part := range n.parts {
		var node string
		name := "N(" + part.name + ")"
		if part.args != nil {
			nameThrough := name
			if p != "" {
				nameThrough = "Q(" + p + "::" + name + ")"
			}
			args := make([]string, len(part.args))
			for j, a := range part.args {
				args[j] = imProjArg(a)
			}
			node = "Tpl(" + nameThrough + ";args=[" + strings.Join(args, ",") + "])"
		} else if p == "" {
			node = name
		} else {
			node = "Q(" + p + "::" + name + ")"
		}
		p = node
	}
	return p
}

func imProjType(t imType) string {
	switch v := t.(type) {
	case *imBuiltin:
		return "B(" + v.name + ")"
	case *imNamed:
		return imProjNamed(v)
	case *imCV:
		var qs []string
		if v.constant {
			qs = append(qs, "const")
		}
		if v.volatile_ {
			qs = append(qs, "volatile")
		}
		return "CV([" + strings.Join(qs, ",") + "]," + imProjType(v.base) + ")"
	case *imPtr:
		return "Ptr(" + imProjType(v.base) + ")"
	case *imRef:
		if v.rv {
			return "Rref(" + imProjType(v.base) + ")"
		}
		return "Lref(" + imProjType(v.base) + ")"
	case *imArray:
		return "Array(" + strconv.Itoa(v.dim) + "," + imProjType(v.elem) + ")"
	case *imFunc:
		ret := "-"
		if v.ret != nil {
			ret = imProjType(v.ret)
		}
		args := make([]string, len(v.params))
		for i, p := range v.params {
			args[i] = imProjType(p)
		}
		return "Fn(ret=" + ret + ",args=[" + strings.Join(args, ",") + "])"
	default:
		return "UNKNOWN(" + fmt.Sprintf("%T", t) + ")"
	}
}

func (m *imModel) wantProjection() string {
	args := make([]string, len(m.params))
	for i, p := range m.params {
		args[i] = imProjType(p)
	}
	return "Typed(name=N(" + m.fn + "),Fn(ret=-,args=[" + strings.Join(args, ",") + "]))"
}

// ---------------------------------------------------------------------------
// Itanium encoder with its own structural candidate table.
// ---------------------------------------------------------------------------

type imCandidate struct {
	slot  int
	key   string
	label string
}

type imOp struct {
	path     string
	key      string
	decision string // expand, ref, prefix-ref
	slot     int
	encoding string
}

type imStrategy struct {
	mode int // 0 refs whenever possible, 1 never refs, 2 deterministic selection
	salt int64
}

const (
	imRefsAll = iota
	imRefsNone
	imRefsSelect
)

type imEncoding struct {
	body  string
	table []imCandidate
	ops   []imOp
}

type imEnc struct {
	strategy imStrategy
	out      strings.Builder
	table    []imCandidate
	ops      []imOp
}

func imSeqID(index int) string {
	if index == 0 {
		return "S_"
	}
	n := index - 1
	const digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	var b []byte
	for {
		b = append([]byte{digits[n%36]}, b...)
		n /= 36
		if n == 0 {
			break
		}
	}
	return "S" + string(b) + "_"
}

func sourceName(s string) string { return strconv.Itoa(len(s)) + s }

func (e *imEnc) raw(s string) { e.out.WriteString(s) }

func (e *imEnc) find(key string) (int, bool) {
	for _, c := range e.table {
		if c.key == key {
			return c.slot, true
		}
	}
	return -1, false
}

func (e *imEnc) reg(key, label, path string) int {
	slot := len(e.table)
	e.table = append(e.table, imCandidate{slot: slot, key: key, label: label})
	e.ops = append(e.ops, imOp{
		path: path, key: key, decision: "expand-register", slot: slot,
		encoding: imSeqID(slot),
	})
	return slot
}

func (e *imEnc) allowRef(path, key string, slot int) bool {
	switch e.strategy.mode {
	case imRefsAll:
		return true
	case imRefsNone:
		return false
	default:
		// Deterministic but varied selection.  The salt lets reuse build
		// several legal encodings of one model.
		h := int64(1469598103934665603)
		for _, c := range path + "|" + key {
			h ^= int64(c)
			h *= 1099511628211
		}
		h ^= e.strategy.salt
		return h%5 != 0 // use most existing candidates, expand the rest
	}
}

func (e *imEnc) emitType(t imType, path string) {
	key := t.imKey()
	if slot, ok := e.find(key); ok && e.allowRef(path, key, slot) {
		code := imSeqID(slot)
		e.raw(code)
		e.ops = append(e.ops, imOp{
			path: path, key: key, decision: "ref", slot: slot, encoding: code,
		})
		return
	}

	switch v := t.(type) {
	case *imBuiltin:
		e.raw(imBuiltinCode(v.name))
		return // builtins are not implicit substitution candidates
	case *imNamed:
		e.emitNamed(v, path)
	case *imCV:
		// cvQualifiers prepend later qualifiers into AST order; emitting VK
		// gives the model's canonical const volatile order.
		if v.constant && v.volatile_ {
			e.raw("VK")
		} else if v.constant {
			e.raw("K")
		} else if v.volatile_ {
			e.raw("V")
		}
		e.emitType(v.base, path+".base")
	case *imPtr:
		e.raw("P")
		e.emitType(v.base, path+".base")
	case *imRef:
		if v.rv {
			e.raw("O")
		} else {
			e.raw("R")
		}
		e.emitType(v.base, path+".base")
	case *imArray:
		e.raw("A" + strconv.Itoa(v.dim) + "_")
		e.emitType(v.elem, path+".elem")
	case *imFunc:
		e.raw("F")
		e.emitType(v.ret, path+".ret")
		for i, p := range v.params {
			e.emitType(p, fmt.Sprintf("%s.arg%d", path, i))
		}
		e.raw("E")
	default:
		panic("itanium model: unsupported type in encoder")
	}

	// Every non-builtin complete type returns through demangleType's
	// addSubst path in this subset.
	e.reg(key, imCPPType(t), path)
}

func imBuiltinCode(name string) string {
	switch name {
	case "void":
		return "v"
	case "bool":
		return "b"
	case "char":
		return "c"
	case "int":
		return "i"
	case "unsigned int":
		return "j"
	case "long":
		return "l"
	case "unsigned long":
		return "m"
	case "long long":
		return "x"
	case "float":
		return "f"
	case "double":
		return "d"
	default:
		panic("itanium model: unsupported builtin " + name)
	}
}

func imPrefixKey(n *imNamed, count int) string {
	p := &imNamed{parts: append([]imPart(nil), n.parts[:count]...)}
	return p.imKey()
}

// prefixHasTemplate reports whether the replaced prefix contains a template
// argument block.  The structured tests currently use substitution prefixes
// only for plain qualified prefixes; complete template types are referenced
// directly.
func imPrefixHasTemplate(n *imNamed, count int) bool {
	for i := 0; i < count; i++ {
		if n.parts[i].args != nil {
			return true
		}
	}
	return false
}

func (e *imEnc) emitNamed(n *imNamed, path string) {
	e.raw("N")
	start := 0

	// Share a plain qualified prefix when an identical structural candidate
	// is already registered.
	best := -1
	bestSlot := -1
	for k := 1; k < len(n.parts); k++ {
		if imPrefixHasTemplate(n, k) {
			continue
		}
		if slot, ok := e.find(imPrefixKey(n, k)); ok {
			if e.strategy.mode == imRefsAll || e.allowRef(path+".prefix", imPrefixKey(n, k), slot) {
				best = k
				bestSlot = slot
			}
		}
	}
	if best > 0 {
		code := imSeqID(bestSlot)
		e.raw(code)
		e.ops = append(e.ops, imOp{
			path: path + ".prefix", key: imPrefixKey(n, best),
			decision: "prefix-ref", slot: bestSlot, encoding: code,
		})
		start = best
	}

	for i := start; i < len(n.parts); i++ {
		p := n.parts[i]
		e.raw(sourceName(p.name))

		if p.args != nil {
			nameCount := i + 1
			plain := &imNamed{parts: append([]imPart(nil), n.parts[:nameCount]...)}
			plain.parts[nameCount-1].args = nil
			// prefix() adds the template's plain name before the next loop
			// iteration parses and registers candidates inside I...E.
			e.reg(plain.imKey(), imNamedCPP(plain), path+".template-name")

			e.raw("I")
			for _, a := range p.args {
				if a.isInt {
					e.raw("L" + imBuiltinCode("int") + strconv.Itoa(a.intVal) + "E")
				} else {
					e.emitType(a.typ, fmt.Sprintf("%s.part%d.arg", path, i))
				}
			}
			e.raw("E")

			if i < len(n.parts)-1 {
				tpl := &imNamed{parts: append([]imPart(nil), n.parts[:nameCount]...)}
				e.reg(tpl.imKey(), imNamedCPP(tpl), path+".template-prefix")
			}
		} else if i < len(n.parts)-1 {
			key := imPrefixKey(n, i+1)
			e.reg(key, imNamedCPP(&imNamed{parts: n.parts[:i+1]}), path+".prefix")
		}
	}
	e.raw("E")
}

func (m *imModel) encode(strategy imStrategy) (*imEncoding, error) {
	e := &imEnc{strategy: strategy}
	for i, p := range m.params {
		e.emitType(p, fmt.Sprintf("param%d", i))
	}
	return &imEncoding{body: e.out.String(), table: e.table, ops: e.ops}, nil
}

func (m *imModel) symbol(mode int, salt int64) (string, *imEncoding, error) {
	enc, err := m.encode(imStrategy{mode: mode, salt: salt})
	if err != nil {
		return "", nil, err
	}
	return "_Z" + sourceName(m.fn) + enc.body, enc, nil
}

// ---------------------------------------------------------------------------
// AST projection.
// ---------------------------------------------------------------------------

func imQualifierNames(q AST) []string {
	qs, ok := q.(*Qualifiers)
	if !ok {
		if ql, ok := q.(*Qualifier); ok {
			return []string{ql.Name}
		}
		return []string{fmt.Sprintf("%T", q)}
	}
	out := make([]string, 0, len(qs.Qualifiers))
	for _, a := range qs.Qualifiers {
		if ql, ok := a.(*Qualifier); ok {
			out = append(out, ql.Name)
		} else {
			out = append(out, fmt.Sprintf("%T", a))
		}
	}
	return out
}

func imProjectAST(a AST) string {
	switch v := a.(type) {
	case *Typed:
		return "Typed(name=" + imProjectAST(v.Name) + "," + imProjectAST(v.Type) + ")"
	case *Name:
		return "N(" + v.Name + ")"
	case *BuiltinType:
		return "B(" + v.Name + ")"
	case *Qualified:
		return "Q(" + imProjectAST(v.Scope) + "::" + imProjectAST(v.Name) + ")"
	case *Template:
		args := make([]string, len(v.Args))
		for i, x := range v.Args {
			args[i] = imProjectAST(x)
		}
		return "Tpl(" + imProjectAST(v.Name) + ";args=[" + strings.Join(args, ",") + "])"
	case *Literal:
		if v.Neg {
			return "Lit(-" + v.Val + ")"
		}
		return "Lit(" + v.Val + ")"
	case *TypeWithQualifiers:
		return "CV([" + strings.Join(imQualifierNames(v.Qualifiers), ",") + "]," + imProjectAST(v.Base) + ")"
	case *PointerType:
		return "Ptr(" + imProjectAST(v.Base) + ")"
	case *ReferenceType:
		return "Lref(" + imProjectAST(v.Base) + ")"
	case *RvalueReferenceType:
		return "Rref(" + imProjectAST(v.Base) + ")"
	case *ArrayType:
		dim := "?"
		if n, ok := v.Dimension.(*Name); ok {
			dim = n.Name
		} else {
			dim = imProjectAST(v.Dimension)
		}
		return "Array(" + dim + "," + imProjectAST(v.Element) + ")"
	case *FunctionType:
		ret := "-"
		if v.Return != nil {
			ret = imProjectAST(v.Return)
		}
		args := make([]string, len(v.Args))
		for i, x := range v.Args {
			args[i] = imProjectAST(x)
		}
		return "Fn(ret=" + ret + ",args=[" + strings.Join(args, ",") + "])"
	default:
		return "UNKNOWN(" + fmt.Sprintf("%T", a) + ")"
	}
}

// ---------------------------------------------------------------------------
// Shared validation used by deterministic tests and FuzzItaniumSubstitution.
// ---------------------------------------------------------------------------

type imCheckFailure struct {
	model   *imModel
	kind    string
	detail  string
	symbols map[string]string
	tables  map[string][]imCandidate
	ops     map[string][]imOp
}

func (e *imCheckFailure) Error() string {
	var b strings.Builder
	b.WriteString("ITANIUM_MODEL_SEMANTIC_ASSERTION ")
	b.WriteString(e.kind)
	b.WriteString("\nmodel C++: ")
	b.WriteString(e.model.cppDeclaration())
	b.WriteString("\nmodel projection: ")
	b.WriteString(e.model.wantProjection())
	if e.model.note != "" {
		b.WriteString("\nnote: " + e.model.note)
	}
	for name, sym := range e.symbols {
		b.WriteString("\nencoding " + name + ": " + sym)
	}
	for name, table := range e.tables {
		b.WriteString("\ncandidate table " + name + ":")
		for _, c := range table {
			fmt.Fprintf(&b, "\n  %s slot=%d key=%s label=%s", imSeqID(c.slot), c.slot, c.key, c.label)
		}
	}
	for name, ops := range e.ops {
		b.WriteString("\nrewrite log " + name + ":")
		for _, op := range ops {
			fmt.Fprintf(&b, "\n  %s path=%s decision=%s slot=%d key=%s", op.encoding, op.path, op.decision, op.slot, op.key)
		}
	}
	if e.detail != "" {
		b.WriteString("\n" + e.detail)
	}
	return b.String()
}

func imFirstDiff(want, got string) string {
	rw, rg := []rune(want), []rune(got)
	n := len(rw)
	if len(rg) < n {
		n = len(rg)
	}
	pos := 0
	for pos < n && rw[pos] == rg[pos] {
		pos++
	}
	context := func(rs []rune, p int) string {
		lo := p - 30
		if lo < 0 {
			lo = 0
		}
		hi := p + 60
		if hi > len(rs) {
			hi = len(rs)
		}
		return string(rs[lo:hi])
	}
	return fmt.Sprintf("first deviation rune=%d\nwant: %q\n got: %q\nwant context: ...%s...\n got context: ...%s...",
		pos, want, got, context(rw, pos), context(rg, pos))
}

func checkItaniumModel(m *imModel) error {
	if m == nil {
		return fmt.Errorf("ITANIUM_MODEL_SEMANTIC_ASSERTION nil model")
	}
	refSym, refEnc, err := m.symbol(imRefsAll, 0)
	if err != nil {
		return err
	}
	plainSym, plainEnc, err := m.symbol(imRefsNone, 0)
	if err != nil {
		return err
	}
	symbols := map[string]string{"refs": refSym, "plain": plainSym}
	tables := map[string][]imCandidate{"refs": refEnc.table, "plain": plainEnc.table}

	validateOne := func(name, sym string, enc *imEncoding) error {
		ast, err := ToAST(sym)
		if err != nil {
			return &imCheckFailure{
				model: m, kind: "legal-symbol-rejected",
				detail:  fmt.Sprintf("encoding=%s error=%v", name, err),
				symbols: symbols, tables: tables,
			}
		}
		gotProj := imProjectAST(ast)
		wantProj := m.wantProjection()
		if gotProj != wantProj {
			return &imCheckFailure{
				model: m, kind: "projection-mismatch",
				detail:  "encoding=" + name + "\n" + imFirstDiff(wantProj, gotProj),
				symbols: symbols, tables: tables,
			}
		}
		wantStr := m.wantDemangled()
		gotASTStr := ASTToString(ast)
		if gotASTStr != wantStr {
			return &imCheckFailure{
				model: m, kind: "ast-to-string-mismatch",
				detail:  "encoding=" + name + "\n" + imFirstDiff(wantStr, gotASTStr),
				symbols: symbols, tables: tables,
			}
		}
		gotToStr, err := ToString(sym)
		if err != nil {
			return &imCheckFailure{
				model: m, kind: "to-string-error", detail: err.Error(),
				symbols: symbols, tables: tables,
			}
		}
		if gotToStr != wantStr {
			return &imCheckFailure{
				model: m, kind: "to-string-mismatch",
				detail:  "encoding=" + name + "\n" + imFirstDiff(wantStr, gotToStr),
				symbols: symbols, tables: tables,
			}
		}
		return nil
	}

	opsMap := map[string][]imOp{"refs": refEnc.ops, "plain": plainEnc.ops}
	attachOps := func(err error) error {
		if failure, ok := err.(*imCheckFailure); ok {
			failure.ops = opsMap
		}
		return err
	}

	if err := validateOne("refs", refSym, refEnc); err != nil {
		return attachOps(err)
	}
	if err := validateOne("plain", plainSym, plainEnc); err != nil {
		return attachOps(err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Deterministic generation.
// ---------------------------------------------------------------------------

var imBuiltinNames = []string{"int", "char", "bool", "double", "long"}

func imRandBuiltin(r *rand.Rand) *imBuiltin {
	return ib(imBuiltinNames[r.Intn(len(imBuiltinNames))])
}

func imRandPlainNamed(r *rand.Rand, minDepth int) *imNamed {
	pools := [][]string{
		{"A", "B", "C", "D", "E"},
		{"ns", "detail", "codec", "Node"},
		{"v1", "net", "http", "Header", "Field"},
	}
	pool := pools[r.Intn(len(pools))]
	depth := minDepth + r.Intn(3)
	if depth > len(pool) {
		depth = len(pool)
	}
	parts := make([]imPart, depth)
	for i := 0; i < depth; i++ {
		parts[i] = imNamePart(pool[i])
	}
	// Vary terminal/component names while keeping textual duplicates possible.
	if r.Intn(2) == 0 {
		parts[depth-1].name = []string{"Box", "Node", "Value", "Widget"}[r.Intn(4)]
	}
	return &imNamed{parts: parts}
}

func imRandTemplate(r *rand.Rand) *imNamed {
	switch r.Intn(5) {
	case 0:
		arg := imTypeArg(imRandBuiltin(r))
		return &imNamed{parts: []imPart{imNamePart("ns"), imNamePart("detail"), imTplPart("Box", arg)}}
	case 1:
		depth := 3 + r.Intn(2)
		n := imRandPlainNamed(r, depth)
		arg := imTypeArg(n)
		return &imNamed{parts: []imPart{imNamePart("geom"), imNamePart("v2"), imTplPart("Map", arg, imIntArg(3+r.Intn(30)))}}
	case 2:
		return &imNamed{parts: []imPart{
			imNamePart("wrap"), imNamePart("inner"), imTplPart("Counter", imIntArg(1+r.Intn(99))),
		}}
	case 3:
		// Same terminal class name inside an instantiated enclosing template.
		return &imNamed{parts: []imPart{
			imTplPart("Outer", imTypeArg(imRandBuiltin(r))),
			imNamePart("Box"),
		}}
	default:
		// Global template with the same Box name used by nested classes.
		return &imNamed{parts: []imPart{
			imTplPart("Box", imTypeArg(imRandBuiltin(r)), imIntArg(1+r.Intn(16))),
		}}
	}
}

func imRandBase(r *rand.Rand) imType {
	if r.Intn(2) == 0 {
		return imRandPlainNamed(r, 3)
	}
	return imRandTemplate(r)
}

func imVary(r *rand.Rand, base imType) imType {
	dim := 2 + r.Intn(30)
	switch r.Intn(10) {
	case 0:
		return base
	case 1:
		return ip(base)
	case 2:
		return ic(ip(base)) // const pointer: CV(Ptr(T))
	case 3:
		return ip(ic(base)) // pointer to const: Ptr(CV(T))
	case 4:
		return ir(base)
	case 5:
		return irr(base)
	case 6:
		return ia(ip(base), dim) // array of pointers
	case 7:
		return ip(ia(imRandBuiltin(r), dim)) // pointer to fixed array
	case 8:
		return ifp(imRandBuiltin(r), base, imRandBuiltin(r))
	default:
		return ip(icv(ip(ic(base))))
	}
}

func imGenerateGroup(group string, count int, seedBase int64) []*imModel {
	out := make([]*imModel, 0, count)
	for i := 0; i < count; i++ {
		seed := seedBase*1000 + int64(i)
		r := rand.New(rand.NewSource(seed))
		m := &imModel{
			id:    int(seed),
			group: group,
			fn:    fmt.Sprintf("im%04d", seed%10000),
		}
		m.result = imRandBuiltin(r)

		switch group {
		case "prefix_chain":
			a := in("Alpha", "Beta", "Gamma")
			b := &imNamed{parts: []imPart{imNamePart("Alpha"), imNamePart("Beta"), imNamePart("Delta")}}
			c := &imNamed{parts: []imPart{imNamePart("Alpha"), imNamePart("Epsilon")}}
			m.params = []imType{a, b, c, ip(b), ir(c)}
			if i%3 == 0 {
				long := imRandPlainNamed(r, 6)
				m.params = append(m.params, long, ip(long))
			}
		case "qualified_type":
			q := imRandPlainNamed(r, 3)
			m.params = []imType{
				q,
				ic(q),
				ip(q),
				ip(ic(q)),
				ic(ip(q)),
				ip(iv(ip(q))),
				irr(ip(ic(q))),
			}
		case "templates":
			t1 := imRandTemplate(r)
			t2 := imRandTemplate(r)
			globalBox := &imNamed{parts: []imPart{imTplPart("Box", imTypeArg(ib("int")), imIntArg(2+i%9))}}
			enclosedBox := &imNamed{parts: []imPart{
				imTplPart("Outer", imTypeArg(ib("char"))),
				imNamePart("Box"),
			}}
			m.params = []imType{globalBox, enclosedBox, t1, t2, ip(t1), ip(ic(t2)), irr(t1)}
		case "declarators":
			dim := 4 + i%12
			m.params = []imType{
				ia(imRandBuiltin(r), dim),
				ir(ia(imRandBuiltin(r), dim)),
				ip(ia(imRandBuiltin(r), dim)),
				ia(ip(imRandBuiltin(r)), dim),
				ifp(imRandBuiltin(r), imRandBuiltin(r), imRandBuiltin(r)),
				ip(ia(ia(imRandBuiltin(r), 3), dim)),
			}
		case "candidate_order":
			q := in("Cand", "Prefix", "Chain")
			m.params = []imType{q, ip(ic(q)), ic(ip(q)), ir(ip(q)), irr(q)}
			if i%2 == 0 {
				longParts := make([]string, 42)
				for j := range longParts {
					longParts[j] = fmt.Sprintf("L%02d", j)
				}
				long := in(longParts...)
				m.params = append(m.params, long, ip(long))
			}
		default: // reuse
			bases := []imType{imRandBase(r), imRandBase(r), imRandTemplate(r)}
			for _, b := range bases {
				m.params = append(m.params, b)
			}
			for j := 0; j < 5; j++ {
				b := bases[r.Intn(len(bases))]
				m.params = append(m.params, imVary(r, b), b)
			}
		}

		// Add a deterministic reuse tail to every group.  The tail refers to
		// an earlier complex parameter, so references are not merely attached
		// to an unreferenced final type.
		if len(m.params) > 0 && group != "reuse" {
			anchor := m.params[0]
			m.params = append(m.params, anchor, ip(anchor))
		}
		out = append(out, m)
	}
	return out
}

func imAllGroups() []string {
	return []string{"prefix_chain", "qualified_type", "templates", "declarators", "candidate_order", "reuse"}
}

// imFuzzModel selects a small model from short fuzz input.  A false result is
// a controlled rejection of an invalid selection, not a demangler skip.
func imFuzzModel(data []byte) (*imModel, int, bool) {
	if len(data) < 3 {
		return nil, 0, false
	}
	baseSel := int(data[0])
	wrapperSel := int(data[1])
	mode := int(data[2]) % 3
	if baseSel >= len(imFuzzBaseNodes) || wrapperSel >= len(imFuzzWrappers) {
		return nil, 0, false
	}
	base := imFuzzBaseNodes[baseSel]()
	if base == nil {
		return nil, 0, false
	}
	m := &imModel{id: baseSel*16 + wrapperSel, group: "fuzz", fn: "fz", result: ib("int")}
	wrapped := imFuzzWrappers[wrapperSel](base)
	if wrapped == nil {
		return nil, 0, false
	}
	// Base first registers structural candidates; the wrapper and repeated
	// base then have real slots to reference.
	m.params = []imType{base, wrapped, base, wrapped}
	return m, mode, true
}

var imFuzzBaseNodes = []func() imType{
	func() imType { return ib("int") },
	func() imType { return ib("char") },
	func() imType { return in("A", "B", "C") },
	func() imType { return in("ns", "deep", "Node") },
	func() imType {
		return &imNamed{parts: []imPart{imNamePart("ns"), imNamePart("Box"), imTplPart("Holder", imTypeArg(ib("int")))}}
	},
	func() imType {
		return &imNamed{parts: []imPart{imTplPart("Outer", imTypeArg(ib("int"))), imNamePart("Box")}}
	},
	func() imType { return ia(ib("double"), 8) },
	func() imType { return ifp(ib("int"), ib("char"), in("A", "B", "C")) },
}

var imFuzzWrappers = []func(imType) imType{
	func(t imType) imType { return t },
	func(t imType) imType { return ip(t) },
	func(t imType) imType { return ip(ic(t)) },
	func(t imType) imType { return ic(ip(t)) },
	func(t imType) imType { return ir(t) },
	func(t imType) imType { return irr(t) },
	func(t imType) imType { return ia(ip(t), 7) },
	func(t imType) imType { return ip(ia(t, 5)) },
}

func TestItaniumSubstitutionModel(t *testing.T) {
	for _, group := range imAllGroups() {
		group := group
		t.Run(group, func(t *testing.T) {
			models := imGenerateGroup(group, 36, 0x494D4142)
			if len(models) < 36 {
				t.Fatalf("generated %d cases", len(models))
			}
			for i, m := range models {
				if err := checkItaniumModel(m); err != nil {
					t.Fatalf("deterministic case group=%s index=%d seed=%d\n%v", group, i, m.id, err)
				}
			}

			// Stable hand-computed semantic cases used by mutation checks.
			switch group {
			case "prefix_chain":
				m := &imModel{fn: "f", group: group, params: []imType{in("A", "B", "C"), &imNamed{parts: []imPart{imNamePart("A"), imNamePart("B"), imNamePart("D")}}}}
				if err := checkItaniumModel(m); err != nil {
					t.Fatal(err)
				}
				want := "f(A::B::C, A::B::D)"
				sym, enc, err := m.symbol(imRefsAll, 0)
				if err != nil {
					t.Fatal(err)
				}
				if sym != "_Z1fN1A1B1CENS0_1DE" {
					t.Fatalf("unexpected prefix encoding %s table=%+v", sym, enc.table)
				}
				wantSlots := []string{
					"N(A)",
					"Q(N(A)::N(B))",
					"Q(Q(N(A)::N(B))::N(C))",
					"Q(Q(N(A)::N(B))::N(D))",
				}
				if len(enc.table) != len(wantSlots) {
					t.Fatalf("prefix table len=%d want %d: %+v", len(enc.table), len(wantSlots), enc.table)
				}
				for i, key := range wantSlots {
					if enc.table[i].slot != i || enc.table[i].key != key {
						t.Fatalf("prefix slot %d=%+v want key %s", i, enc.table[i], key)
					}
				}
				if got, _ := ToString(sym); got != want {
					t.Fatalf("%s\n%s", imFirstDiff(want, got), &imCheckFailure{model: m, kind: "hand-prefix", symbols: map[string]string{"refs": sym}, tables: map[string][]imCandidate{"refs": enc.table}})
				}
			case "candidate_order":
				m := &imModel{fn: "f", group: group, params: []imType{in("A", "B", "C"), &imNamed{parts: []imPart{imNamePart("A"), imNamePart("B"), imNamePart("D")}}}}
				if err := checkItaniumModel(m); err != nil {
					t.Fatal(err)
				}
				// Explicit S_ (slot 0), S0_ (index 1), and S1_ (index 2).
				handCandidates := []struct {
					code string
					tail string
					want string
				}{
					{"S_", "NS_1XE", "f(A::B::C, A::X)"},
					{"S0_", "NS0_1XE", "f(A::B::C, A::B::X)"},
					{"S1_", "S1_", "f(A::B::C, A::B::C)"},
				}
				for _, hc := range handCandidates {
					sym := "_Z1fN1A1B1CE" + hc.tail
					got, err := ToString(sym)
					if err != nil {
						t.Fatalf("hand candidate %s rejected: %v", hc.code, err)
					}
					if got != hc.want {
						t.Fatalf("hand candidate %s mismatch\n%s", hc.code, imFirstDiff(hc.want, got))
					}
				}
				// Build a table with more than 36 slots and exercise S10_.
				parts := make([]string, 42)
				for i := range parts {
					parts[i] = fmt.Sprintf("Q%02d", i)
				}
				long := in(parts...)
				big := &imModel{fn: "g", group: group, params: []imType{long, long}}
				if err := checkItaniumModel(big); err != nil {
					t.Fatal(err)
				}
				bigSym, bigEnc, err := big.symbol(imRefsAll, 0)
				if err != nil {
					t.Fatal(err)
				}
				if !strings.Contains(bigSym, "S14_") {
					t.Fatalf("expected multi-digit base36 reference S14_ in %s", bigSym)
				}
				if len(bigEnc.table) < 37 {
					t.Fatalf("expected at least 37 candidates, got %d", len(bigEnc.table))
				}
			case "reuse":
				imCheckReuseInterleaving(t, models)
			}
		})
	}
}

func imCheckReuseInterleaving(t *testing.T, models []*imModel) {
	t.Helper()
	bad := []string{
		"_Z1fN1A1B1C", // truncated nested name
		"_Z1fP",       // truncated pointer
		"_Z1fS50_",    // forward/out-of-range reference
		"_Z1fS_",      // empty table reference
		"_Z1fA10_",    // truncated array element
		"_Z1fPFii",    // missing function E
	}
	legal := []*imModel{
		{fn: "f", params: []imType{ic(ib("int")), ic(ip(ib("int")))}},
		{fn: "f", params: []imType{ip(ifn(ib("int"), ib("int"))), ip(ifn(ib("int"), ib("int")))}},
		models[0],
		models[5],
	}
	for round := 0; round < 4; round++ {
		for i, badSym := range bad {
			if _, err := ToAST(badSym); err == nil {
				t.Fatalf("round %d bad input %d was accepted: %s", round, i, badSym)
			}
			m := legal[(round+i)%len(legal)]
			if _, _, err := m.symbol(imRefsAll, int64(round*7+i)); err != nil {
				t.Fatal(err)
			}
			if err := checkItaniumModel(m); err != nil {
				t.Fatalf("legal symbol after bad input failed\n%v", err)
			}
		}
	}

	// Several selective equivalent encodings of one model must all survive
	// interleaving with truncations and forward references.
	m := &imModel{
		fn:     "z",
		params: []imType{in("Reuse", "Prefix", "Node"), ip(ic(in("Reuse", "Prefix", "Node"))), irr(in("Reuse", "Prefix", "Node"))},
	}
	for salt := int64(0); salt < 8; salt++ {
		enc, err := m.encode(imStrategy{mode: imRefsSelect, salt: salt})
		if err != nil {
			t.Fatal(err)
		}
		sym := "_Z" + sourceName(m.fn) + enc.body
		if _, err := ToString("_Z1fS99_"); err == nil {
			t.Fatal("forward reference unexpectedly accepted")
		}
		ast, err := ToAST(sym)
		if err != nil {
			t.Fatalf("selective encoding salt=%d rejected: %v\nops=%+v", salt, err, enc.ops)
		}
		if got := ASTToString(ast); got != m.wantDemangled() {
			t.Fatalf("selective encoding salt=%d mismatch\n%s", salt, imFirstDiff(m.wantDemangled(), got))
		}
	}
}

func FuzzItaniumSubstitution(f *testing.F) {
	seeds := [][]byte{
		{0, 1, 0},
		{2, 2, 1},
		{4, 5, 2},
		{5, 4, 0},
		{7, 3, 1},
		{3, 7, 2},
	}
	for _, seed := range seeds {
		f.Add(seed)
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		m, mode, ok := imFuzzModel(data)
		if !ok {
			return // controlled rejection of invalid short model selection
		}
		if mode == imRefsSelect {
			enc, err := m.encode(imStrategy{mode: imRefsSelect, salt: int64(data[2])})
			if err != nil {
				t.Fatal(err)
			}
			sym := "_Z" + sourceName(m.fn) + enc.body
			if _, err := ToAST(sym); err != nil {
				t.Fatalf("selected legal model encoding rejected: %v", err)
			}
		}
		if err := checkItaniumModel(m); err != nil {
			t.Fatal(err)
		}
	})
}
