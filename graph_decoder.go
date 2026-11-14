package jsonapi

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"reflect"
	"strings"
)

// resourceIdentity is the ordered (type, id) pair that uniquely identifies a
// resource inside a JSON:API document. Both members are non-empty strings.
type resourceIdentity struct {
	typ string
	id  string
}

func (r resourceIdentity) String() string {
	return fmt.Sprintf("{type: %q, id: %q}", r.typ, r.id)
}

// modelSpec describes one registered model type. A *modelSpec is immutable once
// NewGraphDecoder has returned and may be read concurrently by many Decode
// calls.
type modelSpec struct {
	typ          reflect.Type // the struct type T
	typeName     string
	primaryIndex int
	attrFields   []graphAttrSpec
	relationMap  map[string]graphRelationSpec
	relationList []graphRelationSpec
}

type graphAttrSpec struct {
	index int
	name  string
	args  []string
	field reflect.StructField
}

type graphRelationSpec struct {
	index  int
	name   string
	field  reflect.StructField
	toMany bool
	iface  bool
	// target is the concrete registered model for typed pointer/slice fields
	// and nil for interface{} / []interface{} fields.
	target *modelSpec
}

// GraphDecoder decodes JSON:API documents into shared Go object graphs.
//
// Unlike UnmarshalPayload, which allocates a fresh struct every time a
// relationship is walked, a GraphDecoder keeps one Go object per resource
// identity (the ordered type/id pair). Relationships therefore become real
// shared pointers: cycles, self references and resources referenced from many
// places all resolve to the same addressable value.
//
// The registry captured by a GraphDecoder is built once and read-only
// afterwards; the same decoder can serve concurrent Decode calls, and no
// resource object is ever shared between calls.
type GraphDecoder struct {
	models     map[reflect.Type]*modelSpec
	byTypeName map[string]*modelSpec
}

