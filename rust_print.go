// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package demangle

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// rustPrinter renders a RustAST to a string. All state is local to one
// RustASTToString call, so repeated calls never share lifetime scopes,
// truncation state or cycle-detection marks.
type rustPrinter struct {
	// noGenericArgs implements NoTemplateParams: the graph is still
	// walked in full, but I-path argument lists render as empty angle
	// brackets. Dyn-trait associated bindings are not affected.
	noGenericArgs bool

	max int

	// lifetimes is the number of bound lifetimes currently in scope.
	// It follows the binder nesting of the encoding and is adjusted
	// without mutating any node.
	lifetimes int64

	// last is the last byte appended, mirroring the legacy printer's
	// spacing decisions around dyn-trait lifetimes.
	last byte

	buf strings.Builder

	// visiting detects cycles in hand-built or modified graphs.
	visiting map[*RustNode]bool
}

// printRustAST validates and renders ast.
func printRustAST(ast *RustAST, options []Option) (ret string, err error) {
	// Parsed graphs cannot reach this panic; it only guards hand-built
	// graphs whose lifetime indices exceed their binder scopes.
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(rustLifetimeError); ok {
				ret = ""
				err = errors.New("demangle: invalid lifetime in RustAST")
				return
			}
			panic(r)
		}
	}()

	pr := &rustPrinter{
		visiting: make(map[*RustNode]bool),
	}
	llvmStyle := false
	for _, o := range options {
		switch {
		case o == NoTemplateParams:
			pr.noGenericArgs = true
		case isMaxLength(o):
			pr.max = maxLength(o)
		case o == LLVMStyle:
			llvmStyle = true
		}
	}

	if ast.Root == nil {
		return "", errors.New("demangle: RustAST is missing its root path")
	}
	if err := pr.printPath(ast.Root, true); err != nil {
		return "", err
	}

	if ast.Suffix != "" && llvmStyle {
		pr.writeString(" (")
		pr.writeString(ast.Suffix)
		pr.writeByte(')')
	}

	s := pr.buf.String()
	if pr.max > 0 && len(s) > pr.max {
		// The whole graph has been walked and validated above; the
		// option only shortens the returned text.
		s = s[:pr.max]
	}
	return s, nil
}

// writeByte appends one byte and records it as the last written byte.
func (pr *rustPrinter) writeByte(c byte) {
	pr.last = c
	pr.buf.WriteByte(c)
}

// writeString appends s and records its final byte.
func (pr *rustPrinter) writeString(s string) {
	if len(s) > 0 {
		pr.last = s[len(s)-1]
	}
	pr.buf.WriteString(s)
}

// enter marks n as being visited, rejecting cycles.
func (pr *rustPrinter) enter(n *RustNode) error {
	if n == nil {
		return errors.New("demangle: RustAST contains a nil node")
	}
	if pr.visiting[n] {
		return fmt.Errorf("demangle: cycle detected at %s node", n.Kind)
	}
	pr.visiting[n] = true
	return nil
}

// leave clears the visit mark so shared subtrees remain walkable.
func (pr *rustPrinter) leave(n *RustNode) {
	delete(pr.visiting, n)
}

// skipNode walks a subtree without printing it, still checking refs and
// cycles. It keeps hidden productions (impl paths, suppressed generic
// arguments) part of the printer's structural validation.
func (pr *rustPrinter) skipNode(n *RustNode) error {
	if n.Kind == RustNodeBackref {
		// Resolve first so a hand-built back-reference cycle is
		// reported as an error instead of overflowing the stack.
		target, err := resolveBackref(n)
		if err != nil {
			return err
		}
		return pr.skipNode(target)
	}
	if err := pr.enter(n); err != nil {
		return err
	}
	defer pr.leave(n)
	for _, ch := range n.Children {
		if err := pr.skipNode(ch); err != nil {
			return err
		}
	}
	return nil
}

