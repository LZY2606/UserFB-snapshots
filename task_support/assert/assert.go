// Package taskassert adapts the upstream assertion call sites to testing.TB.
// Original implementation for this offline task support; licensed MIT.
package taskassert

import (
	"reflect"
	"strings"
	"testing"
)

type T struct{ t testing.TB }

func New(t testing.TB) *T { return &T{t: t} }
func (c *T) AssertEquals(want, got interface{}) {
	c.t.Helper()
	if !reflect.DeepEqual(want, got) {
		c.t.Fatalf("want %#v (%T), got %#v (%T)", want, want, got, got)
	}
}
func (c *T) AssertTrue(got bool) {
	c.t.Helper()
	if !got {
		c.t.Fatal("want true")
	}
}
func (c *T) AssertFalse(got bool) {
	c.t.Helper()
	if got {
		c.t.Fatal("want false")
	}
}
func nilish(v interface{}) bool {
	if v == nil {
		return true
	}
	r := reflect.ValueOf(v)
	switch r.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Ptr, reflect.Slice:
		return r.IsNil()
	}
	return false
}
func (c *T) AssertNil(got interface{}) {
	c.t.Helper()
	if !nilish(got) {
		c.t.Fatalf("want nil, got %#v", got)
	}
}
func (c *T) AssertNotNil(got interface{}) {
	c.t.Helper()
	if nilish(got) {
		c.t.Fatal("want non-nil")
	}
}
func (c *T) AssertContains(needle, haystack string) {
	c.t.Helper()
	if !strings.Contains(haystack, needle) {
		c.t.Fatalf("%q does not contain %q", haystack, needle)
	}
}
func (c *T) AssertGreaterThan(lower, got int) {
	c.t.Helper()
	if got <= lower {
		c.t.Fatalf("want value > %d, got %d", lower, got)
	}
}
