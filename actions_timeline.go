// Action timelines: a high-level API that compiles named input sources with
// time-stamped events into a single W3C WebDriver Actions request.
//
// Unlike StoreKeyActions/StorePointerActions, where the caller is responsible
// for aligning the per-source ticks by hand, an ActionTimeline lets every
// input source use its own schedule. The timelines are merged into one common
// set of ticks when the timeline is performed, so that pointers whose
// trajectories have key points at different moments still move at their
// original speeds.

package selenium

import (
	"fmt"
	"math"
	"sort"
	"time"
)

// TimelinePoint is a position used by a pointer trajectory in an
// ActionTimeline.
//
// When Element is nil, X and Y are absolute viewport coordinates in CSS
// pixels. When Element is set, X and Y are normalized coordinates relative to
// the element's rectangle: (0, 0) is the element's top-left corner and
// (0.5, 0.5) is its center. The element's rectangle is read once per
// PerformActionTimeline call and all points referencing it are converted to
// viewport coordinates before the actions are sent.
type TimelinePoint struct {
	X, Y    float64
	Element WebElement
}

// ViewportPoint returns a TimelinePoint in absolute viewport coordinates.
func ViewportPoint(x, y float64) TimelinePoint {
	return TimelinePoint{X: x, Y: y}
}

// ElementPoint returns a TimelinePoint in normalized coordinates relative to
// the rectangle of elem: (0, 0) is the element's top-left corner, (1, 1) its
// bottom-right corner.
func ElementPoint(elem WebElement, x, y float64) TimelinePoint {
	return TimelinePoint{X: x, Y: y, Element: elem}
}

// timelineEventKind identifies the type of a single timeline event.
type timelineEventKind int

const (
	timelineMove timelineEventKind = iota
	timelinePointerDown
	timelinePointerUp
	timelineKeyDown
	timelineKeyUp
)

// timelineEvent is a single event on a TimelineSource. Move events are the
// key points of the source's pointer trajectory; the other kinds are discrete
// events happening at a single moment.
type timelineEvent struct {
	kind   timelineEventKind
	at     time.Duration
	seq    int // input order across the whole timeline
	point  TimelinePoint
	button MouseButton
	key    string
}

// timelineSourceKind identifies whether a source is a keyboard or a pointer.
type timelineSourceKind int

const (
	timelineKeySource timelineSourceKind = iota
	timelinePointerSource
)

// TimelineSource is a single named input source of an ActionTimeline, either
// a keyboard or a pointer (mouse, touch or pen). Events are attached to it
// with the Move, Down, Up, KeyDown and KeyUp methods, all of which return the
// source so calls can be chained.
type TimelineSource struct {
	tl      *ActionTimeline
	name    string
	kind    timelineSourceKind
	pointer PointerType
	events  []timelineEvent
}

func (s *TimelineSource) add(ev timelineEvent) {
	ev.seq = s.tl.seq
	s.tl.seq++
	s.events = append(s.events, ev)
}

// Move adds a key point of the pointer's trajectory: at moment at the pointer
// is at position p. Between two key points the pointer moves at a constant
// speed; before the first and after the last key point it stays on the
// respective end point. Key point times must be non-negative and strictly
// increasing within a source.
func (s *TimelineSource) Move(at time.Duration, p TimelinePoint) *TimelineSource {
	s.add(timelineEvent{kind: timelineMove, at: at, point: p})
	return s
}

// Down presses the pointer button at moment at.
func (s *TimelineSource) Down(at time.Duration, button MouseButton) *TimelineSource {
	s.add(timelineEvent{kind: timelinePointerDown, at: at, button: button})
	return s
}

// Up releases the pointer button at moment at.
func (s *TimelineSource) Up(at time.Duration, button MouseButton) *TimelineSource {
	s.add(timelineEvent{kind: timelinePointerUp, at: at, button: button})
	return s
}

// KeyDown presses and holds key at moment at.
func (s *TimelineSource) KeyDown(at time.Duration, key string) *TimelineSource {
	s.add(timelineEvent{kind: timelineKeyDown, at: at, key: key})
	return s
}

// KeyUp releases key at moment at.
func (s *TimelineSource) KeyUp(at time.Duration, key string) *TimelineSource {
	s.add(timelineEvent{kind: timelineKeyUp, at: at, key: key})
	return s
}

// ActionTimeline is a set of named input sources whose events are scheduled
// on a common timeline, in milliseconds. It is executed on an existing
// WebDriver session with PerformActionTimeline, which compiles the timeline
// into a single W3C Actions request.
//
// An ActionTimeline is not consumed by performing it: the same timeline can
// be performed again, and it does not interact with actions stored through
// StoreKeyActions or StorePointerActions.
type ActionTimeline struct {
	sources []*TimelineSource
	seq     int
}

// NewActionTimeline returns an empty action timeline.
func NewActionTimeline() *ActionTimeline {
	return &ActionTimeline{}
}