// resolveBackref follows back-reference links to the referenced node.
func resolveBackref(n *RustNode) (*RustNode, error) {
	seen := make(map[*RustNode]bool)
	for n != nil && n.Kind == RustNodeBackref {
		if seen[n] {
			return nil, errors.New("demangle: cycle detected in back-reference")
		}
		seen[n] = true
		n = n.Ref
	}
	if n == nil {
		return nil, errors.New("demangle: back-reference node has no target")
	}
	return n, nil
}

// printPath renders a path node. needsSeparator mirrors the legacy
// parser's flag: only the symbol's main path writes "::" before its
// generic arguments.
func (pr *rustPrinter) printPath(n *RustNode, needsSeparator bool) error {
	if n.Kind == RustNodeBackref {
		// Back-references are not structural edges: resolve and print
		// the shared target without touching the cycle-visit set.
		target, err := resolveBackref(n)
		if err != nil {
			return err
		}
		return pr.printPath(target, needsSeparator)
	}

	if err := pr.enter(n); err != nil {
		return err
	}
	defer pr.leave(n)

	if n.Kind != RustNodePath {
		return fmt.Errorf("demangle: expected path node, got %q", n.Kind)
	}

	switch n.Value {
	case "crate":
		if len(n.Children) != 1 {
			return errors.New("demangle: crate path requires one identifier")
		}
		return pr.printIdentifier(n.Children[0])
	case "inherent-impl":
		if len(n.Children) != 2 {
			return errors.New("demangle: malformed inherent-impl path")
		}
		return pr.printImplPath(n, false)
	case "trait-impl":
		if len(n.Children) != 3 {
			return errors.New("demangle: malformed trait-impl path")
		}
		return pr.printImplPath(n, true)
	case "trait-definition":
		if len(n.Children) != 2 {
			return errors.New("demangle: malformed trait-definition path")
		}
		pr.writeByte('<')
		if err := pr.printType(n.Children[0]); err != nil {
			return err
		}
		pr.writeString(" as ")
		if err := pr.printPath(n.Children[1], false); err != nil {
			return err
		}
		pr.writeByte('>')
		return nil
	case "":
		if len(n.Children) == 0 {
			return errors.New("demangle: path node has no children")
		}
		switch n.Children[0].Kind {
		case RustNodeNamespace:
			return pr.printNestedPath(n, needsSeparator)
		case RustNodePath, RustNodeBackref:
			return pr.printGenericPath(n, needsSeparator)
		default:
			return fmt.Errorf("demangle: unexpected path child %q", n.Children[0].Kind)
		}
	default:
		return fmt.Errorf("demangle: unrecognized path variant %q", n.Value)
	}
}

// printImplPath renders an M or X path. The hidden impl-path child is
// walked for validation but never printed.
func (pr *rustPrinter) printImplPath(n *RustNode, isTrait bool) error {
	if n.Children[0].Kind != RustNodeImplPath {
		return errors.New("demangle: impl path missing its impl-path child")
	}
	if err := pr.skipNode(n.Children[0]); err != nil {
		return err
	}
	pr.writeByte('<')
	if err := pr.printType(n.Children[1]); err != nil {
		return err
	}
	if isTrait {
		pr.writeString(" as ")
		if err := pr.printPath(n.Children[2], false); err != nil {
			return err
		}
	}
	pr.writeByte('>')
	return nil
}

// printNestedPath renders an N path: <namespace> <path> <identifier>.
func (pr *rustPrinter) printNestedPath(n *RustNode, needsSeparator bool) error {
	if len(n.Children) != 3 {
		return errors.New("demangle: nested path requires namespace, path and identifier")
	}
	ns, parent, ident := n.Children[0], n.Children[1], n.Children[2]
	if ns.Kind != RustNodeNamespace || len(ns.Value) != 1 {
		return errors.New("demangle: malformed namespace node")
	}
	if err := pr.printPath(parent, needsSeparator); err != nil {
		return err
	}
	if err := pr.checkKind(ident, RustNodeIdentifier); err != nil {
		return err
	}

	letter := ns.Value[0]
	if letter >= 'A' && letter <= 'Z' {
		pr.writeString("::{")
		switch letter {
		case 'C':
			pr.writeString("closure")
		case 'S':
			pr.writeString("shim")
		default:
			pr.writeByte(letter)
		}
		if ident.Value != "" {
			pr.writeByte(':')
			pr.writeString(ident.Value)
		}
		dis, err := pr.identifierDisambiguator(ident)
		if err != nil {
			return err
		}
		pr.writeByte('#')
		pr.writeString(strconv.FormatInt(dis, 10))
		pr.writeByte('}')
	} else {
		pr.writeString("::")
		pr.writeString(ident.Value)
	}
	return nil
}

