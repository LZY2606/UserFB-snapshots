package agouti

import (
	"fmt"
	"strings"

	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/internal/element"
)

// A FormStep describes the desired state of one named field group in a form.
// Name is the literal value of the name attribute shared by the fields in the
// group; it is never interpreted as a CSS selector or path expression.
// The interpretation of Values depends on the type of the fields:
//
//	text input or textarea: exactly one string, the desired text
//	checkbox group:          the set of values that should be checked (may be empty)
//	radio group:             exactly one value, the one that should be selected
//	single <select>:         exactly one option value
//	multiple <select>:       the set of option values that should be selected
type FormStep struct {
	Name   string
	Values []string
}

// A FormResult reports the outcome of ApplyForm. Completed counts the
// consecutive steps, starting from the first, that were confirmed to have
// reached their target state. Actions counts the remote Clear, Value, and
// Click requests that were actually sent, including failed ones.
type FormResult struct {
	Completed int
	Actions   int
}

const (
	formControlsXPath = `.//input | .//textarea | .//select`
	formOptionsXPath  = `.//option`
)

var formTextInputTypes = map[string]bool{
	"": true, "text": true, "email": true, "tel": true,
	"url": true, "search": true, "password": true,
}

type formKind int

const (
	formText formKind = iota
	formCheckboxes
	formRadios
	formSelect
)

// formControl is a snapshot of a single control or option.
type formControl struct {
	element   element.Element
	tag       string
	inputType string
	value     string
	enabled   bool
	selected  bool
}

// formSnapshot is a fully validated snapshot of one named field group.
type formSnapshot struct {
	kind     formKind
	multiple bool
	text     formControl
	members  []formControl
	options  []formControl
}

func (s *formSnapshot) items() []formControl {
	if s.kind == formSelect {
		return s.options
	}
	return s.members
}

// ApplyForm reconciles the fields of the form referred to by the selection
// with the provided steps, in order. The selection must resolve to exactly
// one <form> element; all lookups are scoped to that form and repeated from
// the original selection before every action, so a page that replaces the
// form (or the field group) between actions is followed naturally and no
// remote element IDs are retained across actions.
//
// The maxActions budget is shared by all steps. Every Clear, Value, or Click
// request consumes one unit, whether it succeeds or fails. When the next
// action would exceed the budget, ApplyForm stops with an error before
// sending it, so a reactive page that keeps reverting changes cannot cause
// an infinite loop.
//
// On failure, the returned FormResult preserves the completed prefix of
// steps and the number of actions actually sent, and the error names the
// failing step. No failed write is replayed and no rollback is attempted;
// a later call starts over from the then-current page state.
//
// ApplyForm does not change the behavior of Fill, Check, Select, or any
// other Selection method.
func (s *Selection) ApplyForm(steps []FormStep, maxActions int) (FormResult, error) {
	if maxActions <= 0 {
		return FormResult{}, fmt.Errorf("failed to apply form to %s: action budget must be positive, got %d", s, maxActions)
	}

	checked, err := validateFormSteps(s, steps)
	if err != nil {
		return FormResult{}, err
	}

	reconciler := &formReconciler{selection: s, budget: maxActions}
	for i, step := range checked {
		if err := reconciler.applyStep(step); err != nil {
			result := FormResult{Completed: i, Actions: reconciler.actions}
			return result, fmt.Errorf("failed to apply form step %q to %s: %s", step.Name, s, err)
		}
	}

	return FormResult{Completed: len(checked), Actions: reconciler.actions}, nil
}

// validateFormSteps rejects empty names, duplicate names, and duplicate
// values within a step before any remote request is sent, and returns a
// copy of the steps so the caller's slices are never mutated.
func validateFormSteps(s *Selection, steps []FormStep) ([]FormStep, error) {
	checked := make([]FormStep, len(steps))
	seen := map[string]bool{}
	for i, step := range steps {
		if step.Name == "" {
			return nil, fmt.Errorf("failed to apply form to %s: step %d has an empty name", s, i)
		}
		if seen[step.Name] {
			return nil, fmt.Errorf("failed to apply form to %s: duplicate step name %q", s, step.Name)
		}
		seen[step.Name] = true

		values := make([]string, len(step.Values))
		copy(values, step.Values)
		seenValues := map[string]bool{}
		for _, value := range values {
			if seenValues[value] {
				return nil, fmt.Errorf("failed to apply form to %s: duplicate value %q in step %q", s, value, step.Name)
			}
			seenValues[value] = true
		}
		checked[i] = FormStep{Name: step.Name, Values: values}
	}
	return checked, nil
}

