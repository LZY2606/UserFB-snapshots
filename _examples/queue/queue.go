package main

import (
	"fmt"

	"github.com/gocolly/colly/v2"
	"github.com/gocolly/colly/v2/queue"
)

func main() {
	url := "https://httpbin.org/delay/1"

	// An asynchronous collector works with the queue exactly like a
	// synchronous one. Run blocks until every queued request and its
	// callbacks have finished, so there is no need to call c.Wait() for
	// requests submitted through the queue. The Async flag and the
	// collector's WaitGroup are left untouched.
	c := colly.NewCollector(
		colly.Async(true),
		colly.AllowURLRevisit(),
	)

	// Two consumer threads: at most two requests dispatched by this queue
	// are processed at the same time. A slot is held from dispatch until
	// the response body and all callbacks of that request have completed.
	q, _ := queue.New(
		2,
		&queue.InMemoryQueueStorage{MaxSize: 10000},
	)

	var processed int

	c.OnRequest(func(r *colly.Request) {
		fmt.Println("visiting", r.URL)
		// Links discovered while crawling can be added straight back to
		// the running queue. AddRequest stores first and returns
		// immediately; it never waits for the next network response.
		if r.ID < 15 {
			r2, err := r.New("GET", fmt.Sprintf("%s?x=%v", url, r.ID), nil)
			if err == nil {
				q.AddRequest(r2)
			}
		}
	})

	c.OnResponse(func(r *colly.Response) {
		processed++
		fmt.Println("done", r.Request.URL, "status", r.StatusCode)
		if processed >= 5 {
			// Stop returns immediately, even from inside a callback. No
			// new request is dispatched afterwards, but the request in
			// flight runs to completion and Run waits for it. Requests
			// already stored are kept and processed by the next Run.
			fmt.Println("stopping after", processed, "responses")
			q.Stop()
		}
	})

	c.OnError(func(r *colly.Response, err error) {
		// Network and HTTP errors keep being delivered through OnError,
		// exactly as without the queue.
		fmt.Println("error", r.Request.URL, err)
	})

	for i := 0; i < 5; i++ {
		q.AddURL(fmt.Sprintf("%s?n=%d", url, i))
	}

	// Run returns once the queue is drained and the queue's own callbacks
	// have all exited. Errors from crawling go to OnError above; Run
	// itself only reports storage errors.
	if err := q.Run(c); err != nil {
		panic(err)
	}
}
