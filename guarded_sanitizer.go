package taint

// Guarded sanitizers (also called fallible sanitizers) are library calls that
// clean a value only when they report success through a paired error result.
// A model declares them with SanitizerModel.Result and
// SanitizerModel.ErrorResult; this file holds the use-site proof that pairs
// the cleaned result with a control-flow check of that specific call's error.
//
// The proof is deliberately local and conservative:
//
//   - only static calls with resolvable signatures are considered;
//   - the sanitized value is the tuple element at Result, paired with the
//     error element at ErrorResult of the *same* SSA call value;
//   - the error must be compared directly against nil (either == or !=), and
//     the nil edge of that branch must dominate the use: no path to the use
//     may bypass it;
//   - Phis are proven per incoming edge, using each predecessor as that
//     edge's use site;
//   - local register loads are followed through reaching stores, and every
//     reaching store must carry a proof at the use;
//   - index bounds, non-error error slots, unresolved calls, reassigned
//     errors, cyclic values, and loop-carried values all fail closed
//     (unproven), so they keep reporting.
//
// What is intentionally *not* modeled: cross-function condition propagation,
// closures sharing written state, and concurrent scheduling. Those fall back
// to the ordinary conservative taint walk.

import (
	"go/token"
	"go/types"

	"golang.org/x/tools/go/ssa"
)

// guardedStatus is the outcome of resolving a value against guarded sanitizer
// models at a use site.
type guardedStatus int

const (
	// guardNone means the value has no guarded sanitizer involvement, so the
	// ordinary taint walk is the only authority.
	guardNone guardedStatus = iota
	// guardUnproven means the value is (or merges) a guarded sanitizer result
	// but the use is not dominated by a proof that the same call succeeded.
	guardUnproven
	// guardProven means every contributing guarded sanitizer result was proven
	// at the use site.
	guardProven
)

// guardedSanitizerAt resolves v at the given use block against the registry's
// guarded sanitizer rules. It understands the same SSA value aliases as the
// unconditional sanitizer check — conversions, interface changes, and local
// loads — plus per-edge reasoning for Phis. seen is only used to break cycles
// (for example loop-carried Phis); it is not a memo table.
func (r *ruleRegistry) guardedSanitizerAt(v ssa.Value, use *ssa.BasicBlock, seen map[ssa.Value]struct{}) (sanitizerRule, guardedStatus) {
	if r == nil || v == nil || use == nil {
		return sanitizerRule{}, guardNone
	}
	// No guarded model is configured: skip the proof machinery entirely so the
	// common path costs nothing beyond this check.
	if len(r.guardedSanitizers) == 0 {
		return sanitizerRule{}, guardNone
	}
	if seen == nil {
		seen = map[ssa.Value]struct{}{}
	}
	if _, ok := seen[v]; ok {
		// A cyclic value cannot carry a proof for itself at this use.
		return sanitizerRule{}, guardUnproven
	}
	seen[v] = struct{}{}

	switch value := v.(type) {
	case *ssa.Extract:
		return r.guardedExtractAt(value, use)
	case *ssa.Phi:
		block := value.Block()
		var (
			firstRule   sanitizerRule
			involved    bool
			allProven   = true
			haveEdgeUse bool
		)
		for i, edge := range value.Edges {
			var edgeUse *ssa.BasicBlock
			if block != nil && i < len(block.Preds) {
				edgeUse = block.Preds[i]
			}
			if edgeUse == nil {
				allProven = false
				continue
			}
			haveEdgeUse = true
			rule, status := r.guardedSanitizerAt(edge, edgeUse, cloneSeen(seen))
			if status == guardNone {
				allProven = false
				continue
			}
			involved = true
			if firstRule.id == "" {
				firstRule = rule
			}
			if status != guardProven {
				allProven = false
			}
		}
		if !involved {
			return sanitizerRule{}, guardNone
		}
		if allProven && haveEdgeUse {
			return firstRule, guardProven
		}
		return firstRule, guardUnproven
	case *ssa.UnOp:
		if value.Op != token.MUL {
			return sanitizerRule{}, guardNone
		}
		return r.guardedLoadedValueAt(value, use, seen)
	case *ssa.MakeInterface:
		return r.guardedSanitizerAt(value.X, use, seen)
	case *ssa.ChangeInterface:
		return r.guardedSanitizerAt(value.X, use, seen)
	case *ssa.ChangeType:
		return r.guardedSanitizerAt(value.X, use, seen)
	case *ssa.Convert:
		return r.guardedSanitizerAt(value.X, use, seen)
	case *ssa.TypeAssert:
		return r.guardedSanitizerAt(value.X, use, seen)
	}
	return sanitizerRule{}, guardNone
}

