package govaluate

import (
	"errors"
	"fmt"
)

/*
	BatchStats reports execution counters for a single BatchPlan.Eval call.

	Instructions is the number of compiled batch instructions in the plan.
	Dispatches is the number of times a batch instruction handler was actually
	entered during this run (instructions with an empty active row set are not
	dispatched). ActiveLanes is the sum, over all dispatches, of the number of
	active rows present when the instruction was entered.
*/
type BatchStats struct {
	Instructions uint64
	Dispatches   uint64
	ActiveLanes  uint64
}

/*
	BatchPlan is a compiled, reusable batch execution plan for an
	EvaluableExpression. It is created by CompileBatch and captures the
	expression's evaluation stages and type-checking behavior at compile time;
	later changes to the expression's public configuration do not affect the
	plan. A BatchPlan is safe for concurrent use by multiple goroutines,
	provided each goroutine calls Eval with its own rows.
*/
type BatchPlan struct {
	instructions []batchInstruction
	numSets      int
	numSlots     int
	rootSet      int
	rootOut      int
	rootSlot     int
}

// kinds of batch instructions
const (
	batchLeaf  uint8 = iota // no child stages; calls the stage operator directly
	batchApply              // regular operator over already-computed child values
	batchSplit              // control split for &&, ||, ??, and ternary stages
	batchMerge              // operator application for control stages, merging short-circuited rows back in
)

type batchInstruction struct {
	kind   uint8
	symbol OperatorSymbol

	operator        evaluationOperator
	leftTypeCheck   stageTypeCheck
	rightTypeCheck  stageTypeCheck
	typeCheck       stageCombinedTypeCheck
	typeErrorFormat string

	inSet    int // lane set read as input
	outSet   int // lane set written as output
	rightSet int // split only: lanes that continue into the right subtree

	leftSlot  int // value slot of the left child result, -1 if none
	rightSlot int // value slot of the right child result, -1 if none
	outSlot   int // value slot this instruction writes
}

/*
	CompileBatch lowers this expression's evaluation stages into a reusable
	batch execution plan. The expression must have ChecksTypes set to true;
	otherwise an error is returned. Compilation does not read any Parameters,
	does not run user functions or accessors, and does not modify the
	expression's tokens or evaluation stages.
*/
func (this *EvaluableExpression) CompileBatch() (*BatchPlan, error) {

	if !this.ChecksTypes {
		return nil, errors.New("Cannot compile a batch plan from an expression with ChecksTypes disabled")
	}

	plan := &BatchPlan{}

	// an expression without evaluation stages (such as one created from empty
	// input) compiles to an empty plan which yields nil, nil for every row.
	if this.evaluationStages == nil {
		return plan, nil
	}

	compiler := &batchCompiler{}
	rootSet := compiler.newSet()
	rootOut, rootSlot := compiler.compileStage(this.evaluationStages, rootSet)

	plan.instructions = compiler.instructions
	plan.numSets = compiler.numSets
	plan.numSlots = compiler.numSlots
	plan.rootSet = rootSet
	plan.rootOut = rootOut
	plan.rootSlot = rootSlot
	return plan, nil
}

/*
	Eval runs the compiled plan over [rows], one lane per record. The returned
	value and error slices have the same length as [rows]; rows that failed
	have a nil value and a non-nil error, while all other rows are still fully
	evaluated. A nil Parameters entry behaves exactly like Eval(nil).

	All intermediate values and lane masks belong to this call; the plan itself
	is not modified, and results returned by earlier calls are never touched.
*/
func (this *BatchPlan) Eval(rows []Parameters) ([]interface{}, []error, BatchStats) {

	rowCount := len(rows)
	values := make([]interface{}, rowCount)
	errs := make([]error, rowCount)
	stats := BatchStats{Instructions: uint64(len(this.instructions))}

	if rowCount == 0 || len(this.instructions) == 0 {
		return values, errs, stats
	}

	run := &batchRun{
		values: make([][]interface{}, this.numSlots),
		sets:   make([][]int, this.numSets),
		params: make([]Parameters, rowCount),
		errs:   errs,
	}
	for i := range run.values {
		run.values[i] = make([]interface{}, rowCount)
	}
	for i, parameters := range rows {
		if parameters != nil {
			run.params[i] = &sanitizedParameters{parameters}
		} else {
			run.params[i] = DUMMY_PARAMETERS
		}
	}

	root := make([]int, rowCount)
	for i := range root {
		root[i] = i
	}
	run.sets[this.rootSet] = root

	for i := range this.instructions {
		instruction := &this.instructions[i]
		active := run.sets[instruction.inSet]
		if len(active) == 0 {
			continue
		}
		stats.Dispatches++
		stats.ActiveLanes += uint64(len(active))
		instruction.execute(run, active)
	}

	for _, row := range run.sets[this.rootOut] {
		values[row] = run.values[this.rootSlot][row]
	}

	return values, errs, stats
}

