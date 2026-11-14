package sitemap

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/oxffaa/gopher-parse-sitemap/task_support/taskcheck"
)

func contractFixtureDocument() string {
	return `<?xml version="1.0" encoding="UTF-8"?>` + "\n" +
		`<s:urlset xmlns:s="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:aux="https://capture.invalid/extra" note="a&amp;b">` + "\n" +
		`  <!-- 注释 with multibyte ✨ -->` + "\n" +
		`  <s:url note="x&amp;y"><s:loc>https://capture.invalid/猫?x=1&amp;y=2</s:loc><s:lastmod>2026-01-02</s:lastmod><s:changefreq>daily</s:changefreq><s:priority>0.7</s:priority></s:url>` + "\n" +
		`  <s:url><s:loc><![CDATA[https://capture.invalid/b?q=一&x=2]]></s:loc><s:lastmod>2026-02-03T04:05:06+09:00</s:lastmod></s:url>` + "\n" +
		`  <aux:url><aux:loc>https://capture.invalid/extension</aux:loc></aux:url>` + "\n" +
		`  <s:url><s:loc>https://capture.invalid/final</s:loc><s:lastmod>2026-03-04</s:lastmod></s:url>` + "\n" +
		`</s:urlset>` + "\n"
}

func TestResumeByteCursor(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		document := contractFixtureDocument()
		server := newContractServer(t, []byte(document))
		checkpoint := filepath.Join(t.TempDir(), "checkpoint.json")

		// The transport buffers the whole document before handing any of it to
		// the parser, so the parser sees far more than the cursor while only
		// one entry is acknowledged. The saved position must still be the byte
		// boundary of the last acknowledged entry.
		server.prefetchSize = len(document)
		var deliveries []string
		stop := errors.New("stop on the second entry")
		firstErr := server.runResume(func(entry Entry) error {
			deliveries = append(deliveries, entry.GetLocation())
			if len(deliveries) == 2 {
				return stop
			}
			return nil
		}, checkpoint)

		c.Check(errors.Is(firstErr, stop), "consumer error must be returned unchanged, got %v", firstErr)
		c.Check(reflect.DeepEqual(deliveries, []string{
			"https://capture.invalid/猫?x=1&y=2",
			"https://capture.invalid/b?q=一&x=2",
		}), "first interrupted run delivered %v", deliveries)

		saved, ok := server.readCheckpoint(checkpoint)
		c.Check(ok, "checkpoint must exist after the acknowledged entries")
		firstClose := int64(strings.Index(document, "</s:url>") + len("</s:url>"))
		if ok {
			c.Check(saved.Offset == firstClose,
				"checkpoint offset %d must be the byte position %d after the acknowledged </s:url>", saved.Offset, firstClose)
			c.Check(saved.Offset > 1, "offset must count bytes, not entries")
			c.Check(!saved.Complete, "interrupted checkpoint must not be marked complete")
			c.Check(strings.HasPrefix(saved.RootTag, `<s:urlset`), "saved root context %q", saved.RootTag)
		}

		c.Check(server.maxSingleRead > int(saved.Offset),
			"transport handed the parser %d bytes in one read, ahead of the %d byte cursor", server.maxSingleRead, saved.Offset)
		c.Check(server.prefetched == len(document),
			"transport buffered %d bytes while the cursor was at %d of %d", server.prefetched, saved.Offset, len(document))
		server.reset()

		var resumed []string
		var resumedEntries []Entry
		resumeErr := server.runResume(func(entry Entry) error {
			resumed = append(resumed, entry.GetLocation())
			resumedEntries = append(resumedEntries, entry)
			return nil
		}, checkpoint)

		c.Check(resumeErr == nil, "resuming after an interruption must succeed, got %v", resumeErr)
		c.Check(reflect.DeepEqual(resumed, []string{
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/final",
		}), "resumed delivery sequence is %v", resumed)
		c.Check(len(server.ranges) == 1 && server.ranges[0] == fmt.Sprintf("bytes=%d-", saved.Offset),
			"resume Range header %v", server.ranges)
		c.Check(len(server.ifRanges) == 1 && server.ifRanges[0] == `"resume-v1"`,
			"resume If-Range header %v", server.ifRanges)
		c.Check(len(server.acceptEnc) == 1 && server.acceptEnc[0] == "identity",
			"resume Accept-Encoding %v", server.acceptEnc)
		c.Check(len(server.bodies) == 1 && !strings.Contains(server.bodies[0], "<s:urlset"),
			"resumed body must be the raw suffix, got %q", contractExcerpt(server.bodies))

		if len(resumedEntries) == 2 {
			second := resumedEntries[0]
			c.Check(second.GetLocation() == "https://capture.invalid/b?q=一&x=2", "entry two location %q", second.GetLocation())
			c.Check(string(second.GetChangeFrequency()) == "always", "entry two default frequency %q", second.GetChangeFrequency())
			c.Check(second.GetPriority() == 0.5, "entry two default priority %v", second.GetPriority())
			if modified := second.GetLastModified(); modified != nil {
				c.Check(modified.Year() == 2026 && modified.Month() == 2 && modified.Day() == 3,
					"entry two lastmod %v", modified)
			} else {
				c.Check(false, "entry two must carry a parsed lastmod")
			}
			final := resumedEntries[1]
			c.Check(final.GetLocation() == "https://capture.invalid/final", "entry three location %q", final.GetLocation())
			if modified := final.GetLastModified(); modified != nil {
				c.Check(modified.Year() == 2026 && modified.Month() == 3 && modified.Day() == 4, "entry three lastmod %v", modified)
			} else {
				c.Check(false, "entry three must carry a parsed lastmod")
			}
		} else {
			c.Check(false, "resume delivered %d entries", len(resumedEntries))
		}

		complete, ok := server.readCheckpoint(checkpoint)
		c.Check(ok && complete.Complete, "final checkpoint must be marked complete")
		if ok {
			c.Check(complete.Offset == int64(len(document)), "completed offset %d must equal document length %d",
				complete.Offset, len(document))
		}

		server.reset()
		counter := 0
		doneErr := server.runResume(func(Entry) error { counter++; return nil }, checkpoint)
		c.Check(doneErr == nil, "completed checkpoint must be a no-op, got %v", doneErr)
		c.Check(counter == 0, "completed checkpoint delivered %d entries", counter)
		c.Check(server.requests == 0, "completed checkpoint issued %d HTTP requests", server.requests)

		// Acknowledging the final entry and then cancelling leaves only the
		// closing root (and trailing whitespace) to read on the next call.
		tailCheckpoint := filepath.Join(t.TempDir(), "tail-only.json")
		tailServer := newContractServer(t, []byte(document))
		tailCtx, tailCancel := context.WithCancel(context.Background())
		tailSeen := 0
		tailPrimeErr := tailServer.runResumeContext(tailCtx, func(Entry) error {
			tailSeen++
			if tailSeen == 3 {
				tailCancel()
			}
			return nil
		}, tailCheckpoint)
		c.Check(errors.Is(tailPrimeErr, context.Canceled), "tail-only priming must stop on cancellation, got %v", tailPrimeErr)
		c.Check(tailSeen == 3, "tail-only priming delivered %d entries", tailSeen)
		tailState, tailOK := tailServer.readCheckpoint(tailCheckpoint)
		c.Check(tailOK && !tailState.Complete, "tail-only checkpoint must be incomplete")
		tailServer.reset()
		deliveredOnTail := 0
		tailErr := tailServer.runResume(func(Entry) error { deliveredOnTail++; return nil }, tailCheckpoint)
		c.Check(tailErr == nil, "closing-tag-only resume must succeed, got %v", tailErr)
		c.Check(deliveredOnTail == 0, "closing-tag-only resume delivered %d entries", deliveredOnTail)
		c.Check(len(tailServer.ranges) == 1 && strings.TrimSpace(tailServer.bodies[0]) == "</s:urlset>",
			"closing-tag-only suffix %q", contractExcerpt(tailServer.bodies))
		tailDone, tailDoneOK := tailServer.readCheckpoint(tailCheckpoint)
		c.Check(tailDoneOK && tailDone.Complete && tailDone.Offset == int64(len(document)),
			"closing-tag-only resume must complete the checkpoint: %v", tailDone)
		c.Check(allBodiesClosed(server) && allBodiesClosed(tailServer),
			"every response body must be closed")

		// A completed checkpoint refuses a different request URL without HTTP.
		server.reset()
		completedElsewhere := newContractServer(t, []byte(document))
		completedElsewhere.requestURL = "https://capture.invalid/other.xml"
		completedURLErr := completedElsewhere.runResume(func(Entry) error {
			c.Check(false, "completed checkpoint delivered an entry for another URL")
			return nil
		}, checkpoint)
		c.Check(errors.Is(completedURLErr, ErrResumeMismatch),
			"completed checkpoint for another URL must mismatch, got %v", completedURLErr)
		c.Check(completedElsewhere.requests == 0, "completed cross-URL call made %d requests", completedElsewhere.requests)

		// A root without a namespace, with unprefixed entries, is accepted.
		plainDocument := `<?xml version="1.0" encoding="UTF-8"?>` + "\n" +
			`<urlset>` + "\n" +
			`  <url><loc>https://capture.invalid/plain-1</loc></url>` + "\n" +
			`  <url><loc>https://capture.invalid/plain-2</loc></url>` + "\n" +
			`</urlset>` + "\n"
		plainCheckpoint := filepath.Join(t.TempDir(), "plain.json")
		plainServer := newContractServer(t, []byte(plainDocument))
		var plain []string
		plainErr := plainServer.runResume(func(entry Entry) error {
			plain = append(plain, entry.GetLocation())
			return nil
		}, plainCheckpoint)
		c.Check(plainErr == nil, "namespace-less urlset must parse, got %v", plainErr)
		c.Check(reflect.DeepEqual(plain, []string{
			"https://capture.invalid/plain-1",
			"https://capture.invalid/plain-2",
		}), "namespace-less delivery %v", plain)

		// An unknown root namespace is rejected outright.
		alienCheckpoint := filepath.Join(t.TempDir(), "alien.json")
		alienServer := newContractServer(t, []byte(
			`<urlset xmlns="https://capture.invalid/alien"><url><loc>x</loc></url></urlset>`))
		alienErr := alienServer.runResume(func(Entry) error { return nil }, alienCheckpoint)
		c.Check(alienErr != nil, "unknown root namespace must be rejected")
		_, alienStat := os.Stat(alienCheckpoint)
		c.Check(errors.Is(alienStat, os.ErrNotExist), "rejected root left a checkpoint")
	})
}

