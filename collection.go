package graphql

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strconv"
	"sync"

	"github.com/pkg/errors"
)

// ErrIncomplete is returned by Collection.Read when the collection has not
// captured every field selected by the request. The returned result is nil;
// a missing field is never reported as an explicit null.
var ErrIncomplete = errors.New("graphql: collection is missing a field selected by the request")

// Collection stores entities extracted from GraphQL responses. A Collection is
// held by the caller for one data source, lives only in memory and is safe for
// concurrent Collect and Read calls.
type Collection struct {
	mu sync.RWMutex

	// entities is keyed by the ordered (typename, id) pair; the zero key holds
	// the synthetic root Query object.
	entities map[entityKey]*entity
}

type entityKey struct {
	typename string
	id       string
}

type entity struct {
	fields map[string]interface{}
}

// entityRef marks a slot that points at a merged entity instead of holding an
// embedded value. It never escapes through Collection.Read.
type entityRef struct {
	key entityKey
}

// NewCollection makes an empty Collection.
func NewCollection() *Collection {
	col := &Collection{entities: map[entityKey]*entity{}}
	col.entities[entityKey{}] = &entity{fields: map[string]interface{}{}}
	return col
}

// compiled binds a parsed document to the variables carried by a request.
type compiled struct {
	doc   *document
	bound map[string]interface{}

	// store is set for Read projections and never touched during extraction.
	store map[entityKey]*entity
}

func compileRequest(req *Request) (*compiled, error) {
	doc, err := parseDocument(req.q)
	if err != nil {
		return nil, err
	}
	bound := map[string]interface{}{}
	for _, vd := range doc.op.vars {
		if v, ok := req.vars[vd.name]; ok {
			bound[vd.name] = v
			continue
		}
		if vd.def != nil {
			bound[vd.name] = literalValue(vd.def)
		}
	}
	c := &compiled{doc: doc, bound: bound}
	if err := c.checkDirectives(doc.op.dirs); err != nil {
		return nil, err
	}
	if err := c.precheckSelections(doc.op.sel); err != nil {
		return nil, err
	}
	for _, frag := range doc.frags {
		if err := c.checkDirectives(frag.dirs); err != nil {
			return nil, err
		}
		if err := c.precheckSelections(frag.sel); err != nil {
			return nil, err
		}
	}
	return c, nil
}

func literalValue(v *gqlValue) interface{} {
	switch v.kind {
	case valInt:
		return v.i
	case valString, valEnum:
		return v.s
	case valBool:
		return v.b
	case valNull:
		return nil
	case valList:
		out := make([]interface{}, len(v.list))
		for i, elem := range v.list {
			out[i] = literalValue(elem)
		}
		return out
	case valObject:
		out := map[string]interface{}{}
		for _, a := range v.obj {
			out[a.name] = literalValue(a.val)
		}
		return out
	default:
		return nil
	}
}

// precheckSelections makes sure every variable referenced from arguments and
// directives has a binding, and that skip/include conditions are boolean.
func (c *compiled) precheckSelections(sels []selection) error {
	for _, s := range sels {
		switch n := s.(type) {
		case *field:
			for _, a := range n.args {
				if _, err := c.evalValue(a.val); err != nil {
					return err
				}
			}
			if err := c.checkDirectives(n.dirs); err != nil {
				return err
			}
			if n.sel != nil {
				if err := c.precheckSelections(n.sel); err != nil {
					return err
				}
			}
		case *fragmentSpread:
			if err := c.checkDirectives(n.dirs); err != nil {
				return err
			}
			frag := c.doc.frags[n.name]
			if err := c.checkDirectives(frag.dirs); err != nil {
				return err
			}
			if err := c.precheckSelections(frag.sel); err != nil {
				return err
			}
		case *inlineFragment:
			if err := c.checkDirectives(n.dirs); err != nil {
				return err
			}
			if err := c.precheckSelections(n.sel); err != nil {
				return err
			}
		}
	}
	return nil
}

