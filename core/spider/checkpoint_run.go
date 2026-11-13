package spider

import (
	"context"
	"errors"
	"fmt"
	"os"
	"sync"
	"time"

	"github.com/hu17889/go_spider/core/common/com_interfaces"
	"github.com/hu17889/go_spider/core/common/mlog"
	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/page_processer"
	"github.com/hu17889/go_spider/core/pipeline"
	"github.com/hu17889/go_spider/core/scheduler"
)

// RunCheckpoint runs the spider against a durable crawl frontier persisted in
// dir. Unlike Run, a process killed (or returning an error) mid-crawl can be
// restarted with the same dir and resumes the remaining requests instead of
// traversing the entry pages again.
//
// On the first run the seeds previously added through AddRequest/AddUrl are
// admitted into the frontier. When dir already holds a snapshot, the queued
// and in-flight requests are restored first (in-flight items, in claim order,
// ahead of the queue) and any new seeds are admitted afterwards.
//
// Identity is the raw url string and is scoped to this crawl: queued,
// in-flight and completed urls all participate in de-duplication, and a url
// keeps the parameters of its first admission.
//
// Run and the custom Scheduler integration are unchanged. RunCheckpoint owns
// its frontier and does not support Redis, multiple processes sharing one
// directory, requests with a custom redirect callback, or requests whose
// Meta is non-nil; those requests are rejected with an error before they are
// persisted or downloaded.
//
// When ctx is cancelled no new request is claimed; pages already in flight
// finish and their state is saved, then ctx.Err() is returned. A custom
// Downloader is never interrupted.
func (this *Spider) RunCheckpoint(ctx context.Context, dir string) error {
	if dir == "" {
		return errors.New("checkpoint: checkpoint directory must not be empty")
	}
	if ctx == nil {
		return errors.New("checkpoint: context must not be nil")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("checkpoint: cannot create directory %s: %w", dir, err)
	}

	store, _, err := loadCheckpoint(dir)
	if err != nil {
		return err
	}
	// The directory drained completely in an earlier run: nothing to do, and
	// Finish must not be invoked a second time.
	if store.finished {
		mlog.StraceInst().Println("** checkpoint already finished, skip **")
		return nil
	}

	seeds, err := drainSeedScheduler(this.pScheduler)
	if err != nil {
		return err
	}
	if err := store.bootstrap(seeds); err != nil {
		return err
	}

	if this.threadnum == 0 {
		this.threadnum = 1
	}
	// slots is the in-memory concurrency limit. A slot is taken before a
	// request is claimed, so the durable in-flight set only ever contains
	// requests a worker has actually started handling.
	slots := make(chan struct{}, this.threadnum)

	runCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	var (
		wg          sync.WaitGroup
		errMu       sync.Mutex
		firstErr    error
		workerBroke bool
	)
	setErr := func(e error) {
		if e == nil {
			return
		}
		errMu.Lock()
		if firstErr == nil {
			firstErr = e
			workerBroke = true
		}
		errMu.Unlock()
		cancel()
	}

	dispatchDone := make(chan struct{})
	go func() {
		defer close(dispatchDone)
		for {
			if runCtx.Err() != nil {
				return
			}
			select {
			case slots <- struct{}{}:
			case <-runCtx.Done():
				return
			}
			// The slot send can win the select in the same instant the
			// context is cancelled: do not claim anything new in that case.
			if runCtx.Err() != nil {
				<-slots
				return
			}

			// The claim write lands before any download call.
			req, err := store.claim()
			if err != nil {
				<-slots
				setErr(err)
				return
			}
			if req == nil {
				<-slots
				// Frontier empty: stop once no worker is in flight (any
				// active worker could still enqueue more children).
				if len(slots) == 0 {
					return
				}
				select {
				case <-time.After(100 * time.Millisecond):
				case <-runCtx.Done():
					return
				}
				continue
			}

			wg.Add(1)
			go func(req *request.Request) {
				defer wg.Done()
				defer func() { <-slots }()
				mlog.StraceInst().Println("start checkpoint crawl : " + req.GetUrl())
				if err := this.processCheckpointPage(store, req); err != nil {
					setErr(err)
				}
			}(req)
		}
	}()

	wg.Wait()
	<-dispatchDone

	// A cancelled context always reports itself, even when workers also
	// surfaced errors while winding down.
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if workerBroke {
		return firstErr
	}
	if !store.empty() {
		// No error was recorded but work remains; refuse to report success.
		return errors.New("checkpoint: crawl stopped while the frontier was not empty")
	}

	// Record completion before invoking Finish so reopening this directory
	// never runs Finish twice, even if the process dies inside the callback.
	if err := store.markFinished(); err != nil {
		return err
	}
	mlog.StraceInst().Println("** executed checkpoint callback **")
	this.pPageProcesser.Finish()
	mlog.StraceInst().Println("** end checkpoint spider **")
	return nil
}