func TestResumeRepresentation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		document := contractFixtureDocument()
		checkpoint := filepath.Join(t.TempDir(), "checkpoint.json")

		server := newContractServer(t, []byte(document))
		stop := errors.New("stop")
		primed := 0
		if err := server.runResume(func(Entry) error {
			primed++
			if primed == 2 {
				return stop
			}
			return nil
		}, checkpoint); !errors.Is(err, stop) {
			c.Check(false, "priming interrupted run failed: %v", err)
		}
		saved, ok := server.readCheckpoint(checkpoint)
		c.Check(ok, "priming run must have published a checkpoint")
		if !ok {
			return
		}
		before, readErr := os.ReadFile(checkpoint)
		c.Check(readErr == nil, "reading checkpoint: %v", readErr)

		assertMismatch := func(name string, configure func()) {
			server.reset()
			configure()
			delivered := 0
			err := server.runResume(func(Entry) error { delivered++; return nil }, checkpoint)
			c.Check(errors.Is(err, ErrResumeMismatch), "%s must match ErrResumeMismatch, got %v", name, err)
			c.Check(delivered == 0, "%s delivered %d entries before failing", name, delivered)
			after, afterErr := os.ReadFile(checkpoint)
			c.Check(afterErr == nil && bytes.Equal(before, after), "%s changed the checkpoint", name)
			server.reset()
		}

		assertMismatch("ETag change", func() { server.etag = `"resume-v2"` })
		assertMismatch("Content-Range start shift", func() { server.rangeStartShift = 1 })
		assertMismatch("Content-Range total shift", func() { server.rangeTotalShift = 2 })
		assertMismatch("Content-Length shift", func() { server.contentLengthShift = 1 })
		assertMismatch("200 for range request", func() { server.answerRangeWithFull = true })
		assertMismatch("416 for range request", func() { server.answerRangeWith416 = true })
		assertMismatch("changed document address", func() { server.redirectDocument = true })
		assertMismatch("non-identity Content-Encoding", func() { server.contentEncodingValue = "gzip" })
		assertMismatch("weak range ETag", func() { server.etag = `W/"resume-v1"` })
		assertMismatch("missing range ETag", func() { server.etag = "" })

		c.Check(allBodiesClosed(server), "mismatch responses must close their bodies")

		c.Check(saved.Offset < saved.ContentLength, "priming checkpoint must be incomplete")

		// The checkpoint survived every mismatch and still resumes.
		server.reset()
		var got []string
		finalErr := server.runResume(func(entry Entry) error { got = append(got, entry.GetLocation()); return nil }, checkpoint)
		c.Check(finalErr == nil, "clean resume after mismatches failed: %v", finalErr)
		c.Check(reflect.DeepEqual(got, []string{
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/final",
		}), "clean resume after mismatches delivered %v", got)

		// A first run rejected for a weak ETag must leave nothing behind.
		fresh := filepath.Join(t.TempDir(), "fresh.json")
		failing := newContractServer(t, []byte(document))
		failing.etag = "W/\"weak\""
		freshErr := failing.runResume(func(Entry) error { return nil }, fresh)
		c.Check(freshErr != nil, "weak ETag response must fail")
		_, statErr := os.Stat(fresh)
		c.Check(errors.Is(statErr, os.ErrNotExist), "failed first run left a checkpoint: %v", statErr)

		// Missing response request identity falls back to the requested URL.
		fallbackPath := filepath.Join(t.TempDir(), "fallback.json")
		fallbackServer := newContractServer(t, []byte(document))
		fallbackServer.omitResponseRequest = true
		fallbackStop := errors.New("stop")
		fallbackSeen := 0
		if err := fallbackServer.runResume(func(Entry) error {
			fallbackSeen++
			if fallbackSeen == 2 {
				return fallbackStop
			}
			return nil
		}, fallbackPath); !errors.Is(err, fallbackStop) {
			c.Check(false, "fallback priming failed: %v", err)
		}
		fallbackState, fallbackOK := fallbackServer.readCheckpoint(fallbackPath)
		c.Check(fallbackOK && fallbackState.DocumentURL == fallbackServer.requestURL,
			"document URL fallback %q", fallbackState.DocumentURL)

		// Corrupt JSON, URL mismatch and illegal offsets all fail before HTTP.
		corruptServer := newContractServer(t, []byte(document))
		corrupt := filepath.Join(t.TempDir(), "corrupt.json")
		c.Check(os.WriteFile(corrupt, []byte("{not json"), 0o600) == nil, "writing corrupt checkpoint")
		corruptErr := corruptServer.runResume(func(Entry) error { return nil }, corrupt)
		c.Check(corruptErr != nil, "corrupt checkpoint must be rejected")
		c.Check(corruptServer.requests == 0, "corrupt checkpoint caused %d requests", corruptServer.requests)

		// A first response that is not 200, lacks a strong ETag, lacks a
		// positive Content-Length or is encoded must fail before delivery.
		for name, configure := range map[string]func(*contractServer){
			"non-200 initial response": func(s *contractServer) { s.initialStatus = http.StatusNotFound },
			"weak initial ETag":        func(s *contractServer) { s.etag = `W/"weak"` },
			"missing initial ETag":     func(s *contractServer) { s.etag = "" },
			"no Content-Length":        func(s *contractServer) { s.omitContentLength = true },
			"encoded initial response": func(s *contractServer) { s.contentEncodingValue = "gzip" },
			"transport failure":        func(s *contractServer) { s.transportError = errors.New("dial refused") },
		} {
			path := filepath.Join(t.TempDir(), "invalid.json")
			invalidServer := newContractServer(t, []byte(document))
			configure(invalidServer)
			delivered := 0
			invalidErr := invalidServer.runResume(func(Entry) error { delivered++; return nil }, path)
			c.Check(invalidErr != nil, "%s must fail, got %v", name, invalidErr)
			c.Check(delivered == 0, "%s delivered %d entries", name, delivered)
			_, invalidStat := os.Stat(path)
			c.Check(errors.Is(invalidStat, os.ErrNotExist), "%s left a checkpoint", name)
			c.Check(allBodiesClosed(invalidServer), "%s left a body open", name)
		}

		// A transport failure on the range request keeps its own cause. Use a
		// fresh incomplete checkpoint, since the one above just completed.
		rangeFailPath := filepath.Join(t.TempDir(), "range-fail.json")
		rangePrimeServer := newContractServer(t, []byte(document))
		rangePrimeSeen := 0
		rangePrimeStop := errors.New("stop for range failure")
		if err := rangePrimeServer.runResume(func(Entry) error {
			rangePrimeSeen++
			if rangePrimeSeen == 2 {
				return rangePrimeStop
			}
			return nil
		}, rangeFailPath); !errors.Is(err, rangePrimeStop) {
			c.Check(false, "range-failure priming failed: %v", err)
		}
		rangeBefore, rangeBeforeErr := os.ReadFile(rangeFailPath)
		c.Check(rangeBeforeErr == nil, "reading range-failure checkpoint: %v", rangeBeforeErr)
		rangeFailServer := newContractServer(t, []byte(document))
		rangeFailServer.transportError = contractTransportError
		rangeDelivered := 0
		rangeFailErr := rangeFailServer.runResume(func(Entry) error {
			rangeDelivered++
			return nil
		}, rangeFailPath)
		c.Check(errors.Is(rangeFailErr, contractTransportError), "range transport failure cause lost: %v", rangeFailErr)
		c.Check(rangeDelivered == 0, "range transport failure delivered %d entries", rangeDelivered)
		rangeFailAfter, rangeFailOK := os.ReadFile(rangeFailPath)
		c.Check(rangeFailOK == nil && bytes.Equal(rangeBefore, rangeFailAfter), "transport failure changed the checkpoint")

		otherPath := filepath.Join(t.TempDir(), "other.json")
		otherServer := newContractServer(t, []byte(document))
		otherServer.requestURL = "https://capture.invalid/other.xml"
		otherSeen := 0
		otherStop := errors.New("stop")
		if err := otherServer.runResume(func(Entry) error {
			otherSeen++
			if otherSeen == 2 {
				return otherStop
			}
			return nil
		}, otherPath); !errors.Is(err, otherStop) {
			c.Check(false, "priming a checkpoint for another URL must not fail")
		}
		otherState, otherOK := otherServer.readCheckpoint(otherPath)
		c.Check(otherOK, "priming other-URL checkpoint")
		if otherOK {
			otherState.Offset = otherState.ContentLength + 1
			badBytes, _ := json.Marshal(otherState)
			badOffset := filepath.Join(t.TempDir(), "bad-offset.json")
			c.Check(os.WriteFile(badOffset, badBytes, 0o600) == nil, "writing illegal-offset checkpoint")
			badOffsetServer := newContractServer(t, []byte(document))
			badOffsetErr := badOffsetServer.runResume(func(Entry) error { return nil }, badOffset)
			c.Check(badOffsetErr != nil, "illegal offset must be rejected")
			c.Check(badOffsetServer.requests == 0, "illegal offset caused %d requests", badOffsetServer.requests)

			// Same checkpoint content, different request URL: no HTTP either.
			wrongURLServer := newContractServer(t, []byte(document))
			wrongURLErr := wrongURLServer.runResume(func(Entry) error { return nil }, otherPath)
			c.Check(errors.Is(wrongURLErr, ErrResumeMismatch),
				"URL mismatch must match ErrResumeMismatch, got %v", wrongURLErr)
			c.Check(wrongURLServer.requests == 0, "URL mismatch caused %d requests", wrongURLServer.requests)
		}
	})
}