// NewGraphDecoder registers the model types that Decode may materialize. Each
// argument must be a typed nil pointer to a struct, e.g. (*Article)(nil).
//
// Every model must have exactly one exported string field carrying a
// `jsonapi:"primary,<type>"` tag, the type names must be non-empty and unique,
// every tagged field must be settable, and every relation field must be a
// registered model pointer, a slice of such pointers, interface{} or
// []interface{}.
func NewGraphDecoder(models ...interface{}) (*GraphDecoder, error) {
	d := &GraphDecoder{
		models:     map[reflect.Type]*modelSpec{},
		byTypeName: map[string]*modelSpec{},
	}

	// First pass: collect struct shapes, primary keys and attributes so that a
	// relation can target a model registered later in the argument list.
	for _, model := range models {
		if model == nil {
			return nil, fmt.Errorf("jsonapi: NewGraphDecoder received a nil model; register typed nil pointers such as (*Article)(nil)")
		}
		ptrType := reflect.TypeOf(model)
		if ptrType.Kind() != reflect.Ptr || ptrType.Elem().Kind() != reflect.Struct {
			return nil, fmt.Errorf("jsonapi: model %s must be a typed nil pointer to a struct", ptrType)
		}
		structType := ptrType.Elem()
		if _, exists := d.models[structType]; exists {
			return nil, fmt.Errorf("jsonapi: model %s is registered more than once", structType.Name())
		}

		spec := &modelSpec{
			typ:          structType,
			primaryIndex: -1,
			relationMap:  map[string]graphRelationSpec{},
		}

		primaryCount := 0
		attrNames := map[string]bool{}
		for i := 0; i < structType.NumField(); i++ {
			structField := structType.Field(i)
			tag := structField.Tag.Get(annotationJSONAPI)
			if tag == "" {
				continue
			}
			if !structField.IsExported() {
				return nil, fmt.Errorf("jsonapi: field %s.%s carries a jsonapi tag but is not exported or settable",
					structType.Name(), structField.Name)
			}

			args := strings.Split(tag, annotationSeperator)
			if len(args) < 1 {
				return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid jsonapi tag %q",
					structType.Name(), structField.Name, tag)
			}
			annotation := args[0]

			// client-id is a single-argument tag for compatibility with the
			// legacy models; graph documents never carry it so the field is
			// simply left untouched.
			if annotation == annotationClientID {
				if len(args) != 1 {
					return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid client-id tag %q",
						structType.Name(), structField.Name, tag)
				}
				continue
			}
			if len(args) < 2 || args[1] == "" {
				return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid jsonapi tag %q",
					structType.Name(), structField.Name, tag)
			}

			switch annotation {
			case annotationPrimary:
				if len(args) != 2 || args[1] == "" {
					return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid primary tag %q",
						structType.Name(), structField.Name, tag)
				}
				if structField.Type.Kind() != reflect.String {
					return nil, fmt.Errorf("jsonapi: primary field %s.%s must be a string",
						structType.Name(), structField.Name)
				}
				primaryCount++
				spec.primaryIndex = i
				spec.typeName = args[1]

			case annotationAttribute:
				if args[1] == "" {
					return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid attr tag %q",
						structType.Name(), structField.Name, tag)
				}
				if attrNames[args[1]] {
					return nil, fmt.Errorf("jsonapi: model %s declares attribute %q more than once",
						structType.Name(), args[1])
				}
				attrNames[args[1]] = true
				spec.attrFields = append(spec.attrFields, graphAttrSpec{
					index: i,
					name:  args[1],
					args:  args,
					field: structField,
				})

			case annotationRelation:
				if args[1] == "" {
					return nil, fmt.Errorf("jsonapi: field %s.%s has an invalid relation tag %q",
						structType.Name(), structField.Name, tag)
				}
				if _, exists := spec.relationMap[args[1]]; exists {
					return nil, fmt.Errorf("jsonapi: model %s declares relation %q more than once",
						structType.Name(), args[1])
				}
				spec.relationMap[args[1]] = graphRelationSpec{
					index: i,
					name:  args[1],
					field: structField,
				}
				spec.relationList = append(spec.relationList, spec.relationMap[args[1]])

			default:
				return nil, fmt.Errorf("jsonapi: field %s.%s uses unsupported jsonapi annotation %q",
					structType.Name(), structField.Name, annotation)
			}
		}

		if primaryCount != 1 {
			return nil, fmt.Errorf("jsonapi: model %s must have exactly one exported string primary field, found %d",
				structType.Name(), primaryCount)
		}
		if spec.typeName == "" {
			return nil, fmt.Errorf("jsonapi: model %s has an empty resource type name", structType.Name())
		}
		if other, exists := d.byTypeName[spec.typeName]; exists {
			return nil, fmt.Errorf("jsonapi: resource type %q is registered by both %s and %s",
				spec.typeName, other.typ.Name(), structType.Name())
		}

		d.models[structType] = spec
		d.byTypeName[spec.typeName] = spec
	}

	// Second pass: validate every relation shape and target type now that all
	// models are known.
	for _, spec := range d.models {
		for i := range spec.relationList {
			rel := spec.relationList[i]
			fieldType := rel.field.Type

			switch {
			case fieldType.Kind() == reflect.Interface:
				if fieldType.NumMethod() != 0 {
					return nil, fmt.Errorf("jsonapi: relation %s.%s must be the empty interface{}, not %s",
						spec.typ.Name(), rel.field.Name, fieldType)
				}
				rel.iface = true

			case fieldType.Kind() == reflect.Ptr && fieldType.Elem().Kind() == reflect.Struct:
				target, ok := d.models[fieldType.Elem()]
				if !ok {
					return nil, fmt.Errorf("jsonapi: relation %s.%s targets unregistered model %s",
						spec.typ.Name(), rel.field.Name, fieldType.Elem().Name())
				}
				rel.target = target

			case fieldType.Kind() == reflect.Slice:
				elemType := fieldType.Elem()
				rel.toMany = true
				switch {
				case elemType.Kind() == reflect.Interface:
					if elemType.NumMethod() != 0 {
						return nil, fmt.Errorf("jsonapi: relation %s.%s must be []interface{}, not %s",
							spec.typ.Name(), rel.field.Name, fieldType)
					}
					rel.iface = true
				case elemType.Kind() == reflect.Ptr && elemType.Elem().Kind() == reflect.Struct:
					target, ok := d.models[elemType.Elem()]
					if !ok {
						return nil, fmt.Errorf("jsonapi: relation %s.%s targets unregistered model %s",
							spec.typ.Name(), rel.field.Name, elemType.Elem().Name())
					}
					rel.target = target
				default:
					return nil, fmt.Errorf("jsonapi: relation %s.%s has unsupported shape %s; want []*Struct or []interface{}",
						spec.typ.Name(), rel.field.Name, fieldType)
				}

			default:
				return nil, fmt.Errorf("jsonapi: relation %s.%s has unsupported shape %s; want *Struct, []*Struct, interface{} or []interface{}",
					spec.typ.Name(), rel.field.Name, fieldType)
			}

			spec.relationMap[rel.name] = rel
			spec.relationList[i] = rel
		}
	}

	return d, nil
}