// drainSeedScheduler collects the seeds added through AddRequest/AddUrl.
// RunCheckpoint replaces the in-memory scheduler with the durable frontier,
// so only the default QueueScheduler is accepted as a seed source.
func drainSeedScheduler(s scheduler.Scheduler) ([]*request.Request, error) {
	qs, ok := s.(*scheduler.QueueScheduler)
	if !ok {
		return nil, errors.New("checkpoint: RunCheckpoint manages its own persistent frontier; do not replace the default Scheduler, add seeds with AddRequest/AddUrl")
	}
	var seeds []*request.Request
	for {
		req := qs.Poll()
		if req == nil {
			return seeds, nil
		}
		seeds = append(seeds, req)
	}
}

// processCheckpointPage handles one claimed request. The claim has already
// been persisted by the caller, so every exit path here is recoverable:
//   - download failures keep the existing three-attempt rule and then the
//     request is requeued (never marked done, never retried forever);
//   - processor or pipeline panics requeue the request and return an error;
//   - success runs Process and the pipelines, then commits parent completion
//     together with every discovered child request in one durable write.
func (this *Spider) processCheckpointPage(store *checkpointStore, req *request.Request) (retErr error) {
	var p *page.Page
	for i := 0; i < 3; i++ {
		this.sleep()
		p = this.pDownloader.Download(req)
		if p.IsSucc() {
			break
		}
	}
	if !p.IsSucc() {
		if err := store.requeue(req); err != nil {
			return err
		}
		return fmt.Errorf("checkpoint: download of %q failed after 3 attempts: %s", req.GetUrl(), p.Errormsg())
	}

	// Processing and output happen before the durable commit. If the process
	// dies before the commit lands, the whole page is replayed on restart;
	// pipeline outputs therefore need to be idempotent on the caller side.
	if err := safeProcess(this.pPageProcesser, p); err != nil {
		if rerr := store.requeue(req); rerr != nil {
			return rerr
		}
		return err
	}
	if err := safePipelines(this.pPiplelines, p, this); err != nil {
		if rerr := store.requeue(req); rerr != nil {
			return rerr
		}
		return err
	}
	if err := store.commit(req, p.GetTargetRequests()); err != nil {
		// The snapshot write failed: the old state stays on disk and the
		// request remains in flight, so a later resume retries this page.
		return err
	}
	return nil
}

func safeProcess(proc page_processer.PageProcesser, p *page.Page) (retErr error) {
	defer func() {
		if r := recover(); r != nil {
			retErr = fmt.Errorf("checkpoint: PageProcesser panicked on %q: %v", p.GetRequest().GetUrl(), r)
			mlog.LogInst().LogError(retErr.Error())
		}
	}()
	proc.Process(p)
	return nil
}

func safePipelines(pips []pipeline.Pipeline, p *page.Page, t com_interfaces.Task) (retErr error) {
	if p.GetSkip() {
		return nil
	}
	defer func() {
		if r := recover(); r != nil {
			retErr = fmt.Errorf("checkpoint: pipeline panicked on %q: %v", p.GetRequest().GetUrl(), r)
			mlog.LogInst().LogError(retErr.Error())
		}
	}()
	for _, pip := range pips {
		pip.Process(p.GetPageItems(), t)
	}
	return nil
}