// identifierDisambiguator returns the decoded disambiguator of an
// identifier node, defaulting to zero.
func (pr *rustPrinter) identifierDisambiguator(n *RustNode) (int64, error) {
	for _, ch := range n.Children {
		if ch.Kind == RustNodeDisambiguator {
			v, err := strconv.ParseInt(ch.Value, 10, 64)
			if err != nil {
				return 0, fmt.Errorf("demangle: malformed disambiguator: %v", err)
			}
			return v, nil
		}
	}
	return 0, nil
}

// printGenericPath renders an I path: <path> "::"? "<" args ">".
func (pr *rustPrinter) printGenericPath(n *RustNode, needsSeparator bool) error {
	if len(n.Children) != 2 || n.Children[1].Kind != RustNodeGenericArgs {
		return errors.New("demangle: malformed generic path")
	}
	if err := pr.printPath(n.Children[0], needsSeparator); err != nil {
		return err
	}
	if needsSeparator {
		pr.writeString("::")
	}
	pr.writeByte('<')
	args := n.Children[1]
	if pr.noGenericArgs {
		// Printing is suppressed, but the arguments are still walked
		// so a broken graph cannot hide behind the option.
		if err := pr.skipNode(args); err != nil {
			return err
		}
	} else if err := pr.printGenericArgs(args); err != nil {
		return err
	}
	pr.writeByte('>')
	return nil
}

// printGenericArgs renders the arguments of an I path.
func (pr *rustPrinter) printGenericArgs(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeGenericArgs); err != nil {
		return err
	}
	for i, arg := range n.Children {
		if i > 0 {
			pr.writeString(", ")
		}
		if err := pr.printGenericArg(arg); err != nil {
			return err
		}
	}
	return nil
}

// printGenericArg renders one lifetime, type or const argument.
func (pr *rustPrinter) printGenericArg(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeGenericArg); err != nil {
		return err
	}
	if len(n.Children) != 1 {
		return errors.New("demangle: malformed generic argument")
	}
	ch := n.Children[0]
	// A back-reference keeps its occurrence node, but the rendering
	// role is determined by its target's kind.
	if ch.Kind == RustNodeBackref {
		target, err := resolveBackref(ch)
		if err != nil {
			return err
		}
		ch = target
	}
	switch ch.Kind {
	case RustNodeLifetime:
		return pr.printLifetime(n.Children[0])
	case RustNodeConst:
		return pr.printConst(n.Children[0])
	default:
		return pr.printType(n.Children[0])
	}
}

// printIdentifier renders a decoded identifier.
func (pr *rustPrinter) printIdentifier(n *RustNode) error {
	if err := pr.enter(n); err != nil {
		return err
	}
	defer pr.leave(n)
	if n.Kind != RustNodeIdentifier {
		return fmt.Errorf("demangle: expected identifier node, got %q", n.Kind)
	}
	pr.writeString(n.Value)
	return nil
}

// checkKind fails unless n has the expected kind.
func (pr *rustPrinter) checkKind(n *RustNode, kind string) error {
	if n == nil {
		return fmt.Errorf("demangle: expected %s node, got nil", kind)
	}
	if n.Kind != kind {
		return fmt.Errorf("demangle: expected %s node, got %q", kind, n.Kind)
	}
	return nil
}

