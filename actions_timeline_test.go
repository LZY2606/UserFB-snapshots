package selenium_test

import (
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/tebeka/selenium"
)

// timelineRequest is the recorded body of a POST /session/{id}/actions
// request.
type timelineRequest struct {
	Actions []struct {
		Type       string                   `json:"type"`
		ID         string                   `json:"id"`
		Parameters map[string]string        `json:"parameters"`
		Actions    []map[string]interface{} `json:"actions"`
	} `json:"actions"`
}

// timelineServer is a fake W3C WebDriver server recording actions requests
// and serving canned element rectangles.
type timelineServer struct {
	*httptest.Server
	t         *testing.T
	requests  []timelineRequest
	rectCalls int
	// rectValues are returned by successive Get Element Rect calls; the
	// last value is repeated when the list is exhausted.
	rectValues []string
	// rectStatus/rectBody, when rectStatus is non-zero, are used to answer
	// every Get Element Rect call.
	rectStatus int
	rectBody   string
	// legacy makes the session endpoint answer like a pre-W3C server.
	legacy bool
}

func newTimelineServer(t *testing.T) *timelineServer {
	t.Helper()
	ts := &timelineServer{t: t}
	ts.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch {
		case r.URL.Path == "/session":
			if ts.legacy {
				w.Write([]byte(`{"status":0,"sessionId":"rpa","value":{"browserName":"chrome"}}`))
			} else {
				w.Write([]byte(`{"value":{"sessionId":"rpa","capabilities":{"browserName":"chrome"}}}`))
			}
		case r.Method == "GET" && strings.HasPrefix(r.URL.Path, "/session/rpa/element/") && strings.HasSuffix(r.URL.Path, "/rect"):
			ts.rectCalls++
			if ts.rectStatus != 0 {
				w.WriteHeader(ts.rectStatus)
				w.Write([]byte(ts.rectBody))
				return
			}
			i := ts.rectCalls - 1
			if i >= len(ts.rectValues) {
				i = len(ts.rectValues) - 1
			}
			w.Write([]byte(ts.rectValues[i]))
		case r.Method == "POST" && r.URL.Path == "/session/rpa/actions":
			var req timelineRequest
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				t.Errorf("decoding actions request: %v", err)
			}
			ts.requests = append(ts.requests, req)
			w.Write([]byte(`{"value":null}`))
		default:
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(ts.Server.Close)
	return ts
}

func (ts *timelineServer) driver(t *testing.T) selenium.WebDriver {
	t.Helper()
	wd, err := selenium.NewRemote(selenium.Capabilities{"browserName": "chrome"}, ts.URL)
	if err != nil {
		t.Fatalf("NewRemote: %v", err)
	}
	return wd
}

// decodeElement returns a WebElement handle for the given id without any
// server round-trip.
func decodeElement(t *testing.T, wd selenium.WebDriver, id string) selenium.WebElement {
	t.Helper()
	elem, err := wd.DecodeElement([]byte(fmt.Sprintf(
		`{"value":{"element-6066-11e4-a52e-4f735466cecf":%q}}`, id)))
	if err != nil {
		t.Fatalf("DecodeElement: %v", err)
	}
	return elem
}

func actType(a map[string]interface{}) string {
	s, _ := a["type"].(string)
	return s
}

func actNumber(a map[string]interface{}, key string) float64 {
	f, _ := a[key].(float64)
	return f
}

func wantPause(t *testing.T, a map[string]interface{}, duration float64) {
	t.Helper()
	if actType(a) != "pause" || actNumber(a, "duration") != duration {
		t.Errorf("want pause(%v), got %v", duration, a)
	}
}

func wantMove(t *testing.T, a map[string]interface{}, duration, x, y float64) {
	t.Helper()
	if actType(a) != "pointerMove" || actNumber(a, "duration") != duration ||
		actNumber(a, "x") != x || actNumber(a, "y") != y || a["origin"] != "viewport" {
		t.Errorf("want pointerMove(%v, %v, %v), got %v", duration, x, y, a)
	}
}

func wantButton(t *testing.T, a map[string]interface{}, typ string, button float64) {
	t.Helper()
	if actType(a) != typ || actNumber(a, "button") != button {
		t.Errorf("want %s(%v), got %v", typ, button, a)
	}
}

func wantKey(t *testing.T, a map[string]interface{}, typ, value string) {
	t.Helper()
	if actType(a) != typ || a["value"] != value {
		t.Errorf("want %s(%q), got %v", typ, value, a)
	}
}