func TestResumeAcknowledgement(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		document := contractFixtureDocument()
		checkpoint := filepath.Join(t.TempDir(), "checkpoint.json")
		server := newContractServer(t, []byte(document))

		rejection := errors.New("consumer rejected entry")
		seen := 0
		rejectErr := server.runResume(func(Entry) error {
			seen++
			if seen == 2 {
				return rejection
			}
			return nil
		}, checkpoint)
		c.Check(errors.Is(rejectErr, rejection), "consumer rejection must be returned, got %v", rejectErr)
		c.Check(seen == 2, "consumer saw %d entries", seen)

		afterRejection, ok := server.readCheckpoint(checkpoint)
		firstClose := int64(strings.Index(document, "</s:url>") + len("</s:url>"))
		c.Check(ok, "checkpoint after consumer rejection")
		if ok {
			c.Check(afterRejection.Offset == firstClose,
				"rejected entry must not be committed, offset %d want %d", afterRejection.Offset, firstClose)
			c.Check(!afterRejection.Complete, "rejected run must not be complete")
		}

		// Mutating a delivered entry and its date must not move the cursor.
		server.reset()
		var secondRun []string
		mutatingErr := server.runResume(func(entry Entry) error {
			secondRun = append(secondRun, entry.GetLocation())
			if parsed, same := entry.(*sitemapEntry); same {
				parsed.Location = "https://mutated.invalid/"
				parsed.LastModified = "2000-01-01"
				parsed.ParsedLastModified = nil
			}
			return nil
		}, checkpoint)
		c.Check(mutatingErr == nil, "mutating consumer run failed: %v", mutatingErr)
		c.Check(reflect.DeepEqual(secondRun, []string{
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/final",
		}), "mutating consumer delivery sequence %v", secondRun)

		finalState, ok := server.readCheckpoint(checkpoint)
		c.Check(ok && finalState.Complete, "mutating run must complete the checkpoint")

		// A damaged tail keeps the last acknowledged position.
		tailCheckpoint := filepath.Join(t.TempDir(), "tail.json")
		tailServer := newContractServer(t, []byte(document))
		tailStop := errors.New("stop before tail")
		seen = 0
		if err := tailServer.runResume(func(Entry) error {
			seen++
			if seen == 2 {
				return tailStop
			}
			return nil
		}, tailCheckpoint); !errors.Is(err, tailStop) {
			c.Check(false, "tail priming run failed: %v", err)
		}
		_, committedOK := tailServer.readCheckpoint(tailCheckpoint)
		c.Check(committedOK, "tail checkpoint exists")

		tailServer.reset()
		tailServer.corruptSuffixEntity = true
		var tailEntries []string
		tailErr := tailServer.runResume(func(entry Entry) error {
			tailEntries = append(tailEntries, entry.GetLocation())
			return nil
		}, tailCheckpoint)
		c.Check(tailErr != nil, "damaged tail must fail")
		c.Check(!errors.Is(tailErr, ErrResumeMismatch), "damaged tail is a read failure, got %v", tailErr)
		afterTail, afterTailOK := tailServer.readCheckpoint(tailCheckpoint)
		lastClose := int64(strings.LastIndex(document, "</s:url>") + len("</s:url>"))
		c.Check(afterTailOK && afterTail.Offset == lastClose,
			"damaged tail must keep the last acknowledged entry at %d, got %v", lastClose, afterTail)
		c.Check(afterTailOK && !afterTail.Complete, "damaged tail must not complete the checkpoint")
		c.Check(reflect.DeepEqual(tailEntries, []string{
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/final",
		}), "entries before the damaged tail %v", tailEntries)

		// A real read failure inside the resumed suffix keeps its own identity
		// for errors.Is and still does not advance the checkpoint.
		readFailCheckpoint := filepath.Join(t.TempDir(), "read-fail.json")
		readFailServer := newContractServer(t, []byte(document))
		readFailStop := errors.New("stop before read failure")
		seen = 0
		if err := readFailServer.runResume(func(Entry) error {
			seen++
			if seen == 2 {
				return readFailStop
			}
			return nil
		}, readFailCheckpoint); !errors.Is(err, readFailStop) {
			c.Check(false, "read-failure priming run failed: %v", err)
		}
		_, readFailOK := readFailServer.readCheckpoint(readFailCheckpoint)
		c.Check(readFailOK, "read-failure checkpoint exists")
		readFailServer.reset()
		readFailServer.readError = contractReadError
		readFailServer.prefetchSize = len(document)
		readFailSeen := 0
		readFailErr := readFailServer.runResume(func(Entry) error { readFailSeen++; return nil }, readFailCheckpoint)
		c.Check(errors.Is(readFailErr, contractReadError), "read failure must keep its cause, got %v", readFailErr)
		afterReadFail, aftOK := readFailServer.readCheckpoint(readFailCheckpoint)
		lastAcknowledged := int64(strings.LastIndex(document, "</s:url>") + len("</s:url>"))
		c.Check(readFailOK && aftOK && !afterReadFail.Complete && afterReadFail.Offset == lastAcknowledged,
			"read failure must keep the last acknowledged offset %d, got %v", lastAcknowledged, afterReadFail)
		c.Check(readFailSeen == 2, "entries acknowledged before the read failure: %d", readFailSeen)

		// Cancellation stops both reading and further delivery.
		cancelCheckpoint := filepath.Join(t.TempDir(), "cancel.json")
		cancelServer := newContractServer(t, []byte(document))
		ctx, cancel := context.WithCancel(context.Background())
		canceled := 0
		cancelErr := cancelServer.runResumeContext(ctx, func(Entry) error {
			canceled++
			cancel()
			return nil
		}, cancelCheckpoint)
		c.Check(errors.Is(cancelErr, context.Canceled), "cancellation must surface context.Canceled, got %v", cancelErr)
		c.Check(canceled == 1, "cancellation delivered %d entries", canceled)

		// Two consecutive interruptions resume cleanly, then complete.
		multiCheckpoint := filepath.Join(t.TempDir(), "multi.json")
		multiServer := newContractServer(t, []byte(document))
		var sequence []string
		firstStop := errors.New("first stop")
		count := 0
		if err := multiServer.runResume(func(entry Entry) error {
			sequence = append(sequence, entry.GetLocation())
			count++
			if count == 2 {
				return firstStop
			}
			return nil
		}, multiCheckpoint); !errors.Is(err, firstStop) {
			c.Check(false, "first multi-run failed: %v", err)
		}
		secondStop := errors.New("second stop")
		if err := multiServer.runResume(func(entry Entry) error {
			sequence = append(sequence, entry.GetLocation())
			if entry.GetLocation() == "https://capture.invalid/final" {
				return secondStop
			}
			return nil
		}, multiCheckpoint); !errors.Is(err, secondStop) {
			c.Check(false, "second multi-run failed: %v", err)
		}
		finalErr := multiServer.runResume(func(entry Entry) error {
			sequence = append(sequence, entry.GetLocation())
			return nil
		}, multiCheckpoint)
		c.Check(finalErr == nil, "third multi-run failed: %v", finalErr)
		c.Check(reflect.DeepEqual(sequence, []string{
			"https://capture.invalid/猫?x=1&y=2",
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/b?q=一&x=2",
			"https://capture.invalid/final",
			"https://capture.invalid/final",
		}), "consecutive interruption sequence %v", sequence)
		c.Check(multiServer.requests == 3, "consecutive interruption made %d requests", multiServer.requests)

		// Checkpoint write failure stops delivery and removes the temp file.
		// A read-only destination directory lets the first entry be delivered
		// but makes the atomic checkpoint publication fail.
		failingDir := t.TempDir()
		c.Check(os.Chmod(failingDir, 0o500) == nil, "making checkpoint directory read-only")
		defer func() { _ = os.Chmod(failingDir, 0o700) }()
		blocked := filepath.Join(failingDir, "checkpoint.json")
		writeFailServer := newContractServer(t, []byte(document))
		writeFailSeen := 0
		writeFailErr := writeFailServer.runResume(func(Entry) error { writeFailSeen++; return nil }, blocked)
		c.Check(writeFailErr != nil, "checkpoint write failure must stop the run, got %v", writeFailErr)
		c.Check(writeFailSeen == 1, "write failure delivered %d entries", writeFailSeen)
		matches, _ := filepath.Glob(filepath.Join(failingDir, ".checkpoint.json.tmp-*"))
		c.Check(len(matches) == 0, "failed write left temporary files %v", matches)
		_, checkpointStat := os.Stat(blocked)
		c.Check(errors.Is(checkpointStat, os.ErrNotExist), "failed write published a checkpoint")
	})
}