func (c *compiled) checkDirectives(dirs []*directive) error {
	for _, d := range dirs {
		if d.name != "skip" && d.name != "include" {
			return fmt.Errorf("graphql: directive @%s is not supported", d.name)
		}
		var cond *gqlValue
		for _, a := range d.args {
			if a.name == "if" {
				cond = a.val
			}
		}
		if cond == nil {
			return fmt.Errorf("graphql: directive @%s requires a boolean \"if\" argument", d.name)
		}
		v, err := c.evalValue(cond)
		if err != nil {
			return err
		}
		if _, ok := v.(bool); !ok {
			return fmt.Errorf("graphql: directive @%s condition must be a boolean", d.name)
		}
	}
	return nil
}

// directiveIncluded reports whether a selection carrying the directives stays
// in the effective selection set.
func (c *compiled) directiveIncluded(dirs []*directive) (bool, error) {
	included := true
	for _, d := range dirs {
		var cond *gqlValue
		for _, a := range d.args {
			if a.name == "if" {
				cond = a.val
			}
		}
		v, err := c.evalValue(cond)
		if err != nil {
			return false, err
		}
		b := v.(bool)
		if d.name == "skip" && b {
			included = false
		}
		if d.name == "include" && !b {
			included = false
		}
	}
	return included, nil
}

func (c *compiled) evalValue(v *gqlValue) (interface{}, error) {
	switch v.kind {
	case valVar:
		got, ok := c.bound[v.s]
		if !ok {
			return nil, fmt.Errorf("graphql: variable $%s is not bound", v.s)
		}
		return got, nil
	case valInt:
		return v.i, nil
	case valString, valEnum:
		return v.s, nil
	case valBool:
		return v.b, nil
	case valNull:
		return nil, nil
	case valList:
		out := make([]interface{}, len(v.list))
		for i, elem := range v.list {
			got, err := c.evalValue(elem)
			if err != nil {
				return nil, err
			}
			out[i] = got
		}
		return out, nil
	case valObject:
		out := map[string]interface{}{}
		for _, a := range v.obj {
			got, err := c.evalValue(a.val)
			if err != nil {
				return nil, err
			}
			out[a.name] = got
		}
		return out, nil
	default:
		return nil, fmt.Errorf("graphql: unsupported argument value")
	}
}

// fieldIdentity is the original field name plus its bound arguments; argument
// and input-object key order never affects it.
func (c *compiled) fieldIdentity(f *field) (string, error) {
	if len(f.args) == 0 {
		return f.name, nil
	}
	args := map[string]interface{}{}
	for _, a := range f.args {
		v, err := c.evalValue(a.val)
		if err != nil {
			return "", err
		}
		args[a.name] = v
	}
	canon, err := canonicalEncode(args)
	if err != nil {
		return "", err
	}
	return f.name + "(" + canon + ")", nil
}