// printType renders a type node.
func (pr *rustPrinter) printType(n *RustNode) error {
	if n.Kind == RustNodeBackref {
		target, err := resolveBackref(n)
		if err != nil {
			return err
		}
		return pr.printType(target)
	}
	// A named type is represented directly by its path node.
	if n.Kind == RustNodePath {
		return pr.printPath(n, false)
	}

	if err := pr.enter(n); err != nil {
		return err
	}
	defer pr.leave(n)

	if n.Kind != RustNodeType {
		return fmt.Errorf("demangle: expected type node, got %q", n.Kind)
	}

	// Basic types carry their spelling directly and have no children.
	// An empty tuple also has no children but renders as "()", so it is
	// dispatched through the tuple case below instead.
	if len(n.Children) == 0 && n.Value != "tuple" {
		if n.Value == "" {
			return errors.New("demangle: type node has no content")
		}
		pr.writeString(n.Value)
		return nil
	}

	switch n.Value {
	case "array":
		if len(n.Children) != 2 {
			return errors.New("demangle: malformed array type")
		}
		pr.writeByte('[')
		if err := pr.printType(n.Children[0]); err != nil {
			return err
		}
		pr.writeString("; ")
		if err := pr.printConst(n.Children[1]); err != nil {
			return err
		}
		pr.writeByte(']')
	case "slice":
		if len(n.Children) != 1 {
			return errors.New("demangle: malformed slice type")
		}
		pr.writeByte('[')
		if err := pr.printType(n.Children[0]); err != nil {
			return err
		}
		pr.writeByte(']')
	case "tuple":
		pr.writeByte('(')
		for i, ch := range n.Children {
			if i > 0 {
				pr.writeString(", ")
			}
			if err := pr.printType(ch); err != nil {
				return err
			}
		}
		if len(n.Children) == 1 {
			pr.writeByte(',')
		}
		pr.writeByte(')')
	case "ref", "ref-mut":
		pr.writeByte('&')
		next := 0
		if len(n.Children) > 0 && n.Children[0].Kind == RustNodeLifetime {
			idx, err := pr.lifetimeIndex(n.Children[0])
			if err != nil {
				return err
			}
			if idx > 0 {
				// A zero index is elided entirely, including the space.
				pr.writeLifetimeName(idx)
				pr.writeByte(' ')
			}
			next = 1
		}
		if n.Value == "ref-mut" {
			pr.writeString("mut ")
		}
		if len(n.Children) != next+1 {
			return errors.New("demangle: malformed reference type")
		}
		if err := pr.printType(n.Children[next]); err != nil {
			return err
		}
	case "ptr-const":
		pr.writeString("*const ")
		if len(n.Children) != 1 {
			return errors.New("demangle: malformed pointer type")
		}
		if err := pr.printType(n.Children[0]); err != nil {
			return err
		}
	case "ptr-mut":
		pr.writeString("*mut ")
		if len(n.Children) != 1 {
			return errors.New("demangle: malformed pointer type")
		}
		if err := pr.printType(n.Children[0]); err != nil {
			return err
		}
	case "fn":
		if len(n.Children) != 1 {
			return errors.New("demangle: malformed function type")
		}
		if err := pr.printFnSig(n.Children[0]); err != nil {
			return err
		}
	case "dyn":
		if len(n.Children) != 2 || n.Children[1].Kind != RustNodeLifetime {
			return errors.New("demangle: malformed dyn type")
		}
		if err := pr.printDynBounds(n.Children[0]); err != nil {
			return err
		}
		idx, err := pr.lifetimeIndex(n.Children[1])
		if err != nil {
			return err
		}
		if idx > 0 {
			if pr.last != ' ' {
				pr.writeByte(' ')
			}
			pr.writeString("+ ")
			pr.writeLifetimeName(idx)
		}
	default:
		return fmt.Errorf("demangle: unrecognized type variant %q", n.Value)
	}
	return nil
}