type formReconciler struct {
	selection *Selection
	budget    int
	actions   int
}

func (r *formReconciler) applyStep(step FormStep) error {
	snapshot, err := r.snapshot(step)
	if err != nil {
		return err
	}

	switch {
	case snapshot.kind == formText:
		return r.applyText(step, snapshot)
	case snapshot.kind == formCheckboxes || (snapshot.kind == formSelect && snapshot.multiple):
		return r.applySet(step, snapshot)
	default:
		return r.applySingle(step, snapshot)
	}
}

// applyText reconciles a single text field. A differing value is cleared
// first; after a successful Clear the form and field are re-resolved and
// re-validated before Value is sent, and Value is sent even if the field
// already holds the target text. The step only completes when a fresh
// observation confirms the target value.
func (r *formReconciler) applyText(step FormStep, snapshot *formSnapshot) error {
	target := step.Values[0]
	if snapshot.text.value == target {
		return nil
	}

	if err := r.act(fmt.Sprintf("clear field %q", step.Name), snapshot.text.element.Clear); err != nil {
		return err
	}

	snapshot, err := r.snapshot(step)
	if err != nil {
		return err
	}
	if snapshot.kind != formText {
		return fmt.Errorf("field %q is no longer a single text field after clear", step.Name)
	}

	if err := r.act(fmt.Sprintf("enter text into field %q", step.Name), func() error {
		return snapshot.text.element.Value(target)
	}); err != nil {
		return err
	}

	snapshot, err = r.snapshot(step)
	if err != nil {
		return err
	}
	if snapshot.kind != formText {
		return fmt.Errorf("field %q is no longer a single text field after entering text", step.Name)
	}
	if snapshot.text.value != target {
		return fmt.Errorf("field %q holds %q after entering text, expected %q", step.Name, snapshot.text.value, target)
	}
	return nil
}

// applySet reconciles a checkbox group or a multiple select. Only the first
// control in DOM order whose selection state differs from the target set is
// clicked; the snapshot is then rebuilt from scratch before the next click,
// since the click may have replaced every element in the group.
func (r *formReconciler) applySet(step FormStep, snapshot *formSnapshot) error {
	kind := snapshot.kind
	for {
		mismatch := firstSetMismatch(snapshot, step.Values)
		if mismatch == nil {
			return nil
		}

		if err := r.act(fmt.Sprintf("click value %q of field %q", mismatch.value, step.Name), mismatch.element.Click); err != nil {
			return err
		}

		var err error
		snapshot, err = r.snapshot(step)
		if err != nil {
			return err
		}
		if snapshot.kind != kind || (kind == formSelect && !snapshot.multiple) {
			return fmt.Errorf("field %q changed type after click", step.Name)
		}
	}
}

func firstSetMismatch(snapshot *formSnapshot, values []string) *formControl {
	desired := map[string]bool{}
	for _, value := range values {
		desired[value] = true
	}
	items := snapshot.items()
	for i := range items {
		if items[i].selected != desired[items[i].value] {
			return &items[i]
		}
	}
	return nil
}

// applySingle reconciles a radio group or a single select. Only the target
// item is ever clicked; the previously selected item is never clicked off.
func (r *formReconciler) applySingle(step FormStep, snapshot *formSnapshot) error {
	kind := snapshot.kind
	target := step.Values[0]
	for {
		items := snapshot.items()
		var targetItem *formControl
		extraSelected := false
		for i := range items {
			if items[i].value == target {
				targetItem = &items[i]
			} else if items[i].selected {
				extraSelected = true
			}
		}
		if targetItem.selected {
			if extraSelected {
				return fmt.Errorf("field %q has values selected besides %q", step.Name, target)
			}
			return nil
		}

		if err := r.act(fmt.Sprintf("click value %q of field %q", target, step.Name), targetItem.element.Click); err != nil {
			return err
		}

		var err error
		snapshot, err = r.snapshot(step)
		if err != nil {
			return err
		}
		if snapshot.kind != kind || (kind == formSelect && snapshot.multiple) {
			return fmt.Errorf("field %q changed type after click", step.Name)
		}
	}
}

