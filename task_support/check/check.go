// Package check reports actual assertions and normal completion for a test group.
package check

import (
	"fmt"
	"testing"
)

type C struct{ count int }

func (c *C) Check(t *testing.T, condition bool, message string) {
	t.Helper()
	c.count++
	if !condition {
		t.Error(message)
	}
}
func Run(t *testing.T, body func(*C)) {
	t.Helper()
	c := &C{}
	body(c)
	if c.count == 0 {
		t.Fatal("group executed no content assertions")
	}
	if t.Failed() {
		return
	}
	fmt.Printf("TASK_GROUP %s assertions=%d complete\n", t.Name(), c.count)
}
