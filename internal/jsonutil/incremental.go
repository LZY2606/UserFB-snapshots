package jsonutil

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"reflect"
	"strconv"
)

// ParsePath validates a GraphQL incremental response path and converts it into
// a slice of string field names and non-negative int array indexes.
func ParsePath(raw []any) error {
	for i, entry := range raw {
		switch e := entry.(type) {
		case string:
			// Response field name (aliases included).
		case json.Number:
			n, err := strconv.Atoi(e.String())
			if err != nil || n < 0 {
				return fmt.Errorf("invalid array index %q in incremental path", e.String())
			}
			raw[i] = n
		default:
			return fmt.Errorf("unsupported incremental path entry %v at segment %d", entry, i)
		}
	}
	return nil
}

// change is a single recorded mutation that can be reverted on rollback.
type change struct {
	target reflect.Value
	old    reflect.Value
}

// Transaction applies the incremental items of one MIME part as a whole.
// If any item fails, Rollback restores the root value to the state it had
// when the transaction started, including the identity of every pointer that
// existed beforehand.
type Transaction struct {
	root    reflect.Value
	changes []change
}

// NewTransaction starts a transaction on the pointer to the query struct.
func NewTransaction(v any) *Transaction {
	rv := reflect.ValueOf(v)
	if rv.Kind() != reflect.Ptr {
		panic(fmt.Sprintf("jsonutil: cannot apply incremental data into non-pointer %T", v))
	}
	return &Transaction{root: rv.Elem()}
}

// Rollback reverts every mutation recorded by the transaction.
func (t *Transaction) Rollback() {
	for i := len(t.changes) - 1; i >= 0; i-- {
		c := t.changes[i]
		c.target.Set(c.old)
	}
	t.changes = nil
}

// set assigns nv to target while remembering the previous value.
func (t *Transaction) set(target, nv reflect.Value) {
	old := reflect.New(target.Type()).Elem()
	old.Set(target)
	t.changes = append(t.changes, change{target: target, old: old})
	target.Set(nv)
}

// navigate follows path through existing values only. It never creates
// intermediate parents and refuses to cross a nil pointer or an array hole.
func (t *Transaction) navigate(path []any) (reflect.Value, error) {
	v := t.root
	for i, entry := range path {
		if v.Kind() == reflect.Ptr {
			if v.IsNil() {
				return reflect.Value{}, fmt.Errorf("incremental path crosses a null value at segment %d", i)
			}
			v = v.Elem()
		}
		switch e := entry.(type) {
		case string:
			if v.Kind() != reflect.Struct {
				return reflect.Value{}, fmt.Errorf("incremental path field %q does not resolve on a %s", e, v.Kind())
			}
			f := fieldByGraphQLName(v, e)
			if !f.IsValid() {
				return reflect.Value{}, fmt.Errorf("incremental path field %q does not exist", e)
			}
			v = f
		case int:
			if v.Kind() != reflect.Slice && v.Kind() != reflect.Array {
				return reflect.Value{}, fmt.Errorf("incremental path index %d does not resolve on a %s", e, v.Kind())
			}
			if e >= v.Len() {
				return reflect.Value{}, fmt.Errorf("incremental path index %d is beyond list length %d", e, v.Len())
			}
			v = v.Index(e)
		default:
			return reflect.Value{}, fmt.Errorf("unsupported incremental path entry %v", entry)
		}
	}
	return v, nil
}

// ApplyData recursively merges the JSON object data into the object at path.
// Unmentioned fields are kept and an explicit JSON null clears the field.
func (t *Transaction) ApplyData(data json.RawMessage, path []any) error {
	target, err := t.navigate(path)
	if err != nil {
		return err
	}
	doc, err := decodeValue(data)
	if err != nil {
		return err
	}
	m, ok := doc.(map[string]any)
	if !ok {
		return fmt.Errorf("incremental data at %v must be a JSON object", path)
	}
	return t.mergeObject(m, target)
}

