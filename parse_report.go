// Copyright 2020 Foolin

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package pagser

import (
	"errors"
	"fmt"
	"math"
	"reflect"
	"strconv"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"github.com/spf13/cast"
)

// reportRequiredTagName marks a field as required: when its selector matches
// no DOM node, the report records a "missing" issue and its handler is never
// called. A node that exists but carries empty text still counts as matched.
const reportRequiredTagName = "pagserRequired"

// ReportIssueCategory classifies a reported field problem.
type ReportIssueCategory string

const (
	// ReportIssueMissing means a required field selector matched no node.
	ReportIssueMissing ReportIssueCategory = "missing"
	// ReportIssueConversion means a value could not be converted to the
	// target field type, including integer/float bit-width overflow.
	ReportIssueConversion ReportIssueCategory = "conversion"
	// ReportIssueFunc means a struct method or registered function returned
	// an error. The original error remains available via errors.Unwrap.
	ReportIssueFunc ReportIssueCategory = "func"
	// ReportIssueConfig means an invalid parser/target configuration.
	ReportIssueConfig ReportIssueCategory = "config"
)

// ReportIssue describes one failed field extraction.
type ReportIssue struct {
	// FieldPath is the Go field path, e.g. Groups[1].Items[2].Price.
	// Slice indexes always use the original DOM order of the selector hits,
	// even after earlier invalid records are filtered out of the output.
	FieldPath string
	// Tag is the raw struct tag value attached to the field.
	Tag string
	// Category is the issue category.
	Category ReportIssueCategory

	// err is the original, unwrappable error. It stays unexported so callers
	// use errors.Unwrap / errors.Is / errors.As instead of depending on a
	// concrete field.
	err error
}

// Error implements the error interface.
func (i ReportIssue) Error() string {
	return fmt.Sprintf("pagser report issue at %s: category=%s tag=%q: %v", i.FieldPath, i.Category, i.Tag, i.err)
}

// Unwrap returns the original error so errors.Is and errors.As work.
func (i ReportIssue) Unwrap() error {
	return i.err
}

// ParseReport holds the diagnostics of a report-mode parse.
type ParseReport struct {
	// Issues lists every field problem collected during the parse, in
	// processing order. Records that survive are committed to the caller's
	// target struct; invalid records are dropped but their issues remain.
	Issues []ReportIssue
}

var (
	// errRecordInvalid marks that the record currently being assembled has one
	// or more reported issues and must not be committed to its parent list or
	// field. It is never returned to callers directly.
	errRecordInvalid = errors.New("pagser: record invalidated by reported issues")
	// errMissingRequired is wrapped by every "missing" issue.
	errMissingRequired = errors.New("required field selector matched no nodes")
)

var (
	goquerySelectionType = reflect.TypeOf((*goquery.Selection)(nil)).Elem()
	stringSliceType      = reflect.TypeOf([]string(nil))
	errorType            = reflect.TypeOf((*error)(nil)).Elem()
	emptyInterfaceType   = reflect.TypeOf((*interface{})(nil)).Elem()
)

// ParseReport parses an HTML document in report mode.
//
// Unlike Parse, a failed field extraction no longer aborts the whole batch:
// struct elements of []T and []*T are record boundaries. A record with any
// failed field is dropped from the output while its problems are collected
// into the report; later records keep processing in original order. Failures
// of the root record's own fields are returned as an error and the caller's
// target value is left untouched.
func (p *Pagser) ParseReport(v interface{}, document string) (*ParseReport, error) {
	reader, err := goquery.NewDocumentFromReader(strings.NewReader(document))
	if err != nil {
		return nil, err
	}
	return p.ParseSelectionReport(v, reader.Selection)
}