func canonicalEncode(v interface{}) (string, error) {
	switch t := v.(type) {
	case nil:
		return "null", nil
	case bool:
		if t {
			return "true", nil
		}
		return "false", nil
	case string:
		return strconv.Quote(t), nil
	case int64:
		return strconv.FormatInt(t, 10), nil
	case int:
		return strconv.Itoa(t), nil
	case float64:
		if n, err := normalizeFloat(t); err == nil {
			return strconv.FormatInt(n, 10), nil
		}
		return strconv.FormatFloat(t, 'g', -1, 64), nil
	case json.Number:
		return t.String(), nil
	case []interface{}:
		var b []byte
		b = append(b, '[')
		for i, elem := range t {
			if i > 0 {
				b = append(b, ',')
			}
			part, err := canonicalEncode(elem)
			if err != nil {
				return "", err
			}
			b = append(b, part...)
		}
		b = append(b, ']')
		return string(b), nil
	case map[string]interface{}:
		keys := make([]string, 0, len(t))
		for k := range t {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		var b []byte
		b = append(b, '{')
		for i, k := range keys {
			if i > 0 {
				b = append(b, ',')
			}
			b = strconv.AppendQuote(b, k)
			b = append(b, ':')
			part, err := canonicalEncode(t[k])
			if err != nil {
				return "", err
			}
			b = append(b, part...)
		}
		b = append(b, '}')
		return string(b), nil
	default:
		return "", fmt.Errorf("graphql: value of type %T cannot be used as an argument", v)
	}
}

func normalizeFloat(f float64) (int64, error) {
	n := int64(f)
	if float64(n) != f {
		return 0, fmt.Errorf("not an integer")
	}
	return n, nil
}

// valuesEqual compares normalized response values; strings, booleans and null
// never compare equal across kinds, and entity references compare by identity.
func valuesEqual(a, b interface{}) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	switch at := a.(type) {
	case bool:
		bt, ok := b.(bool)
		return ok && at == bt
	case string:
		bt, ok := b.(string)
		return ok && at == bt
	case float64:
		return numericEqual(at, b)
	case int64:
		return numericEqual(float64(at), b)
	case int:
		return numericEqual(float64(at), b)
	case []interface{}:
		bt, ok := b.([]interface{})
		if !ok || len(at) != len(bt) {
			return false
		}
		for i := range at {
			if !valuesEqual(at[i], bt[i]) {
				return false
			}
		}
		return true
	case map[string]interface{}:
		bt, ok := b.(map[string]interface{})
		if !ok || len(at) != len(bt) {
			return false
		}
		for k, av := range at {
			bv, ok := bt[k]
			if !ok || !valuesEqual(av, bv) {
				return false
			}
		}
		return true
	case *entityRef:
		bt, ok := b.(*entityRef)
		return ok && at.key == bt.key
	default:
		return false
	}
}

func numericEqual(a float64, b interface{}) bool {
	switch bt := b.(type) {
	case float64:
		return a == bt
	case int64:
		return a == float64(bt)
	case int:
		return a == float64(bt)
	default:
		return false
	}
}

// cloneValue returns a detached copy of a leaf value so projections never
// expose stored slices or maps.
func cloneValue(v interface{}) interface{} {
	switch t := v.(type) {
	case []interface{}:
		out := make([]interface{}, len(t))
		for i, elem := range t {
			out[i] = cloneValue(elem)
		}
		return out
	case map[string]interface{}:
		out := make(map[string]interface{}, len(t))
		for k, elem := range t {
			out[k] = cloneValue(elem)
		}
		return out
	default:
		// nil, bool, string and all numbers are immutable Go values.
		return v
	}
}

// ---------------------------------------------------------------------------
// Selection expansion
// ---------------------------------------------------------------------------

// effectiveField is one occurrence of a field after fragment and directive
// expansion.
type effectiveField struct {
	field       *field
	conditioned bool // reached through a type-conditional fragment
}

// flatten expands a selection set. includeAll ignores type conditions (used to
// discover the unconditioned __typename); otherwise includeType decides
// whether each type-conditional fragment applies. Every occurrence of a shared
// fragment is walked independently.
func (c *compiled) flatten(sels []selection, includeAll bool, includeType func(string) bool) ([]effectiveField, bool, error) {
	var fields []effectiveField
	sawTypeCond := false
	var walk func([]selection, bool) error
	walk = func(nodes []selection, conditioned bool) error {
		for _, s := range nodes {
			switch n := s.(type) {
			case *field:
				ok, err := c.directiveIncluded(n.dirs)
				if err != nil {
					return err
				}
				if ok {
					fields = append(fields, effectiveField{field: n, conditioned: conditioned})
				}
			case *inlineFragment:
				ok, err := c.directiveIncluded(n.dirs)
				if err != nil {
					return err
				}
				if !ok {
					continue
				}
				if n.typeCond == "" {
					if err := walk(n.sel, conditioned); err != nil {
						return err
					}
					continue
				}
				sawTypeCond = true
				if includeAll || includeType(n.typeCond) {
					if err := walk(n.sel, true); err != nil {
						return err
					}
				}
			case *fragmentSpread:
				ok, err := c.directiveIncluded(n.dirs)
				if err != nil {
					return err
				}
				if !ok {
					continue
				}
				frag := c.doc.frags[n.name]
				ok, err = c.directiveIncluded(frag.dirs)
				if err != nil {
					return err
				}
				if !ok {
					continue
				}
				sawTypeCond = true
				if includeAll || includeType(frag.typeCond) {
					if err := walk(frag.sel, true); err != nil {
						return err
					}
				}
			}
		}
		return nil
	}
	if err := walk(sels, false); err != nil {
		return nil, false, err
	}
	return fields, sawTypeCond, nil
}

