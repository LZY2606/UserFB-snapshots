package compiler

import (
	"github.com/expr-lang/expr/ast"
	"github.com/expr-lang/expr/file"
	. "github.com/expr-lang/expr/vm"
	"github.com/expr-lang/expr/vm/runtime"
)

// This file tracks the provenance of values during compilation: the logical
// access path (from the environment root) that leads to the receiver of
// every static field access. The recorded paths are stored in the program's
// capsule metadata (see vm/capsule.go) so that EncodeProgram/DecodeProgram
// can rebind field accesses to a different environment layout without
// relying on struct field indexes, Go type names or a global field table.
//
// Provenance tracking is a side activity: it never changes the emitted
// bytecode, and any expression shape it does not understand simply yields an
// "unknown" provenance, which EncodeProgram reports as a precise error
// instead of guessing.

// prov is the provenance of a value: a logical access path from the
// environment root, or unknown.
type prov struct {
	steps   []ProvStep
	unknown bool
}

// provScope describes the provenance of `#` and `#acc` inside the predicate
// currently being compiled.
type provScope struct {
	elem   prov
	acc    prov
	hasAcc bool
}

func (c *compiler) pushProvScope(s provScope) {
	c.provScopes = append(c.provScopes, s)
}

func (c *compiler) popProvScope() {
	c.provScopes = c.provScopes[:len(c.provScopes)-1]
}

// pushElemScope pushes the predicate scope of a builtin iterating over coll.
func (c *compiler) pushElemScope(coll ast.Node) {
	c.pushProvScope(provScope{elem: elemProvOf(c.provOf(coll))})
}

// elemProvOf returns the provenance of the elements of a collection.
func elemProvOf(p prov) prov {
	if p.unknown {
		return p
	}
	return appendProvStep(p, ProvStep{Kind: ProvElem})
}

func appendProvStep(p prov, step ProvStep) prov {
	steps := make([]ProvStep, len(p.steps), len(p.steps)+1)
	copy(steps, p.steps)
	return prov{steps: append(steps, step)}
}

func unknownProv() prov {
	return prov{unknown: true}
}

func provEqual(a, b prov) bool {
	if a.unknown || b.unknown {
		return false
	}
	if len(a.steps) != len(b.steps) {
		return false
	}
	for i := range a.steps {
		if a.steps[i] != b.steps[i] {
			return false
		}
	}
	return true
}

// provOf computes the provenance of an expression. It is a pure function of
// the AST and the current let-variable and predicate scopes; it never emits
// bytecode and never fails: unsupported shapes yield an unknown provenance.
func (c *compiler) provOf(node ast.Node) prov {
	switch n := node.(type) {
	case *ast.IdentifierNode:
		for i := len(c.scopes) - 1; i >= 0; i-- {
			if c.scopes[i].variableName == n.Value {
				return c.scopes[i].prov
			}
		}
		if n.Value == "$env" {
			return prov{}
		}
		return prov{steps: []ProvStep{{Kind: ProvField, Name: n.Value}}}

	case *ast.MemberNode:
		if prop, ok := n.Property.(*ast.StringNode); ok {
			base := c.provOf(n.Node)
			if base.unknown {
				return base
			}
			return appendProvStep(base, ProvStep{Kind: ProvField, Name: prop.Value})
		}
		// Index access into a collection: a[0], a[i].
		return elemProvOf(c.provOf(n.Node))

	case *ast.SliceNode:
		// A slice of a collection has the same provenance.
		return c.provOf(n.Node)

	case *ast.ChainNode:
		return c.provOf(n.Node)

	case *ast.PointerNode:
		if len(c.provScopes) > 0 {
			s := c.provScopes[len(c.provScopes)-1]
			switch n.Name {
			case "":
				return s.elem
			case "acc":
				if s.hasAcc {
					return s.acc
				}
			}
		}

	case *ast.PredicateNode:
		return c.provOf(n.Node)

	case *ast.BuiltinNode:
		switch n.Name {
		case "filter":
			if len(n.Arguments) >= 1 {
				coll := c.provOf(n.Arguments[0])
				if coll.unknown {
					return coll
				}
				if n.Map != nil {
					mapped := c.provOfWithElem(n.Map, elemProvOf(coll))
					if !mapped.unknown {
						return appendProvStep(mapped, ProvStep{Kind: ProvArrayOf})
					}
					return mapped
				}
				return appendProvStep(elemProvOf(coll), ProvStep{Kind: ProvArrayOf})
			}
		case "map":
			if len(n.Arguments) == 2 {
				elem := elemProvOf(c.provOf(n.Arguments[0]))
				if elem.unknown {
					return elem
				}
				mapped := c.provOfWithElem(n.Arguments[1], elem)
				if !mapped.unknown {
					return appendProvStep(mapped, ProvStep{Kind: ProvArrayOf})
				}
				return mapped
			}
		case "reduce":
			if len(n.Arguments) == 3 {
				return c.provOf(n.Arguments[2])
			}
			if len(n.Arguments) >= 1 {
				return elemProvOf(c.provOf(n.Arguments[0]))
			}
		case "find", "findLast", "first", "last":
			// These return an element of the collection (or nil).
			if len(n.Arguments) >= 1 {
				return elemProvOf(c.provOf(n.Arguments[0]))
			}
		}

	case *ast.ConditionalNode:
		a := c.provOf(n.Exp1)
		b := c.provOf(n.Exp2)
		if provEqual(a, b) {
			return a
		}

	case *ast.SequenceNode:
		if len(n.Nodes) > 0 {
			return c.provOf(n.Nodes[len(n.Nodes)-1])
		}

	case *ast.VariableDeclaratorNode:
		// A nested let: compute the value's provenance, bind it to the
		// variable name and compute the body's provenance. The scope index
		// is irrelevant here as nothing is emitted.
		p := c.provOf(n.Value)
		c.scopes = append(c.scopes, scope{variableName: n.Name, index: -1, prov: p})
		defer func() {
			c.scopes = c.scopes[:len(c.scopes)-1]
		}()
		return c.provOf(n.Expr)
	}
	return unknownProv()
}

// provOfWithElem computes the provenance of an expression evaluated in a
// predicate scope whose `#` is elem (used for map/filter results).
func (c *compiler) provOfWithElem(node ast.Node, elem prov) prov {
	c.pushProvScope(provScope{elem: elem})
	defer c.popProvScope()
	return c.provOf(node)
}

// emitFieldAccess emits an OpLoadField/OpFetchField instruction and records
// its relocation metadata. receiver is the expression the field is fetched
// from (nil for environment loads); leafNode is the whole access expression,
// used to describe the fetched value.
func (c *compiler) emitFieldAccess(loc file.Location, op Opcode, field *runtime.Field, receiver ast.Node, leafNode ast.Node) {
	pc := c.emitLocation(loc, op, c.addConstant(field)) - 1
	ref := FieldRef{
		PC:   pc,
		Path: append([]string(nil), field.Path...),
		Leaf: c.leafShapeOf(leafNode),
	}
	if op == OpFetchField {
		p := c.provOf(receiver)
		if p.unknown {
			ref.Unknown = true
		} else {
			ref.Prov = p.steps
		}
	}
	c.fieldRefs = append(c.fieldRefs, ref)
}

// leafShapeOf computes the relocatable static shape of the value produced by
// the given expression.
func (c *compiler) leafShapeOf(node ast.Node) LeafShape {
	n := node.Nature()
	if n == nil {
		return LeafShape{Kind: ShapeUnknown}
	}
	return LeafShapeOfNature(c.ntCache, *n)
}