// ParseSelectionReport is the report-mode entry point for an existing
// goquery.Selection. It shares the same semantics as ParseReport.
func (p *Pagser) ParseSelectionReport(v interface{}, selection *goquery.Selection) (report *ParseReport, err error) {
	if selection == nil {
		return nil, errors.New("pagser: selection must not be nil")
	}

	objRefValue := reflect.ValueOf(v)
	if objRefValue.Kind() != reflect.Ptr {
		return nil, fmt.Errorf("%v is non-pointer", reflect.TypeOf(v))
	}
	if objRefValue.IsNil() {
		return nil, fmt.Errorf("%v is nil", objRefValue.Type())
	}
	if objRefValue.Elem().Kind() != reflect.Struct {
		return nil, fmt.Errorf("pagser: report target must be a pointer to struct, got %v", objRefValue.Type())
	}

	// All configuration problems are rejected before any field handler runs.
	if err = p.validateTarget(objRefValue.Type().Elem()); err != nil {
		return nil, err
	}

	// Work on a detached candidate: prefilled values are shallow-copied so
	// root methods can read untagged configuration, while the caller's target
	// is only committed once the root record finishes without failure.
	rootCandidate := reflect.New(objRefValue.Type().Elem())
	rootCandidate.Elem().Set(objRefValue.Elem())

	engine := &reportEngine{p: p}
	parseErr := engine.fillStruct(rootCandidate, nil, selection, "")
	report = &ParseReport{Issues: engine.issues}
	if errors.Is(parseErr, errRecordInvalid) {
		// Root record failure: do not commit anything into the caller's target.
		rootErr := fmt.Errorf("pagser: root record failed with %d issue(s)", len(engine.issues))
		if len(engine.issues) > 0 {
			// Wrap the first issue so the original cause stays unwrappable.
			rootErr = fmt.Errorf("%w: %w", rootErr, engine.issues[0])
		}
		return report, rootErr
	}
	if parseErr != nil {
		return report, parseErr
	}

	objRefValue.Elem().Set(rootCandidate.Elem())
	return report, nil
}

// reportEngine carries the diagnostics of one report-mode parse. Every parse
// creates a fresh engine, so reports and outputs never share state across
// calls.
type reportEngine struct {
	p      *Pagser
	issues []ReportIssue
}

// addIssue records one field problem against the nearest enclosing record.
func (e *reportEngine) addIssue(fieldPath, rawTag string, category ReportIssueCategory, cause error) {
	e.issues = append(e.issues, ReportIssue{
		FieldPath: fieldPath,
		Tag:       rawTag,
		Category:  category,
		err:       cause,
	})
}

// execFunc runs a field handler using the same lookup order as the legacy
// parser: current object, nearest ancestors up to the root, then registered
// functions. Unlike the legacy path it wraps errors with %w so the handler's
// original, identity-bearing error remains reachable via errors.Is/As.
func (e *reportEngine) execFunc(objRefValue reflect.Value, ancestors []reflect.Value, selTag *tagTokenizer, node *goquery.Selection) (interface{}, error) {
	if callMethod := findMethod(objRefValue, selTag.FuncName); callMethod.IsValid() {
		return execMethodReport(callMethod, selTag, node)
	}
	for i := len(ancestors) - 1; i >= 0; i-- {
		if callMethod := findMethod(ancestors[i], selTag.FuncName); callMethod.IsValid() {
			return execMethodReport(callMethod, selTag, node)
		}
	}
	if fn, ok := e.p.mapFuncs.Load(selTag.FuncName); ok {
		outValue, err := fn.(CallFunc)(node, selTag.FuncParams...)
		if err != nil {
			return nil, fmt.Errorf("call registered func %s: %w", selTag.FuncName, err)
		}
		return outValue, nil
	}
	return nil, fmt.Errorf("not found method %s", selTag.FuncName)
}

// execMethodReport invokes a struct method, wrapping its returned error with
// %w so handler identity survives. Reflection panics are deliberately not
// recovered: misconfiguration must surface as a real panic, not a record issue.
func execMethodReport(callMethod reflect.Value, selTag *tagTokenizer, node *goquery.Selection) (interface{}, error) {
	callReturns := callMethod.Call([]reflect.Value{reflect.ValueOf(node)})
	if len(callReturns) <= 0 {
		return nil, fmt.Errorf("method %s not return any value", selTag.FuncName)
	}
	if len(callReturns) > 1 {
		if err, ok := callReturns[len(callReturns)-1].Interface().(error); ok && err != nil {
			return nil, fmt.Errorf("method %s: %w", selTag.FuncName, err)
		}
	}
	return callReturns[0].Interface(), nil
}