// Decode reads one JSON:API document from in and publishes the decoded graph
// into dst. dst must be a non-nil *T or *[]*T for a registered T; the shape is
// validated before in is read. Nothing is written to dst unless the whole
// document was decoded, every relationship connected and the single JSON value
// reached EOF without error. The reader is never closed.
func (d *GraphDecoder) Decode(in io.Reader, dst interface{}) error {
	rootSpec, many, err := d.validateDestination(dst)
	if err != nil {
		return err
	}

	session, err := d.parse(in, rootSpec, many)
	if err != nil {
		return err
	}
	if err := session.materialize(); err != nil {
		return err
	}

	// Publication is the only step that touches caller-owned memory.
	session.publish(reflect.ValueOf(dst))
	return nil
}

func (d *GraphDecoder) validateDestination(dst interface{}) (*modelSpec, bool, error) {
	dstValue := reflect.ValueOf(dst)
	if !dstValue.IsValid() || dstValue.Kind() != reflect.Ptr || dstValue.IsNil() {
		return nil, false, fmt.Errorf("jsonapi: Decode destination must be a non-nil pointer to a registered struct or slice of struct pointers")
	}
	elem := dstValue.Elem()
	switch elem.Kind() {
	case reflect.Struct:
		spec, ok := d.models[elem.Type()]
		if !ok {
			return nil, false, fmt.Errorf("jsonapi: destination type %s is not registered with this GraphDecoder", elem.Type().Name())
		}
		return spec, false, nil
	case reflect.Slice:
		elemType := elem.Type().Elem()
		if elemType.Kind() != reflect.Ptr || elemType.Elem().Kind() != reflect.Struct {
			return nil, false, fmt.Errorf("jsonapi: Decode destination must be *[]*T for a registered struct T, got %s", elem.Type())
		}
		spec, ok := d.models[elemType.Elem()]
		if !ok {
			return nil, false, fmt.Errorf("jsonapi: destination element type %s is not registered with this GraphDecoder", elemType.Elem().Name())
		}
		return spec, true, nil
	default:
		return nil, false, fmt.Errorf("jsonapi: Decode destination must be *T or *[]*T for a registered struct T, got %s", dstValue.Type())
	}
}

// rawResource is the validated shape of a JSON:API resource object.
type rawResource struct {
	typ           string
	id            string
	attributes    map[string]json.RawMessage
	relationships map[string]json.RawMessage
}

// rawLinkage is the parsed relationship "data" member.
type rawLinkage struct {
	hasData bool
	null    bool
	toMany  bool
	single  resourceIdentity
	many    []resourceIdentity
}

type rawDefinition struct {
	resource  *rawResource
	identity  resourceIdentity
	relations map[string]*rawLinkage
}

// graphSession holds all per-call state. Nothing in a session is reachable from
// the immutable GraphDecoder, so concurrent Decode calls never share objects.
type graphSession struct {
	decoder *GraphDecoder
	many    bool

	definitions map[resourceIdentity]*rawDefinition
	// order keeps the primary "data" resources in document order.
	order []resourceIdentity

	// objects maps every materialized identity to its concrete *T value.
	objects map[resourceIdentity]reflect.Value
}