// AddKeyboard adds a key input source with the given name to the timeline.
func (t *ActionTimeline) AddKeyboard(name string) *TimelineSource {
	return t.addSource(&TimelineSource{name: name, kind: timelineKeySource})
}

// AddPointer adds a pointer input source with the given name and pointer type
// (MousePointer, TouchPointer or PenPointer) to the timeline.
func (t *ActionTimeline) AddPointer(name string, pointer PointerType) *TimelineSource {
	return t.addSource(&TimelineSource{name: name, kind: timelinePointerSource, pointer: pointer})
}

func (t *ActionTimeline) addSource(s *TimelineSource) *TimelineSource {
	s.tl = t
	t.sources = append(t.sources, s)
	return s
}

// validate performs all checks that do not require talking to the WebDriver
// server.
func (t *ActionTimeline) validate() error {
	if t == nil || len(t.sources) == 0 {
		return fmt.Errorf("selenium: action timeline has no input sources")
	}
	seen := make(map[string]bool, len(t.sources))
	for _, src := range t.sources {
		if seen[src.name] {
			return fmt.Errorf("selenium: action timeline: duplicate input source name %q", src.name)
		}
		seen[src.name] = true

		var lastMove time.Duration
		hasMove := false
		for _, ev := range src.events {
			if ev.at < 0 {
				return fmt.Errorf("selenium: action timeline: source %q: negative event time %s", src.name, ev.at)
			}
			switch src.kind {
			case timelineKeySource:
				if ev.kind != timelineKeyDown && ev.kind != timelineKeyUp {
					return fmt.Errorf("selenium: action timeline: source %q: pointer event on a key input source", src.name)
				}
			case timelinePointerSource:
				if ev.kind == timelineKeyDown || ev.kind == timelineKeyUp {
					return fmt.Errorf("selenium: action timeline: source %q: key event on a pointer input source", src.name)
				}
			}
			if ev.kind != timelineMove {
				continue
			}
			if hasMove && ev.at <= lastMove {
				return fmt.Errorf("selenium: action timeline: source %q: move times are not strictly increasing", src.name)
			}
			hasMove, lastMove = true, ev.at
			if math.IsNaN(ev.point.X) || math.IsInf(ev.point.X, 0) ||
				math.IsNaN(ev.point.Y) || math.IsInf(ev.point.Y, 0) {
				return fmt.Errorf("selenium: action timeline: source %q: non-finite coordinate (%v, %v)", src.name, ev.point.X, ev.point.Y)
			}
		}
	}
	return nil
}

// timelineKeyframe is a trajectory key point resolved to viewport
// coordinates, with its time in milliseconds.
type timelineKeyframe struct {
	at   int64
	x, y float64
}

// positionAt returns the trajectory position at moment at (milliseconds),
// moving at a constant speed between key points and staying on the end points
// outside of them. keyframes must be non-empty and sorted by time.
func positionAt(keyframes []timelineKeyframe, at int64) (float64, float64) {
	first := keyframes[0]
	if at <= first.at {
		return first.x, first.y
	}
	last := keyframes[len(keyframes)-1]
	if at >= last.at {
		return last.x, last.y
	}
	for i := 0; i+1 < len(keyframes); i++ {
		a, b := keyframes[i], keyframes[i+1]
		if at >= a.at && at <= b.at {
			f := float64(at-a.at) / float64(b.at-a.at)
			return a.x + f*(b.x-a.x), a.y + f*(b.y-a.y)
		}
	}
	return last.x, last.y
}

