package jar

import (
	"testing"

	taskassert "github.com/headzoo/surf/task_support/assert"
)

func TestMemoryHistory(t *testing.T) {
	ut := taskassert.New(t)
	stack := NewMemoryHistory()

	page1 := &State{}
	stack.Push(page1)
	ut.AssertEquals(1, stack.Len())
	ut.AssertEquals(page1, stack.Top())

	page2 := &State{}
	stack.Push(page2)
	ut.AssertEquals(2, stack.Len())
	ut.AssertEquals(page2, stack.Top())

	page := stack.Pop()
	ut.AssertEquals(page, page2)
	ut.AssertEquals(1, stack.Len())
	ut.AssertEquals(page1, stack.Top())

	page = stack.Pop()
	ut.AssertEquals(page, page1)
	ut.AssertEquals(0, stack.Len())
}

func TestMemoryHistoryWithMax(t *testing.T) {
	ut := taskassert.New(t)
	stack := NewMemoryHistory()
	stack.SetMax(2)

	ut.AssertEquals(2, stack.maxHist)

	page1 := &State{}
	stack.Push(page1)
	ut.AssertEquals(1, stack.Len())
	ut.AssertEquals(page1, stack.Top())

	page2 := &State{}
	stack.Push(page2)
	ut.AssertEquals(2, stack.Len())
	ut.AssertEquals(page2, stack.Top())

	page3 := &State{}
	stack.Push(page3)
	ut.AssertEquals(2, stack.Len())
	ut.AssertEquals(page3, stack.Top())

	page4 := &State{}
	stack.Push(page4)
	ut.AssertEquals(2, stack.Len())
	ut.AssertEquals(page4, stack.Top())

	page := stack.Pop()
	ut.AssertEquals(page, page4)
	ut.AssertEquals(1, stack.Len())
	ut.AssertEquals(page3, stack.Top())

	page = stack.Pop()
	ut.AssertEquals(page, page3)
	ut.AssertEquals(0, stack.Len())
}

func TestMemoryHistoryClear(t *testing.T) {
	ut := taskassert.New(t)
	stack := NewMemoryHistory()

	stack.Push(&State{})
	stack.Push(&State{})
	ut.AssertEquals(2, stack.Len())
	stack.Clear()
	ut.AssertEquals(0, stack.Len())
}