// guardedValid reports whether the rule carries a usable index pair. Parsed
// models always validate; a programmatically built model with a lone, negative,
// or equal index pair compiles to an invalid rule that never proves.
func (r sanitizerRule) guardedValid() bool {
	return r.guarded && r.resultIndex >= 0 && r.errorIndex >= 0 && r.resultIndex != r.errorIndex
}

// guardedExtractAt resolves a tuple element against the guarded models.
func (r *ruleRegistry) guardedExtractAt(value *ssa.Extract, use *ssa.BasicBlock) (sanitizerRule, guardedStatus) {
	call, ok := value.Tuple.(*ssa.Call)
	if !ok {
		return sanitizerRule{}, guardNone
	}
	for _, rule := range r.guardedSanitizers {
		if rule.matchCall == nil || !rule.matchCall(&call.Call) {
			continue
		}
		if !rule.guardedValid() || value.Index != rule.resultIndex {
			// A malformed rule, or a different tuple element, is not the
			// guarded cleaned result. Malformed rules stay unproven.
			if rule.guardedValid() {
				continue
			}
			return rule, guardUnproven
		}
		if !guardedCallResolvable(call, rule) {
			return rule, guardUnproven
		}
		if r.errorResultProvenAt(call, rule.errorIndex, use) {
			return rule, guardProven
		}
		return rule, guardUnproven
	}
	return sanitizerRule{}, guardNone
}

// guardedLoadedValueAt follows every value a local load could observe. A load
// whose address is written by a helper call (or whose reaching set is empty)
// cannot carry a local proof, so it fails closed.
func (r *ruleRegistry) guardedLoadedValueAt(load *ssa.UnOp, use *ssa.BasicBlock, seen map[ssa.Value]struct{}) (sanitizerRule, guardedStatus) {
	effects, ok := reachingValuesForLoad(load)
	if !ok || len(effects) == 0 {
		return sanitizerRule{}, guardNone
	}
	var (
		firstRule sanitizerRule
		involved  bool
		proven    = true
	)
	for _, effect := range effects {
		// A helper wrote the slot; cross-function condition proofs are
		// outside the supported scope, so any sanitizer value it may have
		// stored is at best unproven.
		rule, status := r.guardedSanitizerAt(effect.value, use, cloneSeen(seen))
		if effect.call != nil || effect.callee != nil {
			status = guardUnproven
		}
		if status == guardNone {
			proven = false
			continue
		}
		involved = true
		if firstRule.id == "" {
			firstRule = rule
		}
		if status != guardProven {
			proven = false
		}
	}
	if !involved {
		return sanitizerRule{}, guardNone
	}
	if proven {
		return firstRule, guardProven
	}
	return firstRule, guardUnproven
}

func cloneSeen(seen map[ssa.Value]struct{}) map[ssa.Value]struct{} {
	out := make(map[ssa.Value]struct{}, len(seen))
	for k := range seen {
		out[k] = struct{}{}
	}
	return out
}

// guardedCallResolvable reports whether the call can be checked for the
// configured result indices: it must be a static, resolvable call whose
// signature has both result positions and whose error slot is the predeclared
// error type. Anything else is treated as unproven rather than crashing or
// suppressing diagnostics.
func guardedCallResolvable(call *ssa.Call, rule sanitizerRule) bool {
	if call == nil {
		return false
	}
	if staticCallee(&call.Call) == nil {
		return false
	}
	sig := call.Call.Signature()
	if sig == nil || sig.Results() == nil {
		return false
	}
	n := sig.Results().Len()
	if rule.resultIndex < 0 || rule.errorIndex < 0 || rule.resultIndex >= n || rule.errorIndex >= n {
		return false
	}
	return isPredeclaredError(sig.Results().At(rule.errorIndex).Type())
}