// act sends one remote action, charging it against the shared budget. The
// budget is checked before the request is sent, and failed requests still
// count.
func (r *formReconciler) act(description string, action func() error) error {
	if r.actions >= r.budget {
		return fmt.Errorf("action budget of %d exhausted before attempting to %s", r.budget, description)
	}
	r.actions++
	return action()
}

// snapshot re-resolves the original selection to the current form, finds the
// current field group for the step name inside that form, and validates the
// whole group and the target values before any action is considered.
func (r *formReconciler) snapshot(step FormStep) (*formSnapshot, error) {
	form, err := r.resolveForm()
	if err != nil {
		return nil, err
	}

	controls, err := form.GetElements(api.Selector{Using: "xpath", Value: formControlsXPath})
	if err != nil {
		return nil, fmt.Errorf("failed to query controls in form for %s: %s", r.selection, err)
	}

	members := []element.Element{}
	for _, control := range controls {
		name, err := control.GetAttribute("name")
		if err != nil {
			return nil, fmt.Errorf("failed to read name attribute of a control in form for %s: %s", r.selection, err)
		}
		if name == step.Name {
			members = append(members, control)
		}
	}
	if len(members) == 0 {
		return nil, fmt.Errorf("no fields named %q found in form", step.Name)
	}

	return classifyFormGroup(step, members)
}

// resolveForm re-resolves the original selection and requires exactly one
// current <form> element.
func (r *formReconciler) resolveForm() (element.Element, error) {
	form, err := r.selection.elements.GetExactlyOne()
	if err != nil {
		return nil, fmt.Errorf("failed to resolve exactly one form from %s: %s", r.selection, err)
	}
	tag, err := form.GetName()
	if err != nil {
		return nil, fmt.Errorf("failed to determine tag of form candidate for %s: %s", r.selection, err)
	}
	if !strings.EqualFold(tag, "form") {
		return nil, fmt.Errorf("%s resolved to <%s>, expected exactly one <form>", r.selection, tag)
	}
	return form, nil
}

// classifyFormGroup determines the kind of a named field group, reads the
// state of every member, and validates the group against the step: no mixed
// or unsupported types, no empty groups, unique checkbox/radio/option
// values, no disabled controls or options, and all target values present.
func classifyFormGroup(step FormStep, members []element.Element) (*formSnapshot, error) {
	tags := make([]string, len(members))
	types := make([]string, len(members))
	for i, member := range members {
		tag, err := member.GetName()
		if err != nil {
			return nil, fmt.Errorf("failed to determine tag of a field named %q: %s", step.Name, err)
		}
		tags[i] = strings.ToLower(tag)
		if tags[i] == "input" {
			inputType, err := member.GetAttribute("type")
			if err != nil {
				return nil, fmt.Errorf("failed to read type attribute of a field named %q: %s", step.Name, err)
			}
			types[i] = strings.ToLower(inputType)
		}
	}

	kind := formText
	for i := range members {
		memberKind, err := formMemberKind(step.Name, tags[i], types[i])
		if err != nil {
			return nil, err
		}
		if i == 0 {
			kind = memberKind
		} else if memberKind != kind {
			return nil, fmt.Errorf("fields named %q have mixed types", step.Name)
		}
	}

	switch kind {
	case formText:
		if len(members) > 1 {
			return nil, fmt.Errorf("multiple text fields named %q found in form", step.Name)
		}
		if len(step.Values) != 1 {
			return nil, fmt.Errorf("text field %q requires exactly one target value, got %d", step.Name, len(step.Values))
		}
		return snapshotText(step.Name, members[0], tags[0], types[0])
	case formCheckboxes:
		return snapshotMembers(step, members, tags, types, formCheckboxes)
	case formRadios:
		if len(step.Values) != 1 {
			return nil, fmt.Errorf("radio group %q requires exactly one target value, got %d", step.Name, len(step.Values))
		}
		return snapshotMembers(step, members, tags, types, formRadios)
	default:
		if len(members) > 1 {
			return nil, fmt.Errorf("multiple select elements named %q found in form", step.Name)
		}
		return snapshotSelect(step, members[0])
	}
}

func formMemberKind(name, tag, inputType string) (formKind, error) {
	switch tag {
	case "textarea":
		return formText, nil
	case "input":
		switch {
		case formTextInputTypes[inputType]:
			return formText, nil
		case inputType == "checkbox":
			return formCheckboxes, nil
		case inputType == "radio":
			return formRadios, nil
		default:
			return 0, fmt.Errorf("field %q has unsupported input type %q", name, inputType)
		}
	case "select":
		return formSelect, nil
	}
	return 0, fmt.Errorf("field %q has unsupported tag <%s>", name, tag)
}