// PerformActionTimeline compiles the action timeline t into a single W3C
// WebDriver Actions request and executes it on the current session.
//
// All trajectory key points and discrete event times of every input source
// are merged into one common sequence of ticks: at every merged moment each
// moving pointer gets a move action to its interpolated position (rounded
// with math.Round) whose duration is the difference to the previous moment,
// and every other source gets a pause of the same duration. Discrete events
// (button and key presses and releases) happening at the same moment are sent
// as individual zero-duration ticks in input order, with the other sources
// pausing; a positioning move at a given moment is always sent before button
// presses at that moment, and button releases are sent only after the move
// ending at that moment has completed. Every input source appears exactly
// once in the request and all sources have the same number of ticks.
//
// Before anything is sent, the timeline is validated: negative event times,
// duplicate source names, non-strictly-increasing move times, non-finite
// coordinates, events that do not match their source's type and elements
// belonging to a different session are all rejected without issuing any
// request. Then the rectangle of every referenced element is read exactly
// once (rectangles are re-read on every call, never cached), and only then is
// a single POST actions request issued. Errors returned while reading an
// element's rectangle, including stale element references, are returned
// unchanged.
//
// The timeline is not consumed by this call and actions previously stored
// with StoreKeyActions or StorePointerActions are left untouched. Legacy
// (non-W3C) protocol sessions are not supported.
func (wd *remoteWD) PerformActionTimeline(t *ActionTimeline) error {
	if !wd.w3cCompatible {
		return fmt.Errorf("selenium: action timelines are not supported on legacy (non-W3C) protocol sessions")
	}
	if err := t.validate(); err != nil {
		return err
	}

	// Validate every referenced element before reading any rectangle.
	for _, src := range t.sources {
		for _, ev := range src.events {
			if ev.kind != timelineMove || ev.point.Element == nil {
				continue
			}
			elem, ok := ev.point.Element.(*remoteWE)
			if !ok {
				return fmt.Errorf("selenium: action timeline: source %q: element of unsupported type %T", src.name, ev.point.Element)
			}
			if elem.parent != wd {
				return fmt.Errorf("selenium: action timeline: source %q: element belongs to a different session", src.name)
			}
		}
	}

	// Read the full rectangle of every referenced element exactly once.
	rects := make(map[string]*rect)
	for _, src := range t.sources {
		for _, ev := range src.events {
			if ev.kind != timelineMove || ev.point.Element == nil {
				continue
			}
			elem := ev.point.Element.(*remoteWE)
			if _, ok := rects[elem.id]; ok {
				continue
			}
			r, err := elem.rect()
			if err != nil {
				return err
			}
			rects[elem.id] = r
		}
	}

	// Resolve the trajectories to viewport coordinates and merge every key
	// point and discrete event time into one common schedule.
	keyframes := make([][]timelineKeyframe, len(t.sources))
	times := map[int64]bool{0: true}
	toMilliseconds := func(d time.Duration) int64 { return int64(d / time.Millisecond) }
	for i, src := range t.sources {
		for _, ev := range src.events {
			times[toMilliseconds(ev.at)] = true
			if ev.kind != timelineMove {
				continue
			}
			x, y := ev.point.X, ev.point.Y
			if ev.point.Element != nil {
				r := rects[ev.point.Element.(*remoteWE).id]
				x = r.X + x*r.Width
				y = r.Y + y*r.Height
			}
			keyframes[i] = append(keyframes[i], timelineKeyframe{at: toMilliseconds(ev.at), x: x, y: y})
		}
	}
	merged := make([]int64, 0, len(times))
	for tm := range times {
		merged = append(merged, tm)
	}
	sort.Slice(merged, func(i, j int) bool { return merged[i] < merged[j] })

	// All discrete events of the timeline, in input order.
	type discreteEvent struct {
		src int
		ev  timelineEvent
	}
	var discreteEvents []discreteEvent
	for i, src := range t.sources {
		for _, ev := range src.events {
			if ev.kind != timelineMove {
				discreteEvents = append(discreteEvents, discreteEvent{src: i, ev: ev})
			}
		}
	}
	sort.Slice(discreteEvents, func(i, j int) bool { return discreteEvents[i].ev.seq < discreteEvents[j].ev.seq })

	pause := func(duration int64) map[string]interface{} {
		return map[string]interface{}{"type": "pause", "duration": duration}
	}
	discrete := func(ev timelineEvent) map[string]interface{} {
		switch ev.kind {
		case timelinePointerDown:
			return map[string]interface{}{"type": "pointerDown", "button": ev.button}
		case timelinePointerUp:
			return map[string]interface{}{"type": "pointerUp", "button": ev.button}
		case timelineKeyDown:
			return map[string]interface{}{"type": "keyDown", "value": ev.key}
		default: // timelineKeyUp
			return map[string]interface{}{"type": "keyUp", "value": ev.key}
		}
	}

	ticks := make([][]interface{}, len(t.sources))
	prev := int64(0)
	for _, tm := range merged {
		duration := tm - prev
		prev = tm

		// The positioning tick for this moment comes first: every pointer
		// with a trajectory moves to its position at tm, every other source
		// pauses for the same duration.
		for i, src := range t.sources {
			if src.kind == timelinePointerSource && len(keyframes[i]) > 0 {
				x, y := positionAt(keyframes[i], tm)
				ticks[i] = append(ticks[i], map[string]interface{}{
					"type":     "pointerMove",
					"duration": duration,
					"origin":   FromViewport,
					"x":        int64(math.Round(x)),
					"y":        int64(math.Round(y)),
				})
			} else {
				ticks[i] = append(ticks[i], pause(duration))
			}
		}

		// Discrete events at this moment follow, one zero-duration tick per
		// event, in input order.
		for _, de := range discreteEvents {
			if toMilliseconds(de.ev.at) != tm {
				continue
			}
			for j := range t.sources {
				if j == de.src {
					ticks[j] = append(ticks[j], discrete(de.ev))
				} else {
					ticks[j] = append(ticks[j], pause(0))
				}
			}
		}
	}

	actions := make([]map[string]interface{}, len(t.sources))
	for i, src := range t.sources {
		switch src.kind {
		case timelineKeySource:
			actions[i] = map[string]interface{}{
				"type":    "key",
				"id":      src.name,
				"actions": ticks[i],
			}
		default:
			actions[i] = map[string]interface{}{
				"type":       "pointer",
				"id":         src.name,
				"parameters": map[string]string{"pointerType": string(src.pointer)},
				"actions":    ticks[i],
			}
		}
	}

	return wd.voidCommand("/session/%s/actions", map[string]interface{}{
		"actions": actions,
	})
}