// unconditionedTypenameKey finds the response key of a __typename field that is
// selected without passing through a type condition.
func unconditionedTypenameKey(fields []effectiveField) string {
	for _, ef := range fields {
		if !ef.conditioned && ef.field.name == "__typename" {
			return ef.field.responseKey()
		}
	}
	return ""
}

func (f *field) responseKey() string {
	if f.alias != "" {
		return f.alias
	}
	return f.name
}

// prepareLevel expands one object level. On the extraction path the typename is
// read from the response object; missing required response keys fail there.
func (c *compiled) prepareLevel(data map[string]interface{}, sels []selection, isRoot bool) ([]effectiveField, error) {
	all, sawTypeCond, err := c.flatten(sels, true, nil)
	if err != nil {
		return nil, err
	}
	if len(all) == 0 {
		return nil, fmt.Errorf("graphql: selection set selects no fields")
	}
	tn, tnKnown, err := c.typenameFromResponse(data, all, isRoot)
	if err != nil {
		return nil, err
	}
	if sawTypeCond && !tnKnown {
		return nil, fmt.Errorf("graphql: type condition used where no unconditioned __typename is selected")
	}
	fields, _, err := c.flatten(sels, false, func(cond string) bool {
		return tnKnown && tn == cond
	})
	if err != nil {
		return nil, err
	}
	if len(fields) == 0 {
		return nil, fmt.Errorf("graphql: selection set selects no fields")
	}
	return fields, nil
}