// batchRun holds all mutable state for a single BatchPlan.Eval call.
type batchRun struct {
	values [][]interface{} // [slot][row]
	sets   [][]int         // lane sets, each sorted by ascending row index
	params []Parameters    // sanitized parameters, one per row
	errs   []error         // first error per row
}

type batchCompiler struct {
	instructions []batchInstruction
	numSets      int
	numSlots     int
}

func (this *batchCompiler) newSet() int {
	ret := this.numSets
	this.numSets++
	return ret
}

func (this *batchCompiler) newSlot() int {
	ret := this.numSlots
	this.numSlots++
	return ret
}

/*
	Lowers one evaluationStage into batch instructions, appended in execution
	order. [inSet] is the lane set whose rows enter this stage. Returns the lane
	set of rows that leave the stage with a value, and the value slot holding
	those results.
*/
func (this *batchCompiler) compileStage(stage *evaluationStage, inSet int) (int, int) {

	outSlot := this.newSlot()

	// leaf: literals, parameters, and function/accessor stages without arguments
	if stage.leftStage == nil && stage.rightStage == nil {
		outSet := this.newSet()
		this.instructions = append(this.instructions, batchInstruction{
			kind:     batchLeaf,
			symbol:   stage.symbol,
			operator: stage.operator,
			inSet:    inSet,
			outSet:   outSet,
			leftSlot: -1,
			rightSlot: -1,
			outSlot:  outSlot,
		})
		return outSet, outSlot
	}

	// control stages split the active lanes between a short-circuit result and
	// the right subtree, then merge both back together after applying.
	if stage.isShortCircuitable() {
		leftOut, leftSlot := this.compileStage(stage.leftStage, inSet)

		rightIn := this.newSet()
		nodeOut := this.newSet()
		this.instructions = append(this.instructions, batchInstruction{
			kind:      batchSplit,
			symbol:    stage.symbol,
			inSet:     leftOut,
			outSet:    nodeOut,
			rightSet:  rightIn,
			leftSlot:  leftSlot,
			rightSlot: -1,
			outSlot:   outSlot,
		})

		rightOut, rightSlot := this.compileStage(stage.rightStage, rightIn)
		this.instructions = append(this.instructions, batchInstruction{
			kind:            batchMerge,
			symbol:          stage.symbol,
			operator:        stage.operator,
			leftTypeCheck:   stage.leftTypeCheck,
			rightTypeCheck:  stage.rightTypeCheck,
			typeCheck:       stage.typeCheck,
			typeErrorFormat: stage.typeErrorFormat,
			inSet:           rightOut,
			outSet:          nodeOut,
			rightSet:        -1,
			leftSlot:        leftSlot,
			rightSlot:       rightSlot,
			outSlot:         outSlot,
		})
		return nodeOut, outSlot
	}

	// regular stages: left subtree, then right subtree gated on left success,
	// then the operator itself.
	current := inSet
	leftSlot := -1
	if stage.leftStage != nil {
		var leftOut int
		leftOut, leftSlot = this.compileStage(stage.leftStage, current)
		current = leftOut
	}

	rightSlot := -1
	if stage.rightStage != nil {
		var rightOut int
		rightOut, rightSlot = this.compileStage(stage.rightStage, current)
		current = rightOut
	}

	outSet := this.newSet()
	this.instructions = append(this.instructions, batchInstruction{
		kind:            batchApply,
		symbol:          stage.symbol,
		operator:        stage.operator,
		leftTypeCheck:   stage.leftTypeCheck,
		rightTypeCheck:  stage.rightTypeCheck,
		typeCheck:       stage.typeCheck,
		typeErrorFormat: stage.typeErrorFormat,
		inSet:           current,
		outSet:          outSet,
		rightSet:        -1,
		leftSlot:        leftSlot,
		rightSlot:       rightSlot,
		outSlot:         outSlot,
	})
	return outSet, outSlot
}

