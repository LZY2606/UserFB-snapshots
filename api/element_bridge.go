package api

import (
	"errors"
	"fmt"
	"reflect"
	"sort"
	"strconv"
	"strings"
)

// w3cElementKey is the W3C WebDriver element reference key. The legacy JSON
// Wire Protocol uses the "ELEMENT" key instead.
const w3cElementKey = "element-6066-11e4-a52e-4f735466cecf"

// elementReference returns the wire representation of an element reference,
// containing both the legacy and W3C element keys and nothing else.
func elementReference(id string) map[string]string {
	return map[string]string{
		"ELEMENT":     id,
		w3cElementKey: id,
	}
}

// checkElement ensures that the provided element may be used in commands
// executed by this session. Element handles may not be shared between
// sessions, even if the sessions share the same URL.
func (s *Session) checkElement(element *Element) error {
	switch {
	case element.Session == nil:
		return errors.New("element has no session")
	case element.Session != s:
		return errors.New("element belongs to a different session")
	case element.ID == "":
		return errors.New("element ID is empty")
	}
	return nil
}

// encodeScriptArguments validates the provided script arguments and returns an
// encoded copy in which elements are replaced by element references. The
// provided arguments are never modified. All arguments are validated before
// anything is sent, so an invalid argument prevents the script from running.
func (s *Session) encodeScriptArguments(arguments []interface{}) ([]interface{}, error) {
	encoded, err := s.encodeScriptValue(arguments, "", map[container]bool{})
	if err != nil {
		return nil, err
	}
	return encoded.([]interface{}), nil
}

// container identifies a map or slice by identity so that circular references
// along the current recursion path can be detected. The same container
// appearing in multiple positions (aliasing) is not a circular reference.
type container struct {
	kind reflect.Kind
	ptr  uintptr
}

func (s *Session) encodeScriptValue(value interface{}, path string, ancestors map[container]bool) (interface{}, error) {
	switch typed := value.(type) {
	case *Element:
		if typed == nil {
			return nil, nil
		}
		if err := s.checkElement(typed); err != nil {
			return nil, fmt.Errorf("invalid element reference at %s: %s", path, err)
		}
		return elementReference(typed.ID), nil
	case []interface{}:
		if typed == nil {
			return nil, nil
		}
		leave, err := enter(typed, path, ancestors)
		if err != nil {
			return nil, err
		}
		defer leave()
		encoded := make([]interface{}, len(typed))
		for i, item := range typed {
			encodedItem, err := s.encodeScriptValue(item, path+"/"+strconv.Itoa(i), ancestors)
			if err != nil {
				return nil, err
			}
			encoded[i] = encodedItem
		}
		return encoded, nil
	case map[string]interface{}:
		if typed == nil {
			return nil, nil
		}
		leave, err := enter(typed, path, ancestors)
		if err != nil {
			return nil, err
		}
		defer leave()
		keys := make([]string, 0, len(typed))
		for key := range typed {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		encoded := make(map[string]interface{}, len(typed))
		for _, key := range keys {
			encodedValue, err := s.encodeScriptValue(typed[key], path+"/"+escapePointerToken(key), ancestors)
			if err != nil {
				return nil, err
			}
			encoded[key] = encodedValue
		}
		return encoded, nil
	default:
		return value, nil
	}
}

// enter marks the provided map or slice as present on the current recursion
// path and returns a function that removes the mark. Empty containers cannot
// participate in a cycle and are never marked.
func enter(value interface{}, path string, ancestors map[container]bool) (func(), error) {
	mirror := reflect.ValueOf(value)
	if mirror.Len() == 0 {
		return func() {}, nil
	}
	id := container{mirror.Kind(), mirror.Pointer()}
	if ancestors[id] {
		return nil, fmt.Errorf("circular reference at %s", path)
	}
	ancestors[id] = true
	return func() { delete(ancestors, id) }, nil
}

// escapePointerToken escapes a map key for use as a JSON Pointer (RFC 6901)
// token in error paths.
func escapePointerToken(token string) string {
	token = strings.Replace(token, "~", "~0", -1)
	return strings.Replace(token, "/", "~1", -1)
}