// ApplyItems appends the elements of the JSON array items to the list reached
// by path. The final path entry must be an index equal to the current list
// length, so existing elements are never overwritten and no holes are filled.
func (t *Transaction) ApplyItems(items json.RawMessage, path []any) error {
	if len(path) == 0 {
		return fmt.Errorf("incremental items require a path ending in a list index")
	}
	last, ok := path[len(path)-1].(int)
	if !ok {
		return fmt.Errorf("incremental items path must end in an array index, got %v", path[len(path)-1])
	}
	list, err := t.navigate(path[:len(path)-1])
	if err != nil {
		return err
	}
	if list.Kind() == reflect.Ptr {
		if list.IsNil() {
			return fmt.Errorf("incremental items path crosses a null value")
		}
		list = list.Elem()
	}
	if list.Kind() != reflect.Slice {
		return fmt.Errorf("incremental items target is a %s, not a list", list.Kind())
	}
	if last != list.Len() {
		return fmt.Errorf("incremental items index %d does not append at current list length %d", last, list.Len())
	}

	dec := json.NewDecoder(bytes.NewReader(items))
	dec.UseNumber()
	tok, err := dec.Token()
	if err != nil {
		return fmt.Errorf("incremental items must be a JSON array: %w", err)
	}
	if d, ok := tok.(json.Delim); !ok || d != '[' {
		return fmt.Errorf("incremental items must be a JSON array")
	}
	for dec.More() {
		t.set(list, reflect.Append(list, reflect.Zero(list.Type().Elem())))
		elem := list.Index(list.Len() - 1)
		if err := (&decoder{tokenizer: dec}).Decode(elem.Addr().Interface()); err != nil {
			return err
		}
	}
	tok, err = dec.Token()
	if err != nil {
		return err
	}
	if d, ok := tok.(json.Delim); !ok || d != ']' {
		return fmt.Errorf("unexpected token %v in incremental items", tok)
	}
	return nil
}

// mergeObject merges doc into v, which is a struct or a pointer to a struct.
// An existing non-nil pointer is updated in place; only a nil pointer is
// allocated, so pointers already delivered to callers stay valid.
func (t *Transaction) mergeObject(doc map[string]any, v reflect.Value) error {
	if v.Kind() == reflect.Ptr {
		if v.IsNil() {
			t.set(v, reflect.New(v.Type().Elem()))
		}
		v = v.Elem()
	}
	if v.Kind() != reflect.Struct {
		return fmt.Errorf("cannot merge incremental object into %s", v.Type())
	}
	views := t.fragmentViews(v)
	for name, raw := range doc {
		var targets []reflect.Value
		for _, view := range views {
			if f := fieldByGraphQLName(view, name); f.IsValid() {
				targets = append(targets, f)
			}
		}
		if len(targets) == 0 {
			return fmt.Errorf("struct field for %q doesn't exist in any of %v places to unmarshal", name, len(views))
		}
		for _, f := range targets {
			if err := t.mergeValue(raw, f); err != nil {
				return err
			}
		}
	}
	return nil
}

// fragmentViews returns the struct itself plus every struct reachable through
// inline fragments and anonymous embedded fields, matching the unmarshaler's
// fan-out so a response field updates every fragment it maps to.
func (t *Transaction) fragmentViews(v reflect.Value) []reflect.Value {
	var views []reflect.Value
	frontier := []reflect.Value{v}
	for len(frontier) > 0 {
		cur := frontier[0]
		frontier = frontier[1:]
		if cur.Kind() == reflect.Ptr {
			if cur.IsNil() {
				t.set(cur, reflect.New(cur.Type().Elem()))
			}
			cur = cur.Elem()
		}
		if cur.Kind() != reflect.Struct {
			continue
		}
		views = append(views, cur)
		for i := 0; i < cur.NumField(); i++ {
			f := cur.Type().Field(i)
			if f.Anonymous || isGraphQLFragment(f) {
				frontier = append(frontier, cur.Field(i))
			}
		}
	}
	return views
}

func (t *Transaction) mergeValue(raw any, f reflect.Value) error {
	switch val := raw.(type) {
	case nil:
		// An explicit null clears the field, whatever its type.
		t.set(f, reflect.Zero(f.Type()))
		return nil
	case map[string]any:
		return t.mergeObject(val, f)
	default:
		// Scalars and whole-list values are re-encoded and decoded through
		// the usual GraphQL scalar handling.
		b, err := json.Marshal(val)
		if err != nil {
			return err
		}
		tmp := reflect.New(f.Type()).Elem()
		if err := json.Unmarshal(b, tmp.Addr().Interface()); err != nil {
			return err
		}
		t.set(f, tmp)
		return nil
	}
}

func decodeValue(data json.RawMessage) (any, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v any
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	if tok, err := dec.Token(); err != io.EOF {
		if err == nil {
			return nil, fmt.Errorf("invalid token '%v' after incremental value", tok)
		}
		return nil, err
	}
	return v, nil
}
