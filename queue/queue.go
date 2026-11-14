package queue

import (
	"net/url"
	"sync"

	whatwgUrl "github.com/nlnwa/whatwg-url/url"

	"github.com/gocolly/colly/v2"
)

var urlParser = whatwgUrl.NewParser(whatwgUrl.WithPercentEncodeSinglePercentSign())

// Storage is the interface of the queue's storage backend
// Storage must be concurrently safe for multiple goroutines.
type Storage interface {
	// Init initializes the storage
	Init() error
	// AddRequest adds a serialized request to the queue
	AddRequest([]byte) error
	// GetRequest pops the next request from the queue
	// or returns error if the queue is empty
	GetRequest() ([]byte, error)
	// QueueSize returns with the size of the queue
	QueueSize() (int, error)
}

// Queue is a request queue which uses a Collector to consume
// requests in multiple threads
type Queue struct {
	// Threads defines the number of consumer threads. It bounds the number
	// of requests dispatched by this queue that are processed at the same
	// time. A slot is occupied from dispatch until the request's body,
	// parsing and user callbacks have all finished.
	Threads int
	storage Storage

	mut sync.Mutex
	// run is non-nil from Run setup until Run is about to return. Each Run
	// gets a fresh queueRun, so notifications from a previous run can never
	// affect a later one.
	run *queueRun

	// held keeps a request already removed from storage that could not be
	// dispatched because Stop closed the dispatch gate. The next Run sends
	// it out before touching storage again, so the item is neither lost nor
	// overtaken by requests added afterwards. It is only accessed by the
	// loop goroutine and by Run before the loop starts.
	held []byte
}

// queueRun holds the bookkeeping of a single Queue.Run invocation.
type queueRun struct {
	mu sync.Mutex
	// wake carries a "state changed" notification for the loop. It is
	// buffered with capacity one and always sent non-blocking, so neither
	// AddRequest nor worker completion can block, and a notification can
	// never be stranded with nobody receiving it.
	wake chan struct{}
	// stopped is set under mu by Stop. It is the authoritative dispatch
	// gate: a request whose slot is committed under mu before Stop takes
	// the lock counts as dispatched in time and runs to completion, every
	// later request is retained instead.
	stopped bool
	// stopCh is closed once by Stop purely to wake a loop parked in a
	// select; closing instead of sending means no notification can be left
	// unreceived.
	stopCh   chan struct{}
	stopOnce sync.Once
	// inflight counts requests handed to a worker whose processing has not
	// finished. With the fixed worker set it never exceeds Threads, because
	// a worker accepts the next request only after the previous one is
	// fully done.
	inflight int
}

// InMemoryQueueStorage is the default implementation of the Storage interface.
// InMemoryQueueStorage holds the request queue in memory.
type InMemoryQueueStorage struct {
	// MaxSize defines the capacity of the queue.
	// New requests are discarded if the queue size reaches MaxSize
	MaxSize int
	lock    *sync.RWMutex
	size    int
	first   *inMemoryQueueItem
	last    *inMemoryQueueItem
}

type inMemoryQueueItem struct {
	Request []byte
	Next    *inMemoryQueueItem
}

// New creates a new queue with a Storage specified in argument
// A standard InMemoryQueueStorage is used if Storage argument is nil.
func New(threads int, s Storage) (*Queue, error) {
	if s == nil {
		s = &InMemoryQueueStorage{MaxSize: 100000}
	}
	if err := s.Init(); err != nil {
		return nil, err
	}
	return &Queue{
		Threads: threads,
		storage: s,
	}, nil
}

// IsEmpty returns true if the queue is empty
func (q *Queue) IsEmpty() bool {
	s, _ := q.Size()
	return s == 0
}

// AddURL adds a new URL to the queue
func (q *Queue) AddURL(URL string) error {
	u, err := urlParser.Parse(URL)
	if err != nil {
		return err
	}
	u2, err := url.Parse(u.Href(false))
	if err != nil {
		return err
	}
	r := &colly.Request{
		URL:    u2,
		Method: "GET",
	}
	d, err := r.Marshal()
	if err != nil {
		return err
	}
	return q.storage.AddRequest(d)
}