func contractExcerpt(bodies []string) string {
	if len(bodies) == 0 {
		return "<none>"
	}
	return bodies[0]
}

func allBodiesClosed(server *contractServer) bool {
	for _, closed := range server.closedResponses {
		if !closed {
			return false
		}
	}
	return true
}

type contractServer struct {
	t          *testing.T
	document   []byte
	requestURL string
	etag       string

	requests      int
	ranges        []string
	ifRanges      []string
	bodies        []string
	acceptEnc     []string
	maxSingleRead int
	served        int
	prefetched    int

	answerRangeWithFull  bool
	answerRangeWith416   bool
	redirectDocument     bool
	rangeStartShift      int
	rangeTotalShift      int
	contentLengthShift   int
	truncateSuffix       int
	contentEncodingValue string
	omitResponseRequest  bool
	// corruptSuffixEntity rewrites the closing root tag in a requested suffix
	// so the resumed parse fails right after the last entry without changing
	// the declared length.
	corruptSuffixEntity bool
	// prefetchSize is how much of the body each Read pulls into its own
	// buffer before returning any of it to the parser.
	prefetchSize int
	// readError, when set, is returned by the ranged body once its buffered
	// data has been handed out.
	readError error
	// transportError, when set, is returned instead of any response.
	transportError error
	// omitContentLength serves the initial body without a Content-Length.
	omitContentLength bool
	// initialStatus overrides the initial response status.
	initialStatus int
	// closedResponses records, per request, whether its body was closed.
	closedResponses []bool
}