// isPredeclaredError reports whether t is the predeclared error interface.
func isPredeclaredError(t types.Type) bool {
	if t == nil {
		return false
	}
	obj := types.Universe.Lookup("error")
	if obj == nil {
		return false
	}
	return types.Identical(types.Unalias(t), obj.Type())
}

// errorResultProvenAt reports whether every control-flow path to use passes a
// nil comparison of the error result at errorIndex from the same call.
func (r *ruleRegistry) errorResultProvenAt(call *ssa.Call, errorIndex int, use *ssa.BasicBlock) bool {
	if call == nil || use == nil {
		return false
	}
	errValues := errorExtractValues(call, errorIndex)
	if len(errValues) == 0 {
		// The error result is never materialized, so it cannot have been
		// checked. (A comparison needs an SSA value for it.)
		return false
	}
	fn := use.Parent()
	if fn == nil || len(fn.Blocks) == 0 {
		return false
	}
	for _, block := range fn.Blocks {
		if block == nil {
			continue
		}
		for _, instr := range block.Instrs {
			ifInstr, ok := instr.(*ssa.If)
			if !ok {
				continue
			}
			if _, ok := nilComparisonOperand(ifInstr.Cond, errValues); !ok {
				continue
			}
			nilIndex, ok := nilBranchIndex(ifInstr)
			if !ok {
				continue
			}
			if r.nilEdgeReachesUse(fn, block, nilIndex, use) {
				return true
			}
		}
	}
	return false
}

// nilEdgeReachesUse reports whether the nil edge of the If in ifBlock
// dominates use. Dominance is tested exactly by re-running reachability with
// that one edge removed: if use is still reachable, some path can bypass the
// proof, so the edge does not dominate it.
func (r *ruleRegistry) nilEdgeReachesUse(fn *ssa.Function, ifBlock *ssa.BasicBlock, nilIndex int, use *ssa.BasicBlock) bool {
	if fn == nil || ifBlock == nil || use == nil || nilIndex < 0 || nilIndex >= len(ifBlock.Succs) {
		return false
	}
	key := guardEdgeKey{
		fn:       fn,
		ifBlock:  ifBlock,
		nilIndex: nilIndex,
		use:      use,
	}
	if r.guardProofCache != nil {
		if cached, ok := r.guardProofCache[key]; ok {
			return cached
		}
	}
	reachable := blockReachableWithoutEdge(fn, ifBlock, nilIndex, use)
	if r.guardProofCache == nil {
		r.guardProofCache = map[guardEdgeKey]bool{}
	}
	r.guardProofCache[key] = !reachable
	return !reachable
}

type guardEdgeKey struct {
	fn       *ssa.Function
	ifBlock  *ssa.BasicBlock
	nilIndex int
	use      *ssa.BasicBlock
}

// blockReachableWithoutEdge reports whether target is reachable from the
// function entry without traversing the iff's successor at skipIndex.
func blockReachableWithoutEdge(fn *ssa.Function, iff *ssa.BasicBlock, skipIndex int, target *ssa.BasicBlock) bool {
	if fn == nil || len(fn.Blocks) == 0 || target == nil {
		return true
	}
	visited := map[*ssa.BasicBlock]struct{}{}
	stack := []*ssa.BasicBlock{fn.Blocks[0]}
	for len(stack) > 0 {
		block := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if block == nil {
			continue
		}
		if _, ok := visited[block]; ok {
			continue
		}
		visited[block] = struct{}{}
		if block == target {
			return true
		}
		for i, succ := range block.Succs {
			if succ == nil {
				continue
			}
			if block == iff && i == skipIndex {
				continue
			}
			stack = append(stack, succ)
		}
	}
	return false
}

// errorExtractValues collects the SSA values that materialize result
// errorIndex of call. Normally there is exactly one Extract per used tuple
// element; comparing any of them to nil proves the same call result.
func errorExtractValues(call *ssa.Call, errorIndex int) []ssa.Value {
	if call == nil || errorIndex < 0 {
		return nil
	}
	refs := call.Referrers()
	if refs == nil {
		return nil
	}
	var out []ssa.Value
	for _, ref := range *refs {
		extract, ok := ref.(*ssa.Extract)
		if !ok || extract.Tuple != call || extract.Index != errorIndex {
			continue
		}
		out = append(out, extract)
	}
	return out
}