// fillStruct populates the struct pointed to by objRefValue from selection.
//
// ancestors holds the candidate pointers of enclosing records, nearest
// parent last; it is the method lookup chain used by findAndExecFunc. prefix
// is the FieldPath prefix for the struct's own fields.
//
// It returns errRecordInvalid when the current record collected issues, a
// configuration error for unrecoverable problems, or nil on success.
func (e *reportEngine) fillStruct(objRefValue reflect.Value, ancestors []reflect.Value, selection *goquery.Selection, prefix string) error {
	structType := objRefValue.Type().Elem()
	recordInvalid := false

	for i := 0; i < structType.NumField(); i++ {
		fieldType := structType.Field(i)
		fieldValue := objRefValue.Elem().Field(i)

		rawTag, tagOk := fieldType.Tag.Lookup(e.p.Config.TagName)
		if !tagOk || rawTag == ignoreSymbol {
			continue
		}
		fieldPath := prefix + fieldType.Name

		tag, err := e.p.cachedTag(rawTag)
		if err != nil {
			return err
		}

		node := selection
		if tag.Selector != "" {
			node = selection.Find(tag.Selector)
		}

		// Missing node: required fields report "missing" and skip their
		// handler; optional fields keep the ordinary zero-value meaning.
		if node.Size() == 0 {
			if fieldType.Tag.Get(reportRequiredTagName) == "true" {
				e.addIssue(fieldPath, rawTag, ReportIssueMissing, fmt.Errorf("%w: %s", errMissingRequired, rawTag))
				recordInvalid = true
			}
			continue
		}

		if tag.FuncName != "" {
			callOutValue, callErr := e.execFunc(objRefValue, ancestors, tag, node)
			if callErr != nil {
				e.addIssue(fieldPath, rawTag, ReportIssueFunc, callErr)
				recordInvalid = true
				continue
			}
			if subNode, ok := callOutValue.(*goquery.Selection); ok {
				// Selection-returning handlers replace the working node, the
				// same way the legacy parser does.
				node = subNode
			} else {
				converted, convErr := strictConvert(fieldValue.Type(), callOutValue)
				if convErr != nil {
					e.addIssue(fieldPath, rawTag, ReportIssueConversion, convErr)
					recordInvalid = true
					continue
				}
				fieldValue.Set(converted)
				continue
			}
		}

		// The current candidate becomes the nearest ancestor of its children.
		// A fresh backing array keeps sibling branches from sharing state.
		childAncestors := make([]reflect.Value, 0, len(ancestors)+1)
		childAncestors = append(childAncestors, ancestors...)
		childAncestors = append(childAncestors, objRefValue)

		switch {
		case fieldValue.Kind() == reflect.Ptr:
			subModel := reflect.New(fieldType.Type.Elem())
			subErr := e.fillStruct(subModel, childAncestors, node, fieldPath+".")
			if errors.Is(subErr, errRecordInvalid) {
				recordInvalid = true
				continue
			}
			if subErr != nil {
				return subErr
			}
			fieldValue.Set(subModel)
		case fieldValue.Kind() == reflect.Slice:
			if subErr := e.fillSlice(fieldValue, childAncestors, node, fieldPath, rawTag); subErr != nil {
				if errors.Is(subErr, errRecordInvalid) {
					recordInvalid = true
					continue
				}
				return subErr
			}
		case fieldValue.Kind() == reflect.Struct:
			subModel := reflect.New(fieldType.Type)
			subErr := e.fillStruct(subModel, childAncestors, node, fieldPath+".")
			if errors.Is(subErr, errRecordInvalid) {
				recordInvalid = true
				continue
			}
			if subErr != nil {
				return subErr
			}
			fieldValue.Set(subModel.Elem())
		default:
			converted, convErr := strictConvert(fieldType.Type, strings.TrimSpace(node.Text()))
			if convErr != nil {
				e.addIssue(fieldPath, rawTag, ReportIssueConversion, convErr)
				recordInvalid = true
				continue
			}
			fieldValue.Set(converted)
		}
	}

	if recordInvalid {
		return errRecordInvalid
	}
	return nil
}