var contractTransportError = errors.New("synthetic transport failure")

var contractReadError = errors.New("synthetic network read failure")

func newContractServer(t *testing.T, document []byte) *contractServer {
	t.Helper()
	return &contractServer{
		t:          t,
		document:   document,
		requestURL: "https://capture.invalid/sitemap.xml",
		etag:       `"resume-v1"`,
	}
}

func (s *contractServer) reset() {
	s.requests = 0
	s.ranges = nil
	s.ifRanges = nil
	s.bodies = nil
	s.acceptEnc = nil
	s.maxSingleRead = 0
	s.served = 0
	s.prefetched = 0
	s.etag = `"resume-v1"`
	s.answerRangeWithFull = false
	s.answerRangeWith416 = false
	s.redirectDocument = false
	s.rangeStartShift = 0
	s.rangeTotalShift = 0
	s.contentLengthShift = 0
	s.truncateSuffix = 0
	s.contentEncodingValue = ""
	s.corruptSuffixEntity = false
	s.prefetchSize = 0
	s.readError = nil
	s.transportError = nil
	s.omitContentLength = false
	s.initialStatus = 0
	s.closedResponses = nil
}

func (s *contractServer) runResume(consumer EntryConsumer, checkpoint string) error {
	return s.runResumeContext(context.Background(), consumer, checkpoint)
}