// AddRequest adds a new Request to the queue. The request is persisted
// first; when a Run is active the loop is woken afterwards. The storage
// write therefore returns without waiting for the next network response,
// and links discovered inside a crawling callback are picked up by the
// currently running queue.
func (q *Queue) AddRequest(r *colly.Request) error {
	if err := q.storeRequest(r); err != nil {
		return err
	}
	q.mut.Lock()
	run := q.run
	q.mut.Unlock()
	if run != nil {
		run.signalWake()
	}
	return nil
}

func (q *Queue) storeRequest(r *colly.Request) error {
	d, err := r.Marshal()
	if err != nil {
		return err
	}
	return q.storage.AddRequest(d)
}

// Size returns the size of the queue
func (q *Queue) Size() (int, error) {
	return q.storage.QueueSize()
}

// Run starts consumer threads and calls the Collector to perform requests.
// Run blocks until the queue holds no pending request and every request it
// dispatched has finished, including its response body and user callbacks.
// An empty storage alone does not end the run: in-flight callbacks may
// still enqueue more links.
//
// Both synchronous and asynchronous Collectors are supported. The
// Collector is never reconfigured: its Async flag, callbacks and WaitGroup
// keep their original behavior. Run waits for the queue's own work instead
// of the Collector's WaitGroup, so external Visit calls sharing the same
// Collector keep blocking (or running detached in Async mode) as usual but
// neither slow the queue down nor release one of its slots. Crawl errors
// are delivered to the Collector's OnError callback exactly as for
// Visit/Request.Do; Run itself only returns storage errors.
// The given Storage must not be used directly while Run blocks.
func (q *Queue) Run(c *colly.Collector) error {
	q.mut.Lock()
	if q.run != nil {
		q.mut.Unlock()
		panic("cannot call duplicate Queue.Run")
	}
	run := &queueRun{
		wake:   make(chan struct{}, 1),
		stopCh: make(chan struct{}),
	}
	q.run = run
	q.mut.Unlock()
	defer func() {
		q.mut.Lock()
		q.run = nil
		q.mut.Unlock()
	}()

	requestc := make(chan *colly.Request)
	// sem carries one free worker slot. A token must be acquired (and the
	// stop gate re-checked under run.mu) before every handoff, which makes
	// Threads the exact concurrency bound and gives Stop a deterministic
	// linearization point.
	sem := make(chan struct{}, q.Threads)
	for i := 0; i < q.Threads; i++ {
		sem <- struct{}{}
	}
	var workers sync.WaitGroup
	for i := 0; i < q.Threads; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for req := range requestc {
				// RunScraped spans the whole request lifecycle even with
				// an asynchronous Collector: OnRequest, request and
				// response header callbacks, body download, parsing and
				// every user callback. Its error has already been routed
				// to OnError, so ignoring it keeps the historical error
				// contract while the slot is released only after
				// everything has settled.
				_ = c.RunScraped(req)
				run.workerFinished()
				sem <- struct{}{}
			}
		}()
	}

	loopErr := q.loop(c, requestc, sem, run)
	// No more requests will be dispatched; let in-flight workers finish
	// before Run returns.
	close(requestc)
	workers.Wait()
	return loopErr
}

// Stop will stop the running queue. Stop returns immediately and is safe
// to call from a queue callback. After Stop no new request is dispatched,
// but requests already in flight run to completion and Run waits for them.
// Links discovered while stopping are still stored. A request already
// taken from storage but not dispatched is retained and processed first by
// the next Run. When no Run is active, Stop is a no-op.
func (q *Queue) Stop() {
	q.mut.Lock()
	run := q.run
	q.mut.Unlock()
	if run == nil {
		return
	}
	run.mu.Lock()
	run.stopped = true
	run.mu.Unlock()
	run.stopOnce.Do(func() { close(run.stopCh) })
}