// nilComparisonOperand reports whether cond compares one of errValues against
// a nil constant, after peeling SSA value aliases. It returns the matched
// value only for diagnostics; the boolean is the proof signal.
func nilComparisonOperand(cond ssa.Value, errValues []ssa.Value) (ssa.Value, bool) {
	bin, ok := cond.(*ssa.BinOp)
	if !ok || (bin.Op != token.EQL && bin.Op != token.NEQ) {
		return nil, false
	}
	if isNilConstant(bin.X) {
		if v, ok := sameSSAValue(bin.Y, errValues); ok {
			return v, true
		}
	}
	if isNilConstant(bin.Y) {
		if v, ok := sameSSAValue(bin.X, errValues); ok {
			return v, true
		}
	}
	return nil, false
}

// nilBranchIndex returns the successor index taken when the compared error is
// nil. SSA's builder always emits If with Succs[0] for the true edge and
// Succs[1] for the false edge, so `== nil` takes Succs[0] and `!= nil` takes
// Succs[1]. Negated conditions are already folded into that order by the
// builder.
func nilBranchIndex(ifInstr *ssa.If) (int, bool) {
	if ifInstr == nil {
		return 0, false
	}
	bin, ok := ifInstr.Cond.(*ssa.BinOp)
	if !ok || (bin.Op != token.EQL && bin.Op != token.NEQ) {
		return 0, false
	}
	block := ifInstr.Block()
	if block == nil || len(block.Succs) < 2 {
		return 0, false
	}
	if bin.Op == token.EQL {
		return 0, true
	}
	return 1, true
}

func isNilConstant(v ssa.Value) bool {
	c, ok := v.(*ssa.Const)
	return ok && c.IsNil()
}

// sameSSAValue reports whether v is the error value named by errValues, after
// peeling conversions and interface changes and following local loads to
// their reaching stores. Two extracts of the same call and result position
// are considered the same value. A load with a possible helper-written
// reaching value does not match: a wrapper that stores the error is a
// cross-function proof, which is outside the supported scope.
func sameSSAValue(v ssa.Value, errValues []ssa.Value) (ssa.Value, bool) {
	return sameSSAValueSeen(v, errValues, map[ssa.Value]struct{}{})
}

func sameSSAValueSeen(v ssa.Value, errValues []ssa.Value, seen map[ssa.Value]struct{}) (ssa.Value, bool) {
	if v == nil {
		return nil, false
	}
	if _, ok := seen[v]; ok {
		return nil, false
	}
	seen[v] = struct{}{}
	for _, candidate := range errValues {
		if candidate == nil {
			continue
		}
		if v == candidate {
			return candidate, true
		}
		if a, ok := v.(*ssa.Extract); ok {
			if b, ok := candidate.(*ssa.Extract); ok && a.Tuple == b.Tuple && a.Index == b.Index {
				return candidate, true
			}
		}
	}
	switch x := v.(type) {
	case *ssa.MakeInterface:
		return sameSSAValueSeen(x.X, errValues, seen)
	case *ssa.ChangeInterface:
		return sameSSAValueSeen(x.X, errValues, seen)
	case *ssa.ChangeType:
		return sameSSAValueSeen(x.X, errValues, seen)
	case *ssa.Convert:
		return sameSSAValueSeen(x.X, errValues, seen)
	case *ssa.TypeAssert:
		return sameSSAValueSeen(x.X, errValues, seen)
	case *ssa.UnOp:
		if x.Op != token.MUL {
			return nil, false
		}
		effects, ok := reachingValuesForLoad(x)
		if !ok || len(effects) == 0 {
			return nil, false
		}
		var matched ssa.Value
		for _, effect := range effects {
			if effect.call != nil || effect.callee != nil {
				return nil, false
			}
			candidate, ok := sameSSAValueSeen(effect.value, errValues, cloneSeen(seen))
			if !ok {
				return nil, false
			}
			if matched == nil {
				matched = candidate
			}
		}
		return matched, matched != nil
	}
	return nil, false
}