// fillSlice populates a slice field. Struct and pointer-to-struct elements are
// independent record boundaries: invalid elements are skipped while later
// elements keep processing, and reported indexes follow the original DOM
// order instead of the compacted output position.
func (e *reportEngine) fillSlice(fieldValue reflect.Value, ancestors []reflect.Value, node *goquery.Selection, fieldPath, rawTag string) error {
	sliceType := fieldValue.Type()
	itemType := sliceType.Elem()
	output := reflect.MakeSlice(sliceType, 0, node.Size())
	recordInvalid := false
	var fatalErr error

	node.EachWithBreak(func(i int, subNode *goquery.Selection) bool {
		itemPath := fieldPath + "[" + strconv.Itoa(i) + "]"

		switch {
		case itemType.Kind() == reflect.Struct:
			itemCandidate := reflect.New(itemType)
			itemErr := e.fillStruct(itemCandidate, ancestors, subNode, itemPath+".")
			if errors.Is(itemErr, errRecordInvalid) {
				return true
			}
			if itemErr != nil {
				fatalErr = itemErr
				return false
			}
			output = reflect.Append(output, itemCandidate.Elem())
		case itemType.Kind() == reflect.Ptr && itemType.Elem().Kind() == reflect.Struct:
			// Fresh allocation per element keeps pointer records independent.
			itemCandidate := reflect.New(itemType.Elem())
			itemErr := e.fillStruct(itemCandidate, ancestors, subNode, itemPath+".")
			if errors.Is(itemErr, errRecordInvalid) {
				return true
			}
			if itemErr != nil {
				fatalErr = itemErr
				return false
			}
			output = reflect.Append(output, itemCandidate)
		default:
			converted, convErr := strictConvert(itemType, strings.TrimSpace(subNode.Text()))
			if convErr != nil {
				// Not a record boundary: keep collecting the remaining
				// element diagnostics; the whole slice is dropped at the end.
				e.addIssue(itemPath, rawTag, ReportIssueConversion, convErr)
				recordInvalid = true
				return true
			}
			output = reflect.Append(output, converted)
		}
		return true
	})

	if fatalErr != nil {
		return fatalErr
	}
	if recordInvalid {
		return errRecordInvalid
	}
	fieldValue.Set(output)
	return nil
}

// cachedToken parses a struct tag through the existing tokenizer, sharing the
// tag cache used by the legacy parser.
func (p *Pagser) cachedTag(tagValue string) (*tagTokenizer, error) {
	if cached, ok := p.mapTags.Load(tagValue); ok && cached != nil {
		return cached.(*tagTokenizer), nil
	}
	tag, err := p.newTag(tagValue)
	if err != nil {
		return nil, err
	}
	p.mapTags.Store(tagValue, tag)
	return tag, nil
}

// validateTarget verifies the whole reachable configuration before any field
// handler can run. It first collects every struct type and handler name
// reachable through tagged fields, then checks every referenced method's
// signature, so an illegal receiver method can never slip through because its
// first argument was not a *goquery.Selection.
func (p *Pagser) validateTarget(rootType reflect.Type) error {
	types := map[reflect.Type]bool{}
	funcNames := map[string]bool{}
	if err := p.collectTypes(rootType, types, funcNames); err != nil {
		return err
	}
	for structType := range types {
		// Use the Value method set, whose Type excludes the receiver: this is
		// exactly the form findMethod/execMethodReport invoke at parse time.
		candidate := reflect.New(structType)
		for name := range funcNames {
			if method := candidate.MethodByName(name); method.IsValid() {
				if sigErr := validateMethodSignature(name, method.Type()); sigErr != nil {
					return fmt.Errorf("pagser config error: %w", sigErr)
				}
			}
		}
	}
	return nil
}