// loop drives dispatch on a single goroutine. All state changes funnel
// through run.wake, so every iteration re-reads the authoritative state
// (storage, stop flag, in-flight count) instead of trusting a wake reason.
func (q *Queue) loop(c *colly.Collector, requestc chan<- *colly.Request, sem chan struct{}, run *queueRun) error {
	for {
		buf := q.held
		q.held = nil
		if buf == nil {
			if run.isStopped() {
				return nil
			}
			size, err := q.storage.QueueSize()
			if err != nil {
				return err
			}
			if size > 0 {
				buf, err = q.storage.GetRequest()
				if err != nil {
					// Keep the historical behavior of ignoring
					// GetRequest errors instead of failing Run.
					continue
				}
			}
		}

		if buf == nil {
			run.mu.Lock()
			idle := run.inflight == 0
			run.mu.Unlock()
			if idle {
				return nil
			}
			select {
			case <-run.wake:
			case <-run.stopCh:
			}
			continue
		}

		req, err := q.parseRequest(c, buf)
		if err != nil {
			// An unparseable entry is dropped, matching the old loop.
			continue
		}

		// Wait for a free worker slot. If Stop fires while waiting, the
		// popped item goes back on held for the next Run.
		select {
		case <-sem:
		case <-run.stopCh:
			q.held = buf
			return nil
		}

		// Re-check the authoritative stop flag under the same mutex Stop
		// uses. Committing inflight here either happens-before Stop's lock
		// (the request is dispatched and runs to completion) or after it
		// (the request is retained), so the outcome never depends on which
		// ready channel a select would pick.
		run.mu.Lock()
		if run.stopped {
			q.held = buf
			run.mu.Unlock()
			sem <- struct{}{}
			return nil
		}
		run.inflight++
		run.mu.Unlock()

		// The worker is reserved and the request was committed, so this
		// handoff cannot race Stop anymore.
		requestc <- req
	}
}

func (q *Queue) parseRequest(c *colly.Collector, buf []byte) (*colly.Request, error) {
	// Third-party Storage implementations may return borrowed byte slices
	// (for example a buffer reused by the next GetRequest call), so always
	// unmarshal a private copy.
	copied := make([]byte, len(buf))
	copy(copied, buf)
	return c.UnmarshalRequest(copied)
}

func (r *queueRun) isStopped() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.stopped
}

// workerFinished releases the slot occupied since dispatch and wakes the
// loop so it can notice that no tracked request remains in flight.
func (r *queueRun) workerFinished() {
	r.mu.Lock()
	r.inflight--
	r.mu.Unlock()
	r.signalWake()
}

// signalWake is non-blocking. The buffered wake channel only needs to carry
// "something changed"; the loop always re-reads the authoritative state,
// and a full buffer means a wake-up is already pending. This also makes the
// send safe when the loop has exited, for example after Stop.
func (r *queueRun) signalWake() {
	select {
	case r.wake <- struct{}{}:
	default:
	}
}

// Init implements Storage.Init() function
func (q *InMemoryQueueStorage) Init() error {
	q.lock = &sync.RWMutex{}
	return nil
}

// AddRequest implements Storage.AddRequest() function
func (q *InMemoryQueueStorage) AddRequest(r []byte) error {
	q.lock.Lock()
	defer q.lock.Unlock()
	// Discard URLs if size limit exceeded
	if q.MaxSize > 0 && q.size >= q.MaxSize {
		return colly.ErrQueueFull
	}
	i := &inMemoryQueueItem{Request: r}
	if q.first == nil {
		q.first = i
	} else {
		q.last.Next = i
	}
	q.last = i
	q.size++
	return nil
}

// GetRequest implements Storage.GetRequest() function
func (q *InMemoryQueueStorage) GetRequest() ([]byte, error) {
	q.lock.Lock()
	defer q.lock.Unlock()
	if q.size == 0 {
		return nil, nil
	}
	r := q.first.Request
	q.first = q.first.Next
	q.size--
	return r, nil
}

// QueueSize implements Storage.QueueSize() function
func (q *InMemoryQueueStorage) QueueSize() (int, error) {
	q.lock.Lock()
	defer q.lock.Unlock()
	return q.size, nil
}