func snapshotText(name string, member element.Element, tag, inputType string) (*formSnapshot, error) {
	control := formControl{element: member, tag: tag, inputType: inputType}
	if err := readEnabledAndValue(&control, name); err != nil {
		return nil, err
	}
	if !control.enabled {
		return nil, fmt.Errorf("text field %q is disabled", name)
	}
	return &formSnapshot{kind: formText, text: control}, nil
}

func snapshotMembers(step FormStep, members []element.Element, tags, types []string, kind formKind) (*formSnapshot, error) {
	snapshot := &formSnapshot{kind: kind}
	seen := map[string]bool{}
	for i, member := range members {
		control := formControl{element: member, tag: tags[i], inputType: types[i]}
		if err := readEnabledAndValue(&control, step.Name); err != nil {
			return nil, err
		}
		if seen[control.value] {
			return nil, fmt.Errorf("fields named %q have duplicate value %q", step.Name, control.value)
		}
		seen[control.value] = true
		if !control.enabled {
			return nil, fmt.Errorf("field %q with value %q is disabled", step.Name, control.value)
		}
		selected, err := member.IsSelected()
		if err != nil {
			return nil, fmt.Errorf("failed to read selection state of field %q: %s", step.Name, err)
		}
		control.selected = selected
		snapshot.members = append(snapshot.members, control)
	}
	if err := checkFormTargets(step, seen); err != nil {
		return nil, err
	}
	return snapshot, nil
}

func snapshotSelect(step FormStep, member element.Element) (*formSnapshot, error) {
	multiple, err := member.GetAttribute("multiple")
	if err != nil {
		return nil, fmt.Errorf("failed to read multiple attribute of select %q: %s", step.Name, err)
	}
	snapshot := &formSnapshot{kind: formSelect, multiple: multiple != ""}

	enabled, err := member.IsEnabled()
	if err != nil {
		return nil, fmt.Errorf("failed to read enabled state of select %q: %s", step.Name, err)
	}
	if !enabled {
		return nil, fmt.Errorf("select %q is disabled", step.Name)
	}

	if !snapshot.multiple && len(step.Values) != 1 {
		return nil, fmt.Errorf("single select %q requires exactly one target value, got %d", step.Name, len(step.Values))
	}

	optionElements, err := member.GetElements(api.Selector{Using: "xpath", Value: formOptionsXPath})
	if err != nil {
		return nil, fmt.Errorf("failed to query options of select %q: %s", step.Name, err)
	}
	if len(optionElements) == 0 {
		return nil, fmt.Errorf("select %q has no options", step.Name)
	}

	seen := map[string]bool{}
	for _, optionElement := range optionElements {
		option := formControl{element: optionElement, tag: "option"}
		if err := readEnabledAndValue(&option, step.Name); err != nil {
			return nil, err
		}
		if seen[option.value] {
			return nil, fmt.Errorf("select %q has duplicate option value %q", step.Name, option.value)
		}
		seen[option.value] = true
		if !option.enabled {
			return nil, fmt.Errorf("option %q of select %q is disabled", option.value, step.Name)
		}
		selected, err := optionElement.IsSelected()
		if err != nil {
			return nil, fmt.Errorf("failed to read selection state of option %q of select %q: %s", option.value, step.Name, err)
		}
		option.selected = selected
		snapshot.options = append(snapshot.options, option)
	}
	if err := checkFormTargets(step, seen); err != nil {
		return nil, err
	}
	return snapshot, nil
}

func readEnabledAndValue(control *formControl, name string) error {
	value, err := control.element.GetAttribute("value")
	if err != nil {
		return fmt.Errorf("failed to read value of field %q: %s", name, err)
	}
	control.value = value
	enabled, err := control.element.IsEnabled()
	if err != nil {
		return fmt.Errorf("failed to read enabled state of field %q: %s", name, err)
	}
	control.enabled = enabled
	return nil
}

func checkFormTargets(step FormStep, available map[string]bool) error {
	for _, value := range step.Values {
		if !available[value] {
			return fmt.Errorf("no field named %q provides target value %q", step.Name, value)
		}
	}
	return nil
}