func (s *contractServer) runResumeContext(ctx context.Context, consumer EntryConsumer, checkpoint string) error {
	client := &http.Client{Transport: s}
	return ResumeFromSite(ctx, client, s.requestURL, checkpoint, consumer)
}

func (s *contractServer) readCheckpoint(path string) (*resumeCheckpoint, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, false
	}
	var checkpoint resumeCheckpoint
	if err := json.Unmarshal(data, &checkpoint); err != nil {
		return nil, false
	}
	return &checkpoint, true
}

func (s *contractServer) RoundTrip(request *http.Request) (*http.Response, error) {
	s.requests++
	s.acceptEnc = append(s.acceptEnc, request.Header.Get("Accept-Encoding"))
	rangeHeader := request.Header.Get("Range")
	s.ranges = append(s.ranges, rangeHeader)
	s.ifRanges = append(s.ifRanges, request.Header.Get("If-Range"))
	if s.transportError != nil {
		return nil, s.transportError
	}

	document := s.document
	etag := s.etag

	header := http.Header{}
	header.Set("Content-Type", "application/xml")

	var body []byte
	status := http.StatusOK
	if rangeHeader == "" {
		if s.initialStatus != 0 {
			status = s.initialStatus
		}
		body = document
		if etag != "" {
			header.Set("Etag", etag)
		}
		if s.omitContentLength {
			header.Set("Transfer-Encoding", "chunked")
		} else {
			header.Set("Content-Length", strconv.Itoa(len(document)+s.contentLengthShift))
		}
		if s.contentEncodingValue != "" {
			header.Set("Content-Encoding", s.contentEncodingValue)
		}
	} else {
		offset, ok := parseContractRange(rangeHeader)
		if !ok {
			s.t.Fatalf("unexpected Range header %q", rangeHeader)
		}
		switch {
		case s.answerRangeWith416:
			status = http.StatusRequestedRangeNotSatisfiable
			header.Set("Etag", etag)
			header.Set("Content-Range", fmt.Sprintf("bytes */%d", len(document)))
			header.Set("Content-Length", "0")
		case s.answerRangeWithFull:
			status = http.StatusOK
			body = document
			header.Set("Etag", etag)
			header.Set("Content-Length", strconv.Itoa(len(document)))
		default:
			status = http.StatusPartialContent
			body = document[offset:]
			if s.truncateSuffix > 0 && s.truncateSuffix < len(body) {
				body = body[:len(body)-s.truncateSuffix]
			}
			if s.corruptSuffixEntity {
				body = bytes.Replace(body, []byte("</s:urlset>"), []byte("</s:urlseX>"), 1)
			}
			start := offset + s.rangeStartShift
			total := len(document) + s.rangeTotalShift
			header.Set("Etag", etag)
			header.Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, total-1, total))
			header.Set("Content-Length", strconv.Itoa(len(body)+s.contentLengthShift))
			if s.contentEncodingValue != "" {
				header.Set("Content-Encoding", s.contentEncodingValue)
			}
		}
	}

	s.bodies = append(s.bodies, string(body))
	s.closedResponses = append(s.closedResponses, false)
	response := &http.Response{
		StatusCode: status,
		Status:     fmt.Sprintf("%d %s", status, http.StatusText(status)),
		Header:     header,
		Body:       &contractBody{data: body, server: s, index: len(s.closedResponses) - 1},
	}
	if !s.omitResponseRequest {
		response.Request = request.Clone(request.Context())
		if s.redirectDocument {
			shifted := *request.URL
			shifted.Path = "/moved.xml"
			response.Request.URL = &shifted
		}
	}
	return response, nil
}

