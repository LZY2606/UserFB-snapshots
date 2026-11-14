package frontier

import (
	"context"
	"net/url"
)

// Queue defines the interface for both local and distributed frontiers.
type Queue interface {
	// Push adds a URL to the queue if it hasn't been visited.
	// Returns true if the URL was added.
	Push(u *url.URL, depth int) bool

	// Pop blocks until an item is available or the frontier is closed.
	// Returns the item and true, or zero-value and false when closed.
	Pop() (Item, bool)

	// PopContext blocks like Pop but also returns when ctx is done.
	PopContext(ctx context.Context) (Item, bool)

	// Len returns the current number of queued URLs.
	Len() int

	// VisitedCount returns the number of unique URLs seen.
	VisitedCount() uint32

	// Close signals that no more URLs will be pushed.
	Close()
}