// printFnSig renders an F signature. Binder-introduced lifetimes are
// scoped to the signature, matching the legacy save/restore behavior.
func (pr *rustPrinter) printFnSig(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeFnSig); err != nil {
		return err
	}
	hold := pr.lifetimes
	defer func() { pr.lifetimes = hold }()

	var (
		binder *RustNode
		abi    *RustNode
		params []*RustNode
		ret    *RustNode
		hasRet bool
	)
	for _, ch := range n.Children {
		switch ch.Kind {
		case RustNodeBinder:
			binder = ch
		case RustNodeABI:
			abi = ch
		case RustNodeReturnType:
			if len(ch.Children) != 1 {
				return errors.New("demangle: malformed return type")
			}
			ret, hasRet = ch.Children[0], true
		default:
			params = append(params, ch)
		}
	}

	// Order mirrors the legacy printer: for<...>, unsafe, extern, fn.
	if binder != nil {
		if err := pr.printBinder(binder); err != nil {
			return err
		}
	}
	if n.Value == "unsafe" {
		pr.writeString("unsafe ")
	}
	if abi != nil {
		if err := pr.printABI(abi); err != nil {
			return err
		}
	}
	pr.writeString("fn(")
	for i, p := range params {
		if i > 0 {
			pr.writeString(", ")
		}
		if err := pr.printType(p); err != nil {
			return err
		}
	}
	pr.writeByte(')')
	if hasRet {
		pr.writeString(" -> ")
		if err := pr.printType(ret); err != nil {
			return err
		}
	}
	return nil
}

// printBinder renders "for<...> " and extends the lifetime scope.
func (pr *rustPrinter) printBinder(n *RustNode) error {
	count, err := strconv.ParseInt(n.Value, 10, 64)
	if err != nil || count <= 0 {
		return fmt.Errorf("demangle: malformed binder: %q", n.Value)
	}
	pr.writeString("for<")
	for i := int64(0); i < count; i++ {
		if i > 0 {
			pr.writeString(", ")
		}
		pr.lifetimes++
		pr.writeLifetimeName(1)
	}
	pr.writeString("> ")
	return nil
}

// printABI renders an extern ABI.
func (pr *rustPrinter) printABI(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeABI); err != nil {
		return err
	}
	if n.Value == "" {
		return errors.New("demangle: malformed ABI")
	}
	pr.writeString(`extern "`)
	pr.writeString(n.Value)
	pr.writeString(`" `)
	return nil
}

// printDynBounds renders a dyn bound list. Its binder is scoped to
// the list; the enclosing D type's trailing lifetime is printed by
// printType after the scope is restored.
func (pr *rustPrinter) printDynBounds(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeDynBounds); err != nil {
		return err
	}
	hold := pr.lifetimes
	defer func() { pr.lifetimes = hold }()

	pr.writeString("dyn ")
	first := true
	for _, ch := range n.Children {
		switch ch.Kind {
		case RustNodeBinder:
			if err := pr.printBinder(ch); err != nil {
				return err
			}
		case RustNodeDynTrait:
			if !first {
				pr.writeString(" + ")
			}
			first = false
			if err := pr.printDynTrait(ch); err != nil {
				return err
			}
		default:
			return fmt.Errorf("demangle: unexpected dyn-bounds child %q", ch.Kind)
		}
	}
	return nil
}

// printDynTrait renders a trait path plus associated-item bindings.
func (pr *rustPrinter) printDynTrait(n *RustNode) error {
	if err := pr.checkKind(n, RustNodeDynTrait); err != nil {
		return err
	}
	if len(n.Children) < 1 {
		return errors.New("demangle: dyn trait has no path")
	}

	// An I path leaves its angle bracket open so the associated-item
	// bindings share it; only one closing ">" is ever written.
	started, err := pr.printDynTraitPath(n.Children[0])
	if err != nil {
		return err
	}
	for _, b := range n.Children[1:] {
		if b.Kind != RustNodeAssocBinding || len(b.Children) != 2 {
			return errors.New("demangle: malformed associated-item binding")
		}
		if started {
			pr.writeString(", ")
		} else {
			pr.writeByte('<')
			started = true
		}
		id, typ := b.Children[0], b.Children[1]
		if err := pr.printIdentifier(id); err != nil {
			return err
		}
		pr.writeString(" = ")
		if err := pr.printType(typ); err != nil {
			return err
		}
	}
	if started {
		pr.writeByte('>')
	}
	return nil
}

