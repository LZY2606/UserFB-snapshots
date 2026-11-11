// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package demangle

import (
	"errors"
	"strings"
)

// Rust node kinds. Every value returned in RustNode.Kind is one of
// these constants.
//
// The required kinds are:
//
//   - RustNodeIdentifier: a decoded identifier (crate name, path
//     segment, namespace name, ABI name, dyn-trait associated item).
//     Value holds the human-readable identifier (punycode already
//     expanded); Span covers the whole identifier encoding, including
//     the length prefix.
//   - RustNodePath: a <path> production (crate root, nested path,
//     generic path, impl path, trait path).
//   - RustNodeType: a <type> production, including basic types,
//     references, pointers, tuples, arrays, slices, function signatures
//     and dyn trait objects.
//   - RustNodeConst: a <const> production (a const generic value or a
//     placeholder).
//   - RustNodeBinder: a "G" binder. Value is the number of bound
//     lifetimes introduced by the binder, encoded as a decimal string.
//   - RustNodeBackref: a "B" back-reference. The node itself carries
//     the occurrence Span; Ref points at the single shared target
//     node. Back-reference nodes have no children of their own.
//
// The remaining kinds distinguish productions that the printer needs
// to tell apart but that do not fit the six categories above; they are
// part of the public contract:
//
//   - RustNodeGenericArgs / RustNodeGenericArg: the argument list of
//     an "I" path and one argument (lifetime, type or const).
//   - RustNodeLifetime: an "L" lifetime. Value is the raw lifetime
//     index as a decimal string; the printable name ('a, 'b, ...) is
//     recovered by the printer from the surrounding binders.
//   - RustNodeFnSig: an "F" function signature.
//   - RustNodeDynBounds: a "D" trait object's bound list.
//   - RustNodeDynTrait: one trait in a dyn bound, with optional
//     associated-item bindings as children.
//   - RustNodeAssocBinding: a "p" dyn-trait associated-item binding.
//   - RustNodeNamespace: the namespace letter of an "N" path. Value is
//     the single letter; the disambiguator, when present, is carried by
//     a RustNodeDisambiguator child.
//   - RustNodeDisambiguator: an "s" disambiguator. Value is the
//     decoded disambiguator as a decimal string.
//   - RustNodeConstData: the numeric/boolean/character payload of a
//     const, kept separate from its type. Value is the fully decoded
//     payload ("true", "'a'", "0x100...", ...).
//   - RustNodeImplPath: the hidden [<disambiguator>] <path> prefix of
//     an M/X impl path. It is parsed and checked but never printed.
//   - RustNodeABI: the ABI of an F function signature; Value is the
//     ABI string ("C", "cdecl", ...).
//   - RustNodeReturnType: the wrapper around an F signature's return
//     type. A unit ("u") return produces no node, matching the legacy
//     output which writes nothing after ")".
const (
	// Required kinds.
	RustNodeIdentifier = "identifier"
	RustNodePath       = "path"
	RustNodeType       = "type"
	RustNodeConst      = "const"
	RustNodeBinder     = "binder"
	RustNodeBackref    = "backref"

	// Extended kinds.
	RustNodeGenericArgs   = "generic-args"
	RustNodeGenericArg    = "generic-arg"
	RustNodeLifetime      = "lifetime"
	RustNodeFnSig         = "fn-sig"
	RustNodeDynBounds     = "dyn-bounds"
	RustNodeDynTrait      = "dyn-trait"
	RustNodeAssocBinding  = "assoc-binding"
	RustNodeNamespace     = "namespace"
	RustNodeDisambiguator = "disambiguator"
	RustNodeConstData     = "const-data"
	RustNodeImplPath      = "impl-path"
	RustNodeABI           = "abi"
	RustNodeReturnType    = "return-type"
)