// collectTypes walks every reachable tagged field, recording struct types and
// referenced handler names while rejecting unexported tagged fields, bad tag
// syntax and unsupported target shapes.
func (p *Pagser) collectTypes(structType reflect.Type, types map[reflect.Type]bool, funcNames map[string]bool) error {
	if types[structType] {
		return nil
	}
	types[structType] = true

	for i := 0; i < structType.NumField(); i++ {
		field := structType.Field(i)
		rawTag, ok := field.Tag.Lookup(p.Config.TagName)
		if !ok || rawTag == ignoreSymbol {
			continue
		}

		if !field.IsExported() {
			return fmt.Errorf("pagser config error: unexported field %s.%s must not carry a %s tag",
				structType.Name(), field.Name, p.Config.TagName)
		}

		tag, err := p.cachedTag(rawTag)
		if err != nil {
			return err
		}
		if tag.FuncName != "" {
			funcNames[tag.FuncName] = true
		}

		if err := validateFieldShape(structType, field); err != nil {
			return err
		}

		switch field.Type.Kind() {
		case reflect.Struct:
			if err := p.collectTypes(field.Type, types, funcNames); err != nil {
				return err
			}
		case reflect.Ptr:
			if err := p.collectTypes(field.Type.Elem(), types, funcNames); err != nil {
				return err
			}
		case reflect.Slice:
			elemType := field.Type.Elem()
			if elemType.Kind() == reflect.Ptr {
				elemType = elemType.Elem()
			}
			if elemType.Kind() == reflect.Struct {
				if err := p.collectTypes(elemType, types, funcNames); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// validateFieldShape rejects target kinds the parser cannot fill.
func validateFieldShape(structType reflect.Type, field reflect.StructField) error {
	unsupported := func() error {
		return fmt.Errorf("pagser config error: unsupported target type %s for field %s.%s",
			field.Type, structType.Name(), field.Name)
	}
	switch field.Type.Kind() {
	case reflect.Bool, reflect.String, reflect.Interface:
		return nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return nil
	case reflect.Float32, reflect.Float64:
		return nil
	case reflect.Struct:
		return nil
	case reflect.Ptr:
		if field.Type.Elem().Kind() != reflect.Struct {
			return unsupported()
		}
		return nil
	case reflect.Slice:
		elemType := field.Type.Elem()
		if elemType.Kind() == reflect.Ptr {
			if elemType.Elem().Kind() != reflect.Struct {
				return unsupported()
			}
			return nil
		}
		switch elemType.Kind() {
		case reflect.Bool, reflect.String, reflect.Interface,
			reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
			reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
			reflect.Float32, reflect.Float64, reflect.Struct:
			return nil
		default:
			return unsupported()
		}
	default:
		return unsupported()
	}
}

// validateMethodSignature enforces the documented handler signature, where
// methodType is the method's function type with the receiver already excluded
// (the type returned by reflect.Value.MethodByName(...).Type()):
//
//	Method(node *goquery.Selection, args ...string) (out interface{}, err error)
//
// A single return value of interface{} is also accepted.
func validateMethodSignature(name string, methodType reflect.Type) error {
	mt := methodType
	if mt.NumIn() != 2 || !mt.IsVariadic() {
		return fmt.Errorf("method %s must take (*goquery.Selection, ...string)", name)
	}
	if mt.In(0) != reflect.PtrTo(goquerySelectionType) {
		return fmt.Errorf("method %s first argument must be *goquery.Selection", name)
	}
	if mt.In(1) != stringSliceType {
		return fmt.Errorf("method %s variadic argument must be ...string", name)
	}
	if mt.NumOut() < 1 || mt.NumOut() > 2 || mt.Out(0) != emptyInterfaceType {
		return fmt.Errorf("method %s must return (interface{}, error) or interface{}", name)
	}
	if mt.NumOut() == 2 && mt.Out(1) != errorType {
		return fmt.Errorf("method %s second return value must be error", name)
	}
	return nil
}

// strictConvert converts v into a reflect.Value assignable to targetType.
// Numeric conversions are checked against the target's actual bit width, so
// values that would be silently truncated by reflection are reported instead.
func strictConvert(targetType reflect.Type, v interface{}) (reflect.Value, error) {
	result := reflect.New(targetType).Elem()
	switch targetType.Kind() {
	case reflect.String:
		s, err := cast.ToStringE(v)
		if err != nil {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s: %w", v, targetType, err)
		}
		result.SetString(s)
		return result, nil
	case reflect.Bool:
		b, err := cast.ToBoolE(v)
		if err != nil {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s: %w", v, targetType, err)
		}
		result.SetBool(b)
		return result, nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		i64, err := cast.ToInt64E(v)
		if err != nil {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s: %w", v, targetType, err)
		}
		if frac, ok := floatFraction(v); ok && frac != 0 {
			return reflect.Value{}, fmt.Errorf("cannot convert fractional value %#v to integer %s", v, targetType)
		}
		bits := targetType.Bits()
		if bits < 64 {
			min := int64(-1) << (bits - 1)
			max := int64(1)<<(bits-1) - 1
			if i64 < min || i64 > max {
				return reflect.Value{}, fmt.Errorf("value %d overflows %s (range %d..%d)", i64, targetType, min, max)
			}
		}
		result.SetInt(i64)
		return result, nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		u64, err := cast.ToUint64E(v)
		if err != nil {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s: %w", v, targetType, err)
		}
		if frac, ok := floatFraction(v); ok && frac != 0 {
			return reflect.Value{}, fmt.Errorf("cannot convert fractional value %#v to unsigned integer %s", v, targetType)
		}
		if bits := targetType.Bits(); bits < 64 {
			max := uint64(1)<<bits - 1
			if u64 > max {
				return reflect.Value{}, fmt.Errorf("value %d overflows %s (range 0..%d)", u64, targetType, max)
			}
		}
		result.SetUint(u64)
		return result, nil
	case reflect.Float32, reflect.Float64:
		f64, err := cast.ToFloat64E(v)
		if err != nil {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s: %w", v, targetType, err)
		}
		if targetType.Bits() == 32 && !math.IsNaN(f64) {
			if f64 > math.MaxFloat32 || f64 < -math.MaxFloat32 {
				return reflect.Value{}, fmt.Errorf("value %v overflows %s", f64, targetType)
			}
		}
		result.SetFloat(f64)
		return result, nil
	case reflect.Slice:
		rv := reflect.ValueOf(v)
		if !rv.IsValid() || (rv.Kind() != reflect.Slice && rv.Kind() != reflect.Array) {
			return reflect.Value{}, fmt.Errorf("cannot convert %#v to %s", v, targetType)
		}
		output := reflect.MakeSlice(targetType, 0, rv.Len())
		for j := 0; j < rv.Len(); j++ {
			item, err := strictConvert(targetType.Elem(), rv.Index(j).Interface())
			if err != nil {
				return reflect.Value{}, fmt.Errorf("slice element %d: %w", j, err)
			}
			output = reflect.Append(output, item)
		}
		return output, nil
	case reflect.Interface:
		rv := reflect.ValueOf(v)
		if !rv.IsValid() || !rv.Type().AssignableTo(targetType) {
			return reflect.Value{}, fmt.Errorf("cannot assign %#v to %s", v, targetType)
		}
		return rv, nil
	default:
		rv := reflect.ValueOf(v)
		if rv.IsValid() && rv.Type().AssignableTo(targetType) {
			return rv, nil
		}
		return reflect.Value{}, fmt.Errorf("cannot assign %#v to %s", v, targetType)
	}
}

// floatFraction reports the fractional part when v is a floating-point value.
func floatFraction(v interface{}) (float64, bool) {
	switch f := v.(type) {
	case float32:
		return float64(f) - math.Trunc(float64(f)), true
	case float64:
		return f - math.Trunc(f), true
	default:
		return 0, false
	}
}