// segmentPosition computes a position by constant per-segment velocity, the
// independent reference for the interpolation the library performs: on the
// segment from (x0, y0) at t0 to (x1, y1) at t1, the velocity is
// (x1-x0)/(t1-t0) and the position at is x0 + v*(at-t0).
func segmentPosition(t0, x0, t1, x1, at float64) float64 {
	v := (x1 - x0) / (t1 - t0)
	return x0 + v*(at-t0)
}

func TestActionTimeline(t *testing.T) {
	t.Run("pinch_with_shift_key", func(t *testing.T) {
		ts := newTimelineServer(t)
		wd := ts.driver(t)

		tl := selenium.NewActionTimeline()
		kbd := tl.AddKeyboard("kbd")
		f1 := tl.AddPointer("finger-1", selenium.TouchPointer)
		f2 := tl.AddPointer("finger-2", selenium.TouchPointer)

		// finger-1: (100,100)@0 -> (300,100)@400 -> (300,500)@1000.
		f1.Move(0, selenium.ViewportPoint(100, 100)).Down(0, selenium.LeftButton)
		f1.Move(400*time.Millisecond, selenium.ViewportPoint(300, 100))
		f1.Move(time.Second, selenium.ViewportPoint(300, 500)).Up(time.Second, selenium.LeftButton)

		// finger-2: (500,500)@0 -> (100,500)@1000, a single segment with
		// no key point at 400ms; the merged timeline must synthesize one.
		f2.Move(0, selenium.ViewportPoint(500, 500)).Down(0, selenium.LeftButton)
		f2.Move(time.Second, selenium.ViewportPoint(100, 500)).Up(time.Second, selenium.LeftButton)

		kbd.KeyDown(0, selenium.ShiftKey).KeyUp(time.Second, selenium.ShiftKey)

		if err := wd.PerformActionTimeline(tl); err != nil {
			t.Fatalf("PerformActionTimeline: %v", err)
		}
		if len(ts.requests) != 1 {
			t.Fatalf("got %d actions requests, want 1", len(ts.requests))
		}
		if ts.rectCalls != 0 {
			t.Fatalf("got %d rect calls, want 0", ts.rectCalls)
		}

		srcs := ts.requests[0].Actions
		if len(srcs) != 3 {
			t.Fatalf("got %d input sources, want 3", len(srcs))
		}
		if srcs[0].ID != "kbd" || srcs[0].Type != "key" {
			t.Errorf("source 0: %+v", srcs[0])
		}
		for i, id := range []string{"finger-1", "finger-2"} {
			if srcs[i+1].ID != id || srcs[i+1].Type != "pointer" ||
				srcs[i+1].Parameters["pointerType"] != "touch" {
				t.Errorf("source %d: %+v", i+1, srcs[i+1])
			}
		}

		// All sources must have the same number of ticks: 2 move ticks
		// (t=0, t=400, t=1000 is 3) plus 3 discrete ticks at t=0 and 3 at
		// t=1000.
		const wantTicks = 9
		for i := range srcs {
			if len(srcs[i].Actions) != wantTicks {
				t.Fatalf("source %d has %d ticks, want %d", i, len(srcs[i].Actions), wantTicks)
			}
		}
		k, a, b := srcs[0].Actions, srcs[1].Actions, srcs[2].Actions

		// Independent per-segment velocity reference for the synthesized
		// intermediate point of finger-2 at t=400.
		wantF2X := segmentPosition(0, 500, 1000, 100, 400)    // 340
		wantF2Y := segmentPosition(0, 500, 1000, 500, 400)    // 500
		wantF1Y := segmentPosition(400, 100, 1000, 500, 1000) // 500

		// t=0: positioning moves come before the button presses.
		wantPause(t, k[0], 0)
		wantMove(t, a[0], 0, 100, 100)
		wantMove(t, b[0], 0, 500, 500)
		// t=0: discrete events in input order, one zero-duration tick each.
		wantPause(t, k[1], 0)
		wantButton(t, a[1], "pointerDown", 0)
		wantPause(t, b[1], 0)

		wantPause(t, k[2], 0)
		wantPause(t, a[2], 0)
		wantButton(t, b[2], "pointerDown", 0)

		wantKey(t, k[3], "keyDown", selenium.ShiftKey)
		wantPause(t, a[3], 0)
		wantPause(t, b[3], 0)

		// t=400: finger-1 reaches its second key point, finger-2 gets a
		// synthesized intermediate end point on its single segment.
		wantPause(t, k[4], 400)
		wantMove(t, a[4], 400, 300, 100)
		wantMove(t, b[4], 400, wantF2X, wantF2Y)

		// t=1000: both trajectories end.
		wantPause(t, k[5], 600)
		wantMove(t, a[5], 600, 300, wantF1Y)
		wantMove(t, b[5], 600, 100, 500)

		// t=1000: releases only after the final moves completed.
		wantPause(t, k[6], 0)
		wantButton(t, a[6], "pointerUp", 0)
		wantPause(t, b[6], 0)

		wantPause(t, k[7], 0)
		wantPause(t, a[7], 0)
		wantButton(t, b[7], "pointerUp", 0)

		wantKey(t, k[8], "keyUp", selenium.ShiftKey)
		wantPause(t, a[8], 0)
		wantPause(t, b[8], 0)
	})

	t.Run("non_zero_start", func(t *testing.T) {
		ts := newTimelineServer(t)
		wd := ts.driver(t)

		tl := selenium.NewActionTimeline()
		f1 := tl.AddPointer("finger-1", selenium.TouchPointer)
		f1.Move(250*time.Millisecond, selenium.ViewportPoint(10, 10))
		f1.Move(750*time.Millisecond, selenium.ViewportPoint(110, 10))

		if err := wd.PerformActionTimeline(tl); err != nil {
			t.Fatalf("PerformActionTimeline: %v", err)
		}
		if len(ts.requests) != 1 {
			t.Fatalf("got %d actions requests, want 1", len(ts.requests))
		}
		acts := ts.requests[0].Actions[0].Actions
		if len(acts) != 3 {
			t.Fatalf("got %d ticks, want 3", len(acts))
		}
		// Before the trajectory starts the pointer stays on the first key
		// point: an instantaneous positioning move at t=0, then holding
		// the end point until t=250.
		wantMove(t, acts[0], 0, 10, 10)
		wantMove(t, acts[1], 250, 10, 10)
		wantMove(t, acts[2], 500, 110, 10)
	})

	t.Run("single_point_trajectory", func(t *testing.T) {
		ts := newTimelineServer(t)
		wd := ts.driver(t)

		tl := selenium.NewActionTimeline()
		f1 := tl.AddPointer("finger-1", selenium.TouchPointer)
		f1.Move(100*time.Millisecond, selenium.ViewportPoint(50, 60))
		f1.Down(100*time.Millisecond, selenium.LeftButton)
		f1.Up(100*time.Millisecond, selenium.LeftButton)

		if err := wd.PerformActionTimeline(tl); err != nil {
			t.Fatalf("PerformActionTimeline: %v", err)
		}
		if len(ts.requests) != 1 {
			t.Fatalf("got %d actions requests, want 1", len(ts.requests))
		}
		acts := ts.requests[0].Actions[0].Actions
		if len(acts) != 4 {
			t.Fatalf("got %d ticks, want 4", len(acts))
		}
		wantMove(t, acts[0], 0, 50, 60)
		wantMove(t, acts[1], 100, 50, 60)
		// The down and up at the same moment must not be merged.
		wantButton(t, acts[2], "pointerDown", 0)
		wantButton(t, acts[3], "pointerUp", 0)
	})

	t.Run("element_relative_rect_resampled", func(t *testing.T) {
		ts := newTimelineServer(t)
		ts.rectValues = []string{
			`{"value":{"x":100,"y":200,"width":400,"height":300}}`,
			`{"value":{"x":10,"y":20,"width":100,"height":50}}`,
		}
		wd := ts.driver(t)
		elem := decodeElement(t, wd, "elem-1")

		tl := selenium.NewActionTimeline()
		f1 := tl.AddPointer("finger-1", selenium.TouchPointer)
		// The same element is referenced by both key points; its rectangle
		// must be read only once per execution.
		f1.Move(0, selenium.ElementPoint(elem, 0, 0))
		f1.Move(500*time.Millisecond, selenium.ElementPoint(elem, 0.5, 0.5))

		if err := wd.PerformActionTimeline(tl); err != nil {
			t.Fatalf("PerformActionTimeline: %v", err)
		}
		if ts.rectCalls != 1 {
			t.Fatalf("got %d rect calls, want 1", ts.rectCalls)
		}
		if len(ts.requests) != 1 {
			t.Fatalf("got %d actions requests, want 1", len(ts.requests))
		}
		acts := ts.requests[0].Actions[0].Actions
		if len(acts) != 2 {
			t.Fatalf("got %d ticks, want 2", len(acts))
		}
		// (0,0) is the element's top-left corner, (0.5,0.5) its center.
		wantMove(t, acts[0], 0, 100, 200)
		wantMove(t, acts[1], 500, 100+0.5*400, 200+0.5*300)

		// The second execution re-reads the rectangle and gets the new
		// geometry.
		if err := wd.PerformActionTimeline(tl); err != nil {
			t.Fatalf("PerformActionTimeline: %v", err)
		}
		if ts.rectCalls != 2 {
			t.Fatalf("got %d rect calls, want 2", ts.rectCalls)
		}
		if len(ts.requests) != 2 {
			t.Fatalf("got %d actions requests, want 2", len(ts.requests))
		}
		acts = ts.requests[1].Actions[0].Actions
		if len(acts) != 2 {
			t.Fatalf("got %d ticks, want 2", len(acts))
		}
		wantMove(t, acts[0], 0, 10, 20)
		wantMove(t, acts[1], 500, 10+0.5*100, 20+0.5*50)
	})

	t.Run("rect_read_failure", func(t *testing.T) {
		ts := newTimelineServer(t)
		ts.rectStatus = http.StatusNotFound
		ts.rectBody = `{"value":{"error":"stale element reference","message":"stale element reference: element is not attached to the page document"}}`
		wd := ts.driver(t)
		elem := decodeElement(t, wd, "elem-1")

		tl := selenium.NewActionTimeline()
		f1 := tl.AddPointer("finger-1", selenium.TouchPointer)
		f1.Move(0, selenium.ElementPoint(elem, 0.5, 0.5))

		err := wd.PerformActionTimeline(tl)
		if err == nil {
			t.Fatal("PerformActionTimeline succeeded, want stale element error")
		}
		if !strings.Contains(err.Error(), "stale element reference") {
			t.Fatalf("error %q does not propagate the WebDriver error", err)
		}
		if len(ts.requests) != 0 {
			t.Fatalf("got %d actions requests, want 0", len(ts.requests))
		}
	})

	t.Run("input_validation", func(t *testing.T) {
		ts := newTimelineServer(t)
		wd := ts.driver(t)

		// A second session, to mint an element that does not belong to
		// the first one.
		ts2 := newTimelineServer(t)
		wd2 := ts2.driver(t)
		foreign := decodeElement(t, wd2, "elem-1")

		cases := map[string]func() *selenium.ActionTimeline{
			"negative_event_time": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(-time.Millisecond, selenium.ViewportPoint(0, 0))
				return tl
			},
			"duplicate_source_names": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(0, selenium.ViewportPoint(0, 0))
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(0, selenium.ViewportPoint(1, 1))
				return tl
			},
			"equal_move_times": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				p := tl.AddPointer("finger", selenium.TouchPointer)
				p.Move(100*time.Millisecond, selenium.ViewportPoint(0, 0))
				p.Move(100*time.Millisecond, selenium.ViewportPoint(1, 1))
				return tl
			},
			"decreasing_move_times": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				p := tl.AddPointer("finger", selenium.TouchPointer)
				p.Move(200*time.Millisecond, selenium.ViewportPoint(0, 0))
				p.Move(100*time.Millisecond, selenium.ViewportPoint(1, 1))
				return tl
			},
			"nan_coordinate": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(0, selenium.ViewportPoint(math.NaN(), 0))
				return tl
			},
			"infinite_coordinate": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(0, selenium.ViewportPoint(0, math.Inf(1)))
				return tl
			},
			"key_event_on_pointer_source": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).KeyDown(0, "a")
				return tl
			},
			"pointer_event_on_key_source": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddKeyboard("kbd").Down(0, selenium.LeftButton)
				return tl
			},
			"element_from_other_session": func() *selenium.ActionTimeline {
				tl := selenium.NewActionTimeline()
				tl.AddPointer("finger", selenium.TouchPointer).
					Move(0, selenium.ElementPoint(foreign, 0.5, 0.5))
				return tl
			},
		}
		for name, build := range cases {
			t.Run(name, func(t *testing.T) {
				before := len(ts.requests)
				rectsBefore := ts.rectCalls
				if err := wd.PerformActionTimeline(build()); err == nil {
					t.Fatal("PerformActionTimeline succeeded, want validation error")
				}
				if len(ts.requests) != before {
					t.Errorf("actions request sent for invalid timeline")
				}
				if ts.rectCalls != rectsBefore {
					t.Errorf("rectangle read before input validation finished")
				}
			})
		}
	})

	t.Run("legacy_session_not_supported", func(t *testing.T) {
		ts := newTimelineServer(t)
		ts.legacy = true
		wd := ts.driver(t)

		tl := selenium.NewActionTimeline()
		tl.AddPointer("finger", selenium.TouchPointer).
			Move(0, selenium.ViewportPoint(1, 1))

		err := wd.PerformActionTimeline(tl)
		if err == nil {
			t.Fatal("PerformActionTimeline succeeded on a legacy session")
		}
		if !strings.Contains(err.Error(), "not supported") {
			t.Fatalf("error %q does not report the lack of support", err)
		}
		if len(ts.requests) != 0 {
			t.Fatalf("got %d actions requests, want 0", len(ts.requests))
		}
	})
}
