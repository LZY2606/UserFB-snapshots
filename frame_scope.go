package agouti

import (
	"errors"
	"fmt"
	"strings"
	"sync"

	"github.com/sclevine/agouti/internal/target"
)

// ErrFrameContextUnknown is returned by frame-scoped and root Selection
// operations (Count, Text, Click and Fill) when a previous operation failed
// to return the session to the root frame. The Page frame context must be
// recovered with a successful Page.SwitchToRootFrame call before any further
// operation can be attempted.
var ErrFrameContextUnknown = errors.New("frame context is unknown: call Page.SwitchToRootFrame to recover")

// frameContext serializes the frame-aware operations of a single Page and
// records whether the current frame focus can still be trusted.
type frameContext struct {
	mu      sync.Mutex
	unknown bool
	session apiSession
}

func newFrameContext(session apiSession) *frameContext {
	return &frameContext{session: session}
}

// begin acquires the operation boundary for the Page. It returns the unlock
// function, or ErrFrameContextUnknown without sending any request when the
// frame context has been marked uncertain.
func (f *frameContext) begin() (func(), error) {
	f.mu.Lock()
	if f.unknown {
		f.mu.Unlock()
		return nil, ErrFrameContextUnknown
	}
	return f.mu.Unlock, nil
}

// markRecovered clears the uncertain-frame state after an explicit,
// successful return to the root frame.
func (f *frameContext) markRecovered() {
	f.mu.Lock()
	f.unknown = false
	f.mu.Unlock()
}

// runRoot runs an ordinary root selection inside the shared operation
// boundary without changing the current frame focus.
func (f *frameContext) runRoot(action func() error) error {
	release, err := f.begin()
	if err != nil {
		return err
	}
	defer release()
	return action()
}

// runScoped runs a frame-scoped operation: it returns to the root frame,
// descends the frame path segment by segment, runs the field action and
// always attempts to return to the root frame afterwards.
func (f *frameContext) runScoped(path []string, action func() error) error {
	release, err := f.begin()
	if err != nil {
		return err
	}
	defer release()

	for _, segment := range path {
		if strings.TrimSpace(segment) == "" {
			return fmt.Errorf("invalid frame path %q: empty CSS selector segment", path)
		}
	}

	// A zero-segment path is the root page itself: the field action runs
	// under the shared boundary without any frame switch.
	if len(path) == 0 {
		return action()
	}

	if err := f.session.Frame(nil); err != nil {
		// The initial return to the root failed: do not resolve the path.
		return fmt.Errorf("failed to switch to original page frame: %w", err)
	}

	opErr := enterFrames(f.session, path)
	if opErr == nil {
		opErr = action()
	}

	// The initial switch succeeded, so the recovery switch is always
	// attempted even when the path resolution or field action failed.
	if recoveryErr := f.session.Frame(nil); recoveryErr != nil {
		f.unknown = true
		recoveryErr = fmt.Errorf("failed to return to root frame: %w", recoveryErr)
		// The sentinel is joined in so errors.Is reports the uncertain
		// state on this operation as well as on every later one.
		return errors.Join(opErr, recoveryErr, ErrFrameContextUnknown)
	}

	return opErr
}

// enterFrames resolves each CSS segment against the current frame, requiring
// exactly one matching iframe, and switches into it. Frame element handles
// are resolved on every call, so a replaced iframe is located by its new ID.
func enterFrames(session apiSession, path []string) error {
	for _, css := range path {
		selector := target.Selector{Type: target.CSS, Value: css, Single: true}
		elements, err := session.GetElements(selector.API())
		if err != nil {
			return fmt.Errorf("failed to select frame %q: %w", css, err)
		}
		if len(elements) == 0 {
			return fmt.Errorf("failed to select frame %q: element not found", css)
		}
		if len(elements) > 1 {
			return fmt.Errorf("failed to select frame %q: ambiguous find (%d iframes matched)", css, len(elements))
		}
		if err := session.Frame(elements[0]); err != nil {
			return fmt.Errorf("failed to switch to frame %q: %w", css, err)
		}
	}
	return nil
}

// A FrameScope describes a path of nested iframes from the root page. Scopes
// are cheap to create: deriving scopes or selections never contacts the
// WebDriver or modifies any existing object. The frame path is applied
// afresh, segment by segment, for every terminal operation.
type FrameScope struct {
	page *Page
	path []string
}

// InFrames returns a new FrameScope whose frame path is the receiver's path
// followed by the provided CSS selectors. The receiver is not modified.
func (s *FrameScope) InFrames(css ...string) *FrameScope {
	path := make([]string, 0, len(s.path)+len(css))
	path = append(path, s.path...)
	path = append(path, css...)
	return &FrameScope{page: s.page, path: path}
}

// Find returns a *Selection carrying the scope's frame path. The selector is
// resolved in the final frame of the path when a terminal operation runs.
func (s *FrameScope) Find(css string) *Selection {
	path := append([]string(nil), s.path...)
	return newSelection(s.page.session, s.page.frames, path,
		target.Selectors(nil).Append(target.CSS, css).Single())
}

// All returns a *MultiSelection carrying the scope's frame path. The
// selector is resolved in the final frame of the path when a terminal
// operation runs.
func (s *FrameScope) All(css string) *MultiSelection {
	path := append([]string(nil), s.path...)
	return newMultiSelection(s.page.session, s.page.frames, path,
		target.Selectors(nil).Append(target.CSS, css))
}

// runFrameOperation routes one of the four supported terminal operations
// through the shared Page boundary, using the scoped frame protocol when the
// selection carries a frame path and the plain root protocol otherwise.
// Selections constructed without a frame context keep their legacy behavior.
func (s *Selection) runFrameOperation(action func() error) error {
	if s.frames == nil {
		return action()
	}
	if s.framePath != nil {
		return s.frames.runScoped(s.framePath, action)
	}
	return s.frames.runRoot(action)
}