// RustNode is one node in the Rust v0 syntax graph returned by
// ToRustAST.
//
// The graph is shared, not a tree: a back-reference node's Ref field
// points at the same node instance as every other reference to the
// same target. Programs that walk the graph must therefore guard
// against cycles themselves when working with hand-built or modified
// ASTs; RustASTToString does so and returns an error instead of
// overflowing the stack.
type RustNode struct {
	// Kind classifies the node; it is one of the RustNode* constants.
	Kind string

	// Value holds decoded, human-readable content. For identifier
	// nodes it is the readable identifier; for lifetime nodes it is
	// the raw lifetime index; for binder nodes it is the number of
	// bound lifetimes; nodes whose content is fully expressed by
	// their children may leave Value empty.
	Value string

	// Span is the half-open byte interval [Span[0], Span[1]) of the
	// node's encoding in the complete input string passed to
	// ToRustAST. Offsets are zero-based. A child's span is contained
	// within its parent's. Identifier spans include the length prefix
	// and any optional underscore, not only the identifier bytes.
	// Back-reference spans cover the "B" encoding and its number.
	Span [2]int

	// Children are the node's syntactic children. Back-reference
	// nodes never have children: their target is reached through Ref.
	Children []*RustNode

	// Ref is set only on back-reference nodes and points at the
	// unique target node for the referenced encoding. Multiple
	// references to the same target share the same pointer.
	Ref *RustNode
}

// RustAST is the syntax graph of a Rust v0 mangled symbol.
type RustAST struct {
	// Root is the mandatory main <path> of the symbol.
	Root *RustNode

	// InstantiatingCrate, when non-nil, is the optional
	// <instantiating-crate> path. It never appears in the printed
	// main path name.
	InstantiatingCrate *RustNode

	// Suffix is the optional dot suffix of the mangled name
	// (for example ".llvm.9D1C9369"), including the leading dot.
	// It is empty when the symbol has no suffix.
	Suffix string
}

// ToRustAST parses a Rust v0 mangled symbol name (a string beginning
// with "_R") into a shared syntax graph.
//
// The complete encoding is validated, including hidden generic
// arguments, the optional instantiating crate and every back-reference:
// forward references, self references, references landing inside an
// identifier's length/bytes, and truncated or otherwise malformed
// encodings are rejected with an error whose message locates the
// offending byte offset. The NoTemplateParams option does not affect
// parsing; it only changes printing.
//
// The input string is not modified. If name does not start with "_R"
// the returned error is ErrNotMangledName.
func ToRustAST(name string) (*RustAST, error) {
	if !strings.HasPrefix(name, "_R") {
		return nil, ErrNotMangledName
	}
	return parseRustAST(name)
}

// RustASTToString renders the syntax graph ast back to the same
// human-readable string that ToString produces for Rust v0 symbols.
//
// Rendering works from the decoded node contents only: renaming an
// identifier node's Value changes both the direct output and every
// back-reference that points at it. Binder-relative lifetime names are
// recomputed for each traversal and are never cached on the nodes, so
// a shared subtree reached under different binder scopes is named
// correctly each time.
//
// The supported options are NoTemplateParams, LLVMStyle and
// MaxLength. With MaxLength the output may be truncated, but the whole
// graph is still walked and validated first. Hand-built or modified
// graphs that contain cycles or nodes missing required children
// produce an error rather than a crash.
func RustASTToString(ast *RustAST, options ...Option) (string, error) {
	if ast == nil {
		return "", errors.New("demangle: nil RustAST")
	}
	return printRustAST(ast, options)
}

// Compatibility boundaries.
//
// ToRustAST and RustASTToString apply only to the Rust v0 scheme:
// strings beginning with "_R". Legacy "_ZN" Rust symbols are demangled
// by an unchanged code path, and the C++ demangler (ToAST/ASTToString)
// is untouched as well. No new command-line program is added.
//
// For every valid v0 symbol, RustASTToString with no options produces
// exactly the string ToString produced before this API existed.
// NoTemplateParams, LLVMStyle and MaxLength keep their old meaning.
// MaxLength only shortens the returned text; the full graph is built
// and checked first, so a malformed symbol still returns an error
// rather than a truncated success.
//
// The graph returned by ToRustAST is a directed acyclic graph, not a
// tree: back-references share the node they point at. Because the
// structs are exported, callers can mutate them into shapes the parser
// never produces (cycles, missing children). Such graphs make
// RustASTToString return an error; the printer never follows a cycle
// into a stack overflow. Each call uses fresh printer state, so
// lifetime scopes and truncation state never carry over between calls,
// and the input string passed to ToRustAST is never modified.

// rustToString demangles a Rust v0 symbol by parsing it into the
// shared syntax graph and printing that graph. ToString and Filter
// use this single parse/print implementation for every "_R" symbol.
func rustToString(name string, options []Option) (string, error) {
	ast, err := ToRustAST(name)
	if err != nil {
		return "", err
	}
	return RustASTToString(ast, options...)
}
