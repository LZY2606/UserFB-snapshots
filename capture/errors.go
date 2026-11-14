// Package capture collects response bodies of intercepted page requests
// over the Chrome DevTools Protocol while letting the page continue.
package capture

import "errors"

// ErrBodyTooLarge is returned when a decoded response body exceeds the
// MaxBodyBytes limit configured in Options.
var ErrBodyTooLarge = errors.New("capture: response body exceeds MaxBodyBytes")
