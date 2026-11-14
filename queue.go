package ant

import (
	"context"
	"io"
	"sync"
)

// Queue represents a URL queue.
//
// A queue must be safe to use from multiple goroutines.
type Queue interface {
	// Enqueue enqueues the given set of URLs.
	//
	// The method returns an io.EOF if the queue was
	// closed and a context error if the context was
	// canceled.
	//
	// Any other error will be treated as a critical
	// error and will be porpagated.
	Enqueue(ctx context.Context, urls URLs) error

	// Dequeue dequeues a URL.
	//
	// The method returns a URL or io.EOF error if
	// the queue was stopped.
	//
	// The method blocks until a URL is available, the
	// queue is closed or the given context is canceled,
	// in which case the context's error is returned.
	Dequeue(ctx context.Context) (*URL, error)

	// Done acknowledges a URL.
	//
	// When a URL has been handled by the engine the method
	// is called with the URL.
	Done(ctx context.Context, url *URL) error

	// Wait blocks until every URL that was enqueued has
	// either been dequeued and acknowledged with Done or
	// been discarded by Close.
	Wait()

	// Close closes the queue.
	//
	// The method blocks until the queue is closed, any URLs
	// that were still queued are discarded and must not be
	// acknowledged with Done.
	//
	// Closing an already closed queue is a no-op and must
	// not account for the discarded URLs a second time.
	Close(context.Context) error
}

// MemoryQueue implements a naive in-memory queue.
type memoryQueue struct {
	mu       sync.Mutex
	cond     *sync.Cond
	pending  URLs
	stopped  bool
	wg       sync.WaitGroup
	closeOne sync.Once
}

// MemoryQueue returns a new memory queue.
func MemoryQueue(size int) Queue {
	mq := &memoryQueue{
		pending: make(URLs, 0, size),
	}
	mq.cond = sync.NewCond(&mq.mu)
	return mq
}

// Enqueue implementation.
func (mq *memoryQueue) Enqueue(ctx context.Context, urls URLs) error {
	if len(urls) == 0 {
		return nil
	}

	mq.mu.Lock()
	defer mq.mu.Unlock()

	if mq.stopped {
		return io.EOF
	}

	if err := ctx.Err(); err != nil {
		return err
	}

	mq.pending = append(mq.pending, urls...)
	mq.wg.Add(len(urls))
	mq.cond.Broadcast()

	return nil
}

// Dequeue implementation.
func (mq *memoryQueue) Dequeue(ctx context.Context) (*URL, error) {
	mq.mu.Lock()
	defer mq.mu.Unlock()

	var stop func()
	defer func() {
		if stop != nil {
			stop()
		}
	}()

	for len(mq.pending) == 0 {
		if mq.stopped {
			return nil, io.EOF
		}
		if err := ctx.Err(); err != nil {
			return nil, err
		}

		// A blocked Dequeue must wake up on its own when the
		// context is canceled, without relying on an external
		// Enqueue or Close call.
		if stop == nil {
			stop = mq.watchContext(ctx)
		}
		mq.cond.Wait()
	}

	url := mq.pending[0]
	mq.pending = mq.pending[1:]

	return url, nil
}

// Done implementation.
func (mq *memoryQueue) Done(_ context.Context, _ *URL) error {
	mq.wg.Done()
	return nil
}

// Wait implementation.
func (mq *memoryQueue) Wait() {
	mq.wg.Wait()
}

// Close implementation.
func (mq *memoryQueue) Close(_ context.Context) error {
	mq.closeOne.Do(func() {
		mq.mu.Lock()
		pending := mq.pending
		mq.pending = mq.pending[:0]
		mq.stopped = true
		for range pending {
			mq.wg.Done()
		}
		mq.cond.Broadcast()
		mq.mu.Unlock()
	})
	return nil
}

// watchContext broadcasts on the queue's cond when ctx is
// canceled, waking every Dequeue blocked in cond.Wait.
//
// The returned function stops the watcher and must be called
// when the caller is done waiting.
func (mq *memoryQueue) watchContext(ctx context.Context) func() {
	if err := ctx.Err(); err != nil {
		return func() {}
	}

	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			mq.mu.Lock()
			mq.cond.Broadcast()
			mq.mu.Unlock()
		case <-done:
		}
	}()

	return func() { close(done) }
}