func (d *GraphDecoder) parse(in io.Reader, rootSpec *modelSpec, many bool) (*graphSession, error) {
	dec := json.NewDecoder(in)

	var top map[string]json.RawMessage
	if err := dec.Decode(&top); err != nil {
		// Reader errors are returned unwrapped by encoding/json, so errors.Is
		// keeps recognizing the underlying cause.
		return nil, err
	}
	if _, hasErrors := top["errors"]; hasErrors {
		return nil, fmt.Errorf("jsonapi: errors documents are not supported by GraphDecoder")
	}
	dataRaw, ok := top["data"]
	if !ok {
		return nil, fmt.Errorf("jsonapi: document is missing the required top-level \"data\" member")
	}

	session := &graphSession{
		decoder:     d,
		many:        many,
		definitions: map[resourceIdentity]*rawDefinition{},
		objects:     map[resourceIdentity]reflect.Value{},
	}

	dataTrimmed := bytes.TrimSpace(dataRaw)
	if len(dataTrimmed) == 0 {
		return nil, fmt.Errorf("jsonapi: top-level \"data\" must be an object or array")
	}
	switch dataTrimmed[0] {
	case 'n':
		return nil, fmt.Errorf("jsonapi: top-level \"data\" is null; null primary data is not supported")
	case '{':
		if many {
			return nil, fmt.Errorf("jsonapi: destination expects a data array but \"data\" is an object")
		}
		def_, err := parseResourceDefinition(dataRaw, "data")
		if err != nil {
			return nil, err
		}
		if err := session.indexDefinition(def_, rootSpec); err != nil {
			return nil, err
		}
		session.order = []resourceIdentity{def_.identity}
	case '[':
		if !many {
			return nil, fmt.Errorf("jsonapi: destination expects a data object but \"data\" is an array")
		}
		var elements []json.RawMessage
		if err := json.Unmarshal(dataTrimmed, &elements); err != nil {
			return nil, fmt.Errorf("jsonapi: top-level \"data\" array: %w", err)
		}
		for i, element := range elements {
			def_, err := parseResourceDefinition(element, fmt.Sprintf("data[%d]", i))
			if err != nil {
				return nil, err
			}
			if err := session.indexDefinition(def_, rootSpec); err != nil {
				return nil, err
			}
			session.order = append(session.order, def_.identity)
		}
	default:
		return nil, fmt.Errorf("jsonapi: top-level \"data\" must be an object or array")
	}

	if includedRaw, present := top["included"]; present {
		includedTrimmed := bytes.TrimSpace(includedRaw)
		if len(includedTrimmed) > 0 && includedTrimmed[0] == 'n' {
			return nil, fmt.Errorf("jsonapi: top-level \"included\" must be an array")
		}
		var included []json.RawMessage
		if err := json.Unmarshal(includedTrimmed, &included); err != nil {
			return nil, fmt.Errorf("jsonapi: top-level \"included\" must be an array: %w", err)
		}
		for i, element := range included {
			def_, err := parseResourceDefinition(element, fmt.Sprintf("included[%d]", i))
			if err != nil {
				return nil, err
			}
			if err := session.indexDefinition(def_, nil); err != nil {
				return nil, err
			}
		}
	}

	// Reject a second JSON value before any object is built or published.
	if tok, err := dec.Token(); err != io.EOF {
		if err != nil {
			return nil, err
		}
		return nil, fmt.Errorf("jsonapi: trailing data after the JSON:API document near %v", tok)
	}

	return session, nil
}

func (s *graphSession) indexDefinition(def *rawDefinition, rootSpec *modelSpec) error {
	if rootSpec != nil && def.identity.typ != rootSpec.typeName {
		return fmt.Errorf("jsonapi: primary resource %s has type %q but destination expects %q",
			def.identity, def.identity.typ, rootSpec.typeName)
	}
	if _, exists := s.definitions[def.identity]; exists {
		return fmt.Errorf("jsonapi: resource %s is defined more than once in data/included", def.identity)
	}
	s.definitions[def.identity] = def
	return nil
}