func (c *compiled) typenameFromResponse(data map[string]interface{}, all []effectiveField, isRoot bool) (string, bool, error) {
	if isRoot {
		return "Query", true, nil
	}
	key := unconditionedTypenameKey(all)
	if key == "" {
		return "", false, nil
	}
	raw, ok := data[key]
	if !ok {
		return "", false, fmt.Errorf("graphql: response is missing selected key %q", key)
	}
	tn, ok := raw.(string)
	if !ok || tn == "" {
		return "", false, fmt.Errorf("graphql: __typename must be a non-empty string")
	}
	return tn, true, nil
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

type batch struct {
	updates map[entityKey]map[string]interface{}
}

func (b *batch) target(key entityKey) map[string]interface{} {
	m, ok := b.updates[key]
	if !ok {
		m = map[string]interface{}{}
		b.updates[key] = m
	}
	return m
}

// putField records one selected field, failing the whole extraction if the
// same entity field already carries a different normalized value.
func putField(target map[string]interface{}, key string, value interface{}) error {
	if existing, ok := target[key]; ok {
		if !valuesEqual(existing, value) {
			return fmt.Errorf("graphql: conflicting values collected for field %q", key)
		}
		return nil
	}
	target[key] = value
	return nil
}

func (c *compiled) extract(data map[string]interface{}) (*batch, error) {
	b := &batch{updates: map[entityKey]map[string]interface{}{}}
	root := b.target(entityKey{})
	fields, err := c.prepareLevel(data, c.doc.op.sel, true)
	if err != nil {
		return nil, err
	}
	for _, ef := range fields {
		if err := c.extractField(data, ef.field, root, b); err != nil {
			return nil, err
		}
	}
	return b, nil
}

// identityPair reports the ordered (typename, id) pair when the object selects
// non-empty string values for both, using original field names.
func identityPair(data map[string]interface{}, fields []effectiveField) (entityKey, bool, error) {
	typenameVal, idVal := "", ""
	hasTypeName, hasID := false, false
	for _, ef := range fields {
		raw, ok := data[ef.field.responseKey()]
		if !ok {
			continue
		}
		switch ef.field.name {
		case "__typename":
			if s, ok := raw.(string); ok && s != "" {
				typenameVal, hasTypeName = s, true
			}
		case "id":
			if s, ok := raw.(string); ok && s != "" {
				idVal, hasID = s, true
			}
		}
	}
	if hasTypeName && hasID {
		return entityKey{typename: typenameVal, id: idVal}, true, nil
	}
	return entityKey{}, false, nil
}

func (c *compiled) extractField(data map[string]interface{}, f *field, target map[string]interface{}, b *batch) error {
	key, err := c.fieldIdentity(f)
	if err != nil {
		return err
	}
	raw, ok := data[f.responseKey()]
	if !ok {
		return fmt.Errorf("graphql: response is missing selected key %q", f.responseKey())
	}
	value, err := c.extractValue(raw, f, b)
	if err != nil {
		return err
	}
	return putField(target, key, value)
}

func (c *compiled) extractValue(raw interface{}, f *field, b *batch) (interface{}, error) {
	if raw == nil {
		return nil, nil
	}
	if f.sel == nil {
		// A leaf field carries whatever JSON scalar shape the server sent
		// (including lists of scalars); without a schema there is no finer
		// shape check to make.
		return raw, nil
	}
	switch t := raw.(type) {
	case map[string]interface{}:
		return c.extractObject(t, f.sel, b)
	case []interface{}:
		// Lists are replaced wholesale; order, duplicates and null slots stay.
		out := make([]interface{}, len(t))
		for i, elem := range t {
			if elem == nil {
				continue
			}
			m, ok := elem.(map[string]interface{})
			if !ok {
				return nil, fmt.Errorf("graphql: list element for field %q must be an object", f.name)
			}
			v, err := c.extractObject(m, f.sel, b)
			if err != nil {
				return nil, err
			}
			out[i] = v
		}
		return out, nil
	default:
		return nil, fmt.Errorf("graphql: object field %q received a scalar value", f.name)
	}
}

// extractObject returns either an *entityRef for a merged entity or an embedded
// map for a value without a non-empty typename/id pair.
func (c *compiled) extractObject(data map[string]interface{}, sels []selection, b *batch) (interface{}, error) {
	fields, err := c.prepareLevel(data, sels, false)
	if err != nil {
		return nil, err
	}
	if key, ok, err := identityPair(data, fields); err != nil {
		return nil, err
	} else if ok {
		target := b.target(key)
		for _, ef := range fields {
			if err := c.extractField(data, ef.field, target, b); err != nil {
				return nil, err
			}
		}
		return &entityRef{key: key}, nil
	}
	embedded := map[string]interface{}{}
	for _, ef := range fields {
		if err := c.extractField(data, ef.field, embedded, b); err != nil {
			return nil, err
		}
	}
	return embedded, nil
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

func (col *Collection) commit(b *batch) {
	col.mu.Lock()
	defer col.mu.Unlock()
	for key, fields := range b.updates {
		e, ok := col.entities[key]
		if !ok {
			e = &entity{fields: map[string]interface{}{}}
			col.entities[key] = e
		}
		// Merge field by field; replacing a whole snapshot would erase fields
		// committed by another request that was in flight at the same time.
		for fk, v := range fields {
			e.fields[fk] = v
		}
	}
}

// ---------------------------------------------------------------------------
// Read / projection
// ---------------------------------------------------------------------------

// Read projects the collected entities back into the shape of the request
// query. It never performs any network access. If the collection is missing
// any selected field, it returns nil and an error wrapping ErrIncomplete.
func (col *Collection) Read(req *Request) (map[string]interface{}, error) {
	c, err := compileRequest(req)
	if err != nil {
		return nil, err
	}
	col.mu.RLock()
	defer col.mu.RUnlock()
	c.store = col.entities
	root := col.entities[entityKey{}]
	out, err := c.projectEntity(root, c.doc.op.sel, true)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *compiled) projectLevel(sels []selection, isRoot bool, stored map[string]interface{}) ([]effectiveField, error) {
	all, sawTypeCond, err := c.flatten(sels, true, nil)
	if err != nil {
		return nil, err
	}
	if len(all) == 0 {
		return nil, fmt.Errorf("graphql: selection set selects no fields")
	}
	tn, tnKnown := c.typenameFromStore(stored, all, isRoot)
	if sawTypeCond && !tnKnown {
		return nil, ErrIncomplete
	}
	fields, _, err := c.flatten(sels, false, func(cond string) bool {
		return tnKnown && tn == cond
	})
	if err != nil {
		return nil, err
	}
	if len(fields) == 0 {
		return nil, fmt.Errorf("graphql: selection set selects no fields")
	}
	return fields, nil
}

func (c *compiled) typenameFromStore(stored map[string]interface{}, all []effectiveField, isRoot bool) (string, bool) {
	if isRoot {
		return "Query", true
	}
	if unconditionedTypenameKey(all) == "" {
		return "", false
	}
	raw, ok := stored["__typename"]
	if !ok {
		return "", false
	}
	tn, ok := raw.(string)
	if !ok || tn == "" {
		return "", false
	}
	return tn, true
}

func (c *compiled) projectEntity(e *entity, sels []selection, isRoot bool) (map[string]interface{}, error) {
	fields, err := c.projectLevel(sels, isRoot, e.fields)
	if err != nil {
		return nil, err
	}
	out := map[string]interface{}{}
	for _, ef := range fields {
		if err := c.projectField(e.fields, ef.field, out); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func (c *compiled) projectEmbedded(m map[string]interface{}, sels []selection) (map[string]interface{}, error) {
	fields, err := c.projectLevel(sels, false, m)
	if err != nil {
		return nil, err
	}
	out := map[string]interface{}{}
	for _, ef := range fields {
		if err := c.projectField(m, ef.field, out); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func (c *compiled) projectField(stored map[string]interface{}, f *field, out map[string]interface{}) error {
	key, err := c.fieldIdentity(f)
	if err != nil {
		return err
	}
	value, ok := stored[key]
	if !ok {
		return ErrIncomplete
	}
	projected, err := c.projectValue(value, f)
	if err != nil {
		return err
	}
	if existing, dup := out[f.responseKey()]; dup {
		if !valuesEqual(existing, projected) {
			return fmt.Errorf("graphql: conflicting projections for response key %q", f.responseKey())
		}
		return nil
	}
	out[f.responseKey()] = projected
	return nil
}

func (c *compiled) projectValue(value interface{}, f *field) (interface{}, error) {
	if value == nil {
		return nil, nil
	}
	if f.sel == nil {
		// Clone leaf values so callers can never mutate stored slices or maps.
		return cloneValue(value), nil
	}
	switch t := value.(type) {
	case *entityRef:
		e, ok := c.store[t.key]
		if !ok {
			return nil, ErrIncomplete
		}
		return c.projectEntity(e, f.sel, false)
	case map[string]interface{}:
		return c.projectEmbedded(t, f.sel)
	case []interface{}:
		// Fresh slice: the caller can mutate the projection freely.
		out := make([]interface{}, len(t))
		for i, elem := range t {
			if elem == nil {
				continue
			}
			switch et := elem.(type) {
			case *entityRef:
				e, ok := c.store[et.key]
				if !ok {
					return nil, ErrIncomplete
				}
				v, err := c.projectEntity(e, f.sel, false)
				if err != nil {
					return nil, err
				}
				out[i] = v
			case map[string]interface{}:
				v, err := c.projectEmbedded(et, f.sel)
				if err != nil {
					return nil, err
				}
				out[i] = v
			default:
				return nil, fmt.Errorf("graphql: stored list element for field %q has the wrong shape", f.name)
			}
		}
		return out, nil
	default:
		return nil, fmt.Errorf("graphql: stored scalar for object field %q has the wrong shape", f.name)
	}
}

// ---------------------------------------------------------------------------
// Client.Collect
// ---------------------------------------------------------------------------

// Collect executes the single-query request over HTTP exactly like Run, then
// deduplicates and merges the response entities into dst. Local data is never
// substituted for the network request. dst is only updated after a complete,
// successful extraction; GraphQL errors, read errors, cancellation and late
// entity conflicts leave the previous collection intact.
func (c *Client) Collect(ctx context.Context, req *Request, dst *Collection) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
	}
	if dst == nil {
		return errors.New("graphql: Collect requires a non-nil Collection")
	}
	if len(req.files) > 0 {
		return errors.New("graphql: Collect does not support file uploads")
	}
	compiled, err := compileRequest(req)
	if err != nil {
		return err
	}
	status, body, err := c.postJSON(ctx, req)
	if err != nil {
		return err
	}
	if status != http.StatusOK {
		return fmt.Errorf("graphql: server returned a non-200 status code: %v", status)
	}
	dec := json.NewDecoder(bytes.NewReader(body))
	var gr struct {
		Data   json.RawMessage `json:"data"`
		Errors []graphErr      `json:"errors"`
	}
	if err := dec.Decode(&gr); err != nil {
		return errors.Wrap(err, "decoding response")
	}
	var extra json.RawMessage
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return errors.New("graphql: response contains trailing data")
		}
		return errors.Wrap(err, "decoding response")
	}
	if len(gr.Errors) > 0 {
		return gr.Errors[0]
	}
	if len(gr.Data) == 0 {
		return errors.New("graphql: response contains no data")
	}
	var data interface{}
	if err := json.Unmarshal(gr.Data, &data); err != nil {
		return errors.Wrap(err, "decoding response data")
	}
	dataMap, ok := data.(map[string]interface{})
	if !ok {
		return errors.New("graphql: response data must be an object")
	}
	b, err := compiled.extract(dataMap)
	if err != nil {
		return err
	}
	dst.commit(b)
	return nil
}

// postJSON builds and transmits the same JSON request body used by Run, and
// always closes the response body.
func (c *Client) postJSON(ctx context.Context, req *Request) (int, []byte, error) {
	var requestBody bytes.Buffer
	requestBodyObj := struct {
		Query     string                 `json:"query"`
		Variables map[string]interface{} `json:"variables"`
	}{
		Query:     req.q,
		Variables: req.vars,
	}
	if err := json.NewEncoder(&requestBody).Encode(requestBodyObj); err != nil {
		return 0, nil, errors.Wrap(err, "encode body")
	}
	c.logf(">> variables: %v", req.vars)
	c.logf(">> query: %s", req.q)
	r, err := http.NewRequest(http.MethodPost, c.endpoint, &requestBody)
	if err != nil {
		return 0, nil, err
	}
	r.Close = c.closeReq
	r.Header.Set("Content-Type", "application/json; charset=utf-8")
	r.Header.Set("Accept", "application/json; charset=utf-8")
	for key, values := range req.Header {
		for _, value := range values {
			r.Header.Add(key, value)
		}
	}
	c.logf(">> headers: %v", r.Header)
	r = r.WithContext(ctx)
	res, err := c.httpClient.Do(r)
	if err != nil {
		return 0, nil, err
	}
	defer res.Body.Close()
	raw, err := io.ReadAll(res.Body)
	if err != nil {
		return res.StatusCode, nil, errors.Wrap(err, "reading body")
	}
	c.logf("<< %s", string(raw))
	return res.StatusCode, raw, nil
}