func (this *batchInstruction) execute(run *batchRun, active []int) {

	switch this.kind {
	case batchLeaf:
		this.executeLeaf(run, active)
	case batchApply:
		run.sets[this.outSet] = this.executeOperator(run, active)
	case batchSplit:
		this.executeSplit(run, active)
	case batchMerge:
		out := this.executeOperator(run, active)
		// rows that were short-circuited by the split already hold their final
		// values; merge them back with the rows that went through the operator.
		run.sets[this.outSet] = mergeLanes(run.sets[this.outSet], out)
	}
}

func (this *batchInstruction) executeLeaf(run *batchRun, active []int) {

	var out []int

	for _, row := range active {
		value, err := this.operator(nil, nil, run.params[row])
		if err != nil {
			run.errs[row] = err
			continue
		}
		run.values[this.outSlot][row] = value
		out = append(out, row)
	}

	run.sets[this.outSet] = out
}

func (this *batchInstruction) executeOperator(run *batchRun, active []int) []int {

	var out []int

	for _, row := range active {

		var left, right interface{}
		if this.leftSlot >= 0 {
			left = run.values[this.leftSlot][row]
		}
		if this.rightSlot >= 0 {
			right = run.values[this.rightSlot][row]
		}

		if this.typeCheck == nil {
			if err := typeCheck(this.leftTypeCheck, left, this.symbol, this.typeErrorFormat); err != nil {
				run.errs[row] = err
				continue
			}
			if err := typeCheck(this.rightTypeCheck, right, this.symbol, this.typeErrorFormat); err != nil {
				run.errs[row] = err
				continue
			}
		} else if !this.typeCheck(left, right) {
			errorMsg := fmt.Sprintf(this.typeErrorFormat, left, this.symbol.String())
			run.errs[row] = errors.New(errorMsg)
			continue
		}

		value, err := this.operator(left, right, run.params[row])
		if err != nil {
			run.errs[row] = err
			continue
		}
		run.values[this.outSlot][row] = value
		out = append(out, row)
	}

	return out
}

/*
	Splits the active lanes of a short-circuitable stage. Rows whose outcome is
	decided by the left value alone get their final value written immediately
	and join the stage's output set; the remaining lanes flow into the right
	subtree. This mirrors the short-circuit decisions made by the scalar
	interpreter for &&, ||, ??, and both ternary stages.
*/
func (this *batchInstruction) executeSplit(run *batchRun, active []int) {

	var right, decided []int

	for _, row := range active {

		left := run.values[this.leftSlot][row]
		shortCircuited := false
		var value interface{}

		switch this.symbol {
		case AND:
			if left == false {
				shortCircuited = true
				value = false
			}
		case OR:
			if left == true {
				shortCircuited = true
				value = true
			}
		case COALESCE, TERNARY_FALSE:
			if left != nil {
				shortCircuited = true
				value = left
			}
		case TERNARY_TRUE:
			if left == false {
				shortCircuited = true
				value = nil
			}
		}

		if shortCircuited {
			run.values[this.outSlot][row] = value
			decided = append(decided, row)
		} else {
			right = append(right, row)
		}
	}

	run.sets[this.rightSet] = right
	run.sets[this.outSet] = decided
}

// merges two disjoint, ascending lane sets into one ascending set.
func mergeLanes(a, b []int) []int {

	if len(a) == 0 {
		return b
	}
	if len(b) == 0 {
		return a
	}

	out := make([]int, 0, len(a)+len(b))
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		if a[i] < b[j] {
			out = append(out, a[i])
			i++
		} else {
			out = append(out, b[j])
			j++
		}
	}
	out = append(out, a[i:]...)
	out = append(out, b[j:]...)
	return out
}