func parseResourceDefinition(raw json.RawMessage, location string) (*rawDefinition, error) {
	obj, err := decodeJSONObject(raw, location)
	if err != nil {
		return nil, err
	}

	res := &rawResource{}
	if _, ok := obj["lid"]; ok {
		return nil, fmt.Errorf("jsonapi: %s uses \"lid\", which GraphDecoder does not accept", location)
	}

	typeRaw, ok := obj["type"]
	if !ok {
		return nil, fmt.Errorf("jsonapi: %s is missing its non-empty \"type\" member", location)
	}
	if err := json.Unmarshal(bytes.TrimSpace(typeRaw), &res.typ); err != nil {
		return nil, fmt.Errorf("jsonapi: %s has an invalid \"type\" member: %w", location, err)
	}
	if res.typ == "" {
		return nil, fmt.Errorf("jsonapi: %s has an empty resource type", location)
	}

	idRaw, ok := obj["id"]
	if !ok {
		return nil, fmt.Errorf("jsonapi: %s is missing its non-empty \"id\" member", location)
	}
	if err := json.Unmarshal(bytes.TrimSpace(idRaw), &res.id); err != nil {
		return nil, fmt.Errorf("jsonapi: %s has an invalid \"id\" member: %w", location, err)
	}
	if res.id == "" {
		return nil, fmt.Errorf("jsonapi: %s has an empty resource id", location)
	}

	if attrRaw, present := obj["attributes"]; present {
		attrs, err := decodeMemberObject(attrRaw, location+".attributes")
		if err != nil {
			return nil, err
		}
		res.attributes = attrs
	}

	if relRaw, present := obj["relationships"]; present {
		rels, err := decodeMemberObject(relRaw, location+".relationships")
		if err != nil {
			return nil, err
		}
		res.relationships = rels
	}

	def := &rawDefinition{
		resource:  res,
		identity:  resourceIdentity{typ: res.typ, id: res.id},
		relations: map[string]*rawLinkage{},
	}

	for name, relationshipRaw := range res.relationships {
		linkage, err := parseLinkage(relationshipRaw, fmt.Sprintf("%s.relationships.%s", location, name))
		if err != nil {
			return nil, err
		}
		def.relations[name] = linkage
	}

	return def, nil
}

func parseLinkage(raw json.RawMessage, location string) (*rawLinkage, error) {
	obj, err := decodeJSONObject(raw, location)
	if err != nil {
		return nil, err
	}
	dataRaw, hasData := obj["data"]
	if !hasData {
		// A links-only relationship carries no linkage and is treated as absent.
		return &rawLinkage{hasData: false}, nil
	}

	linkage := &rawLinkage{hasData: true}
	trimmed := bytes.TrimSpace(dataRaw)
	if len(trimmed) == 0 {
		return nil, fmt.Errorf("jsonapi: %s.data must be null, an object or an array", location)
	}
	switch trimmed[0] {
	case 'n':
		linkage.null = true
	case '{':
		id, err := parseResourceIdentifier(trimmed, location+".data")
		if err != nil {
			return nil, err
		}
		linkage.single = id
	case '[':
		linkage.toMany = true
		var elements []json.RawMessage
		if err := json.Unmarshal(trimmed, &elements); err != nil {
			return nil, fmt.Errorf("jsonapi: %s.data must be an array of resource identifiers: %w", location, err)
		}
		for i, element := range elements {
			id, err := parseResourceIdentifier(element, fmt.Sprintf("%s.data[%d]", location, i))
			if err != nil {
				return nil, err
			}
			linkage.many = append(linkage.many, id)
		}
	default:
		return nil, fmt.Errorf("jsonapi: %s.data must be null, an object or an array", location)
	}
	return linkage, nil
}

func parseResourceIdentifier(raw json.RawMessage, location string) (resourceIdentity, error) {
	obj, err := decodeJSONObject(raw, location)
	if err != nil {
		return resourceIdentity{}, err
	}
	var id resourceIdentity
	for key := range obj {
		if key != "type" && key != "id" {
			return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s may only carry type and id, found %q", location, key)
		}
	}
	typeRaw, ok := obj["type"]
	if !ok {
		return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s is missing \"type\"", location)
	}
	if err := json.Unmarshal(bytes.TrimSpace(typeRaw), &id.typ); err != nil {
		return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s has invalid \"type\": %w", location, err)
	}
	idRaw, ok := obj["id"]
	if !ok {
		return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s is missing \"id\"", location)
	}
	if err := json.Unmarshal(bytes.TrimSpace(idRaw), &id.id); err != nil {
		return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s has invalid \"id\": %w", location, err)
	}
	if id.typ == "" || id.id == "" {
		return resourceIdentity{}, fmt.Errorf("jsonapi: resource identifier at %s has an empty type or id", location)
	}
	return id, nil
}