// printDynTraitPath renders a dyn-trait path and reports whether an
// angle bracket is currently open awaiting associated-item bindings.
func (pr *rustPrinter) printDynTraitPath(n *RustNode) (bool, error) {
	if n.Kind == RustNodeBackref {
		target, err := resolveBackref(n)
		if err != nil {
			return false, err
		}
		return pr.printDynTraitPath(target)
	}
	if n.Kind != RustNodePath {
		return false, fmt.Errorf("demangle: expected dyn-trait path, got %q", n.Kind)
	}

	// An I path is a generic-args node as its second child.
	if n.Value == "" && len(n.Children) == 2 &&
		n.Children[1].Kind == RustNodeGenericArgs {
		if err := pr.printPath(n.Children[0], false); err != nil {
			return false, err
		}
		pr.writeByte('<')
		if pr.noGenericArgs {
			if err := pr.skipNode(n.Children[1]); err != nil {
				return false, err
			}
		} else if err := pr.printGenericArgs(n.Children[1]); err != nil {
			return false, err
		}
		return true, nil
	}

	if err := pr.printPath(n, false); err != nil {
		return false, err
	}
	return false, nil
}

// printConst renders a constant.
func (pr *rustPrinter) printConst(n *RustNode) error {
	if n.Kind == RustNodeBackref {
		target, err := resolveBackref(n)
		if err != nil {
			return err
		}
		return pr.printConst(target)
	}

	if err := pr.enter(n); err != nil {
		return err
	}
	defer pr.leave(n)

	if n.Kind != RustNodeConst {
		return fmt.Errorf("demangle: expected const node, got %q", n.Kind)
	}
	if n.Value == "placeholder" {
		pr.writeByte('_')
		return nil
	}
	if len(n.Children) != 2 || n.Children[1].Kind != RustNodeConstData {
		return errors.New("demangle: malformed constant")
	}
	pr.writeString(n.Children[1].Value)
	return nil
}

// lifetimeIndex parses the raw index stored on a lifetime node.
func (pr *rustPrinter) lifetimeIndex(n *RustNode) (int64, error) {
	if n.Kind != RustNodeLifetime {
		return 0, fmt.Errorf("demangle: expected lifetime node, got %q", n.Kind)
	}
	idx, err := strconv.ParseInt(n.Value, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("demangle: malformed lifetime index: %v", err)
	}
	return idx, nil
}

// printLifetime renders a lifetime in a position where even index 0
// is visible (generic arguments).
func (pr *rustPrinter) printLifetime(n *RustNode) error {
	idx, err := pr.lifetimeIndex(n)
	if err != nil {
		return err
	}
	pr.writeLifetimeName(idx)
	return nil
}

// writeLifetimeName renders one lifetime index relative to the current
// binder scope. Names are recomputed on every traversal, so a shared
// subtree never reuses a name computed under another binder.
func (pr *rustPrinter) writeLifetimeName(lifetime int64) {
	pr.writeByte('\'')
	if lifetime == 0 {
		pr.writeByte('_')
		return
	}
	depth := pr.lifetimes - lifetime
	if depth < 0 {
		// Surface the same condition the legacy parser rejected.
		panic(rustLifetimeError{})
	}
	if depth < 26 {
		pr.writeByte('a' + byte(depth))
	} else {
		pr.writeByte('z')
		pr.writeString(strconv.FormatInt(depth-26+1, 10))
	}
}

// rustLifetimeError turns an out-of-scope lifetime into an error at
// the print boundary.
type rustLifetimeError struct{}