func parseContractRange(value string) (int, bool) {
	if !strings.HasPrefix(value, "bytes=") || !strings.HasSuffix(value, "-") {
		return 0, false
	}
	offset, err := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(value, "bytes="), "-"))
	if err != nil || offset < 0 {
		return 0, false
	}
	return offset, true
}

// contractBody deliberately pulls a large block of the document into its
// own buffer before handing any of it to the parser, and then returns only what
// the caller's buffer accepts. The parser therefore reads much further ahead
// than it has consumed. A checkpoint position taken from the underlying
// reader's delivery count would be wrong; positions must come from the XML
// parser's token boundaries.
type contractBody struct {
	data       []byte
	pending    []byte
	prefetched int
	eof        bool
	server     *contractServer
	index      int
}

func (body *contractBody) Close() error {
	body.server.closedResponses[body.index] = true
	return nil
}

func (body *contractBody) Read(buffer []byte) (int, error) {
	if len(body.pending) == 0 && !body.eof {
		chunk := body.server.prefetchSize
		if chunk <= 0 || chunk > len(body.data) {
			chunk = len(body.data)
		}
		body.pending = body.data[:chunk]
		body.data = body.data[chunk:]
		body.prefetched += chunk
		body.server.prefetched = body.prefetched
		if len(body.data) == 0 {
			body.eof = true
		}
	}
	if len(body.pending) == 0 {
		if body.server.readError != nil {
			return 0, body.server.readError
		}
		return 0, io.EOF
	}
	count := copy(buffer, body.pending)
	body.pending = body.pending[count:]
	body.server.served += count
	if count > body.server.maxSingleRead {
		body.server.maxSingleRead = count
	}
	// Report EOF together with the final bytes only once nothing is buffered,
	// unless a read error should surface instead.
	if len(body.pending) == 0 && body.eof {
		if body.server.readError != nil {
			return count, body.server.readError
		}
		return count, io.EOF
	}
	return count, nil
}

var _ io.ReadCloser = (*contractBody)(nil)