func decodeJSONObject(raw json.RawMessage, location string) (map[string]json.RawMessage, error) {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || trimmed[0] != '{' {
		return nil, fmt.Errorf("jsonapi: %s must be a JSON object", location)
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(trimmed, &obj); err != nil {
		return nil, fmt.Errorf("jsonapi: %s: %w", location, err)
	}
	return obj, nil
}

func decodeMemberObject(raw json.RawMessage, location string) (map[string]json.RawMessage, error) {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) > 0 && trimmed[0] == 'n' {
		return nil, fmt.Errorf("jsonapi: %s must be a JSON object", location)
	}
	if len(trimmed) == 0 || trimmed[0] != '{' {
		return nil, fmt.Errorf("jsonapi: %s must be a JSON object", location)
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(trimmed, &obj); err != nil {
		return nil, fmt.Errorf("jsonapi: %s: %w", location, err)
	}
	return obj, nil
}

// materialize allocates one object per reachable identity (reusing identities
// across every relationship), fills defined resources, creates placeholders for
// missing definitions and then connects every relationship.
func (s *graphSession) materialize() error {
	queue := make([]resourceIdentity, 0, len(s.order))
	for _, id := range s.order {
		if _, err := s.objectFor(id, &queue); err != nil {
			return err
		}
	}

	for len(queue) > 0 {
		id := queue[0]
		queue = queue[1:]
		if err := s.connectOne(id, &queue); err != nil {
			return err
		}
	}
	return nil
}

// objectFor returns the single Go object for an identity, allocating it on the
// first encounter. Definitions are filled completely; missing definitions
// become shared placeholders carrying only the primary key.
func (s *graphSession) objectFor(id resourceIdentity, queue *[]resourceIdentity) (reflect.Value, error) {
	if existing, ok := s.objects[id]; ok {
		return existing, nil
	}

	spec := s.decoder.byTypeName[id.typ]
	if spec == nil {
		return reflect.Value{}, fmt.Errorf("jsonapi: resource %s uses unregistered type %q", id, id.typ)
	}

	object := reflect.New(spec.typ)
	s.objects[id] = object
	*queue = append(*queue, id)

	object.Elem().Field(spec.primaryIndex).SetString(id.id)

	def := s.definitions[id]
	if def == nil {
		// Placeholder: only the primary key is known.
		return object, nil
	}
	if err := s.fillAttributes(object, spec, def); err != nil {
		return reflect.Value{}, err
	}
	return object, nil
}

func (s *graphSession) fillAttributes(object reflect.Value, spec *modelSpec, def *rawDefinition) error {
	elem := object.Elem()
	for _, attr := range spec.attrFields {
		raw, present := def.resource.attributes[attr.name]
		if !present {
			continue
		}
		var value interface{}
		if err := json.Unmarshal(raw, &value); err != nil {
			return fmt.Errorf("jsonapi: attribute %q of resource %s: %w", attr.name, def.identity, err)
		}
		if value == nil {
			continue
		}
		// The legacy []string helper type-asserts its inputs and would panic on
		// a non-array value or non-string elements. Validate the shape up front
		// so conversion failures stay returned errors instead of panics; the
		// conversion itself still runs through the shared legacy helper below.
		if elem.Field(attr.index).Type() == reflect.TypeOf([]string{}) {
			arr, ok := value.([]interface{})
			if !ok {
				return fmt.Errorf("jsonapi: attribute %q of resource %s: %w", attr.name, def.identity, ErrInvalidType)
			}
			for _, item := range arr {
				if _, ok := item.(string); !ok {
					return fmt.Errorf("jsonapi: attribute %q of resource %s: %w", attr.name, def.identity, ErrInvalidType)
				}
			}
		}
		// Reuse the legacy attribute conversions verbatim: custom scalars,
		// pointers, times, nested structs and slices all behave as before.
		converted, err := unmarshalAttribute(value, attr.args, attr.field, elem.Field(attr.index))
		if err != nil {
			return fmt.Errorf("jsonapi: attribute %q of resource %s: %w", attr.name, def.identity, err)
		}
		assign(elem.Field(attr.index), converted)
	}
	return nil
}

// connectOne wires every relation field declared on the model for one object.
// It is driven by the model fields, so unknown document members are ignored,
// and it never uses a visited cut-off: every edge is connected, including
// self loops and cycles.
func (s *graphSession) connectOne(id resourceIdentity, queue *[]resourceIdentity) error {
	def := s.definitions[id]
	if def == nil {
		// Placeholder has no definition and therefore no linkage to connect.
		return nil
	}
	spec := s.decoder.byTypeName[id.typ]
	object := s.objects[id].Elem()

	for _, rel := range spec.relationList {
		linkage := def.relations[rel.name]
		if linkage == nil || !linkage.hasData {
			// Not provided (or links-only): leave the field at its zero value.
			continue
		}
		field := object.Field(rel.index)

		if rel.toMany {
			if linkage.null {
				return fmt.Errorf("jsonapi: relation %q of resource %s is to-many but received null linkage",
					rel.name, id)
			}
			if !linkage.toMany {
				return fmt.Errorf("jsonapi: relation %q of resource %s is to-many but received a single resource identifier",
					rel.name, id)
			}
			slice := reflect.MakeSlice(rel.field.Type, 0, len(linkage.many))
			for _, ref := range linkage.many {
				target, err := s.relationTarget(ref, rel, id, queue)
				if err != nil {
					return err
				}
				slice = reflect.Append(slice, target)
			}
			field.Set(slice)
			continue
		}

		if linkage.toMany {
			return fmt.Errorf("jsonapi: relation %q of resource %s is to-one but received an array of resource identifiers",
				rel.name, id)
		}
		if linkage.null {
			field.Set(reflect.Zero(rel.field.Type))
			continue
		}
		target, err := s.relationTarget(linkage.single, rel, id, queue)
		if err != nil {
			return err
		}
		field.Set(target)
	}
	return nil
}

// relationTarget resolves one identifier to the concrete pointer that belongs
// in the relation field, enforcing registered types and cardinality. Newly
// reached identities are scheduled for connection via queue.
func (s *graphSession) relationTarget(ref resourceIdentity, rel graphRelationSpec, owner resourceIdentity, queue *[]resourceIdentity) (reflect.Value, error) {
	targetSpec := s.decoder.byTypeName[ref.typ]
	if targetSpec == nil {
		return reflect.Value{}, fmt.Errorf("jsonapi: relation %q of resource %s references unregistered type %q",
			rel.name, owner, ref.typ)
	}
	if !rel.iface && targetSpec != rel.target {
		return reflect.Value{}, fmt.Errorf("jsonapi: relation %q of resource %s expects type %q but identifier %s uses %q",
			rel.name, owner, rel.target.typeName, ref, ref.typ)
	}

	target, err := s.objectFor(ref, queue)
	if err != nil {
		return reflect.Value{}, err
	}
	assignableType := rel.field.Type
	if rel.toMany {
		assignableType = assignableType.Elem()
	}
	if !target.Type().AssignableTo(assignableType) {
		return reflect.Value{}, fmt.Errorf("jsonapi: relation %q of resource %s cannot hold %s",
			rel.name, owner, target.Type())
	}
	return target, nil
}

// publish copies the fully built scratch graph into the caller's destination.
// For single objects the caller's root pointer is preserved: stale fields are
// cleared first and every edge that points back at the primary resource is
// rewritten onto that caller-owned pointer, including interface and slice
// edges.
func (s *graphSession) publish(dst reflect.Value) {
	if !s.many {
		rootIdentity := s.order[0]
		rootSpec := s.decoder.byTypeName[rootIdentity.typ]
		scratchRoot := s.objects[rootIdentity]
		callerStruct := dst.Elem()
		scratchStruct := scratchRoot.Elem()

		// The caller's struct may carry old values. Every field is zeroed so
		// absent attributes and relationships stay at their zero value rather
		// than inheriting stale state.
		callerStruct.Set(reflect.Zero(rootSpec.typ))
		for _, attr := range rootSpec.attrFields {
			callerStruct.Field(attr.index).Set(scratchStruct.Field(attr.index))
		}
		callerStruct.Field(rootSpec.primaryIndex).Set(scratchStruct.Field(rootSpec.primaryIndex))

		// Swap the scratch root for the caller's root in the identity map and
		// reconnect, so every back-reference lands on the caller-owned pointer.
		s.objects[rootIdentity] = dst
		queue := make([]resourceIdentity, 0, len(s.objects))
		for id := range s.objects {
			queue = append(queue, id)
		}
		for len(queue) > 0 {
			id := queue[0]
			queue = queue[1:]
			// Reconnection cannot fail: types and cardinality were validated
			// while building the scratch graph.
			_ = s.connectOne(id, new([]resourceIdentity))
		}
		return
	}

	if len(s.order) == 0 {
		// Even an empty data array publishes a non-nil empty slice.
		dst.Elem().Set(reflect.MakeSlice(dst.Elem().Type(), 0, 0))
		return
	}
	slice := reflect.MakeSlice(dst.Elem().Type(), 0, len(s.order))
	for _, id := range s.order {
		slice = reflect.Append(slice, s.objects[id])
	}
	dst.Elem().Set(slice)
}
