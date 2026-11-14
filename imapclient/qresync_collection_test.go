package imapclient_test

import (
	"bufio"
	"io"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emersion/go-imap/v2"
	"github.com/emersion/go-imap/v2/imapclient"
	"github.com/emersion/go-imap/v2/task_support/taskcheck"
)

const qresyncGreeting = "* PREAUTH [CAPABILITY IMAP4rev1 ENABLE CONDSTORE QRESYNC] scripted fixture\r\n"

// scriptServer is a minimal scripted IMAP server over net.Pipe. A background
// goroutine reads request lines into a channel; the test drives responses on
// its own goroutine, which is required because net.Pipe writes block until
// the peer reads.
type scriptServer struct {
	t        *testing.T
	conn     net.Conn
	reqCh    chan string
	done     chan struct{}
	mu       sync.Mutex
	requests []string
}

func (s *scriptServer) readLoop() {
	defer close(s.done)
	r := bufio.NewReader(s.conn)
	for {
		_ = s.conn.SetReadDeadline(time.Now().Add(20 * time.Second))
		line, err := r.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimSuffix(line, "\r\n")
		s.mu.Lock()
		s.requests = append(s.requests, line)
		s.mu.Unlock()
		s.reqCh <- line
	}
}

func (s *scriptServer) readRequest() (tag, rest string) {
	var line string
	select {
	case got, ok := <-s.reqCh:
		if !ok {
			s.t.Fatalf("script server: request channel closed")
		}
		line = got
	case <-time.After(20 * time.Second):
		s.t.Fatalf("script server: timed out waiting for a request")
	}
	parts := strings.SplitN(line, " ", 2)
	tag = parts[0]
	if len(parts) == 2 {
		rest = parts[1]
	}
	return tag, rest
}

func (s *scriptServer) send(data string) {
	_ = s.conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
	if _, err := io.WriteString(s.conn, data); err != nil {
		s.t.Fatalf("script server: write: %v", err)
	}
}

func (s *scriptServer) snapshotRequests() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]string, len(s.requests))
	copy(out, s.requests)
	return out
}

func newScriptedClient(t *testing.T, opts *imapclient.Options) (*imapclient.Client, *scriptServer) {
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		clientConn.Close()
		serverConn.Close()
	})
	srv := &scriptServer{
		t:     t,
		conn:  serverConn,
		reqCh: make(chan string, 16),
		done:  make(chan struct{}),
	}
	client := imapclient.New(clientConn, opts)
	// The client read goroutine is running now, so the greeting write can
	// complete (net.Pipe writes block until read).
	srv.send(qresyncGreeting)
	go srv.readLoop()
	return client, srv
}

// enableQResync performs the ENABLE QRESYNC handshake. When confirmed is
// false, the server does not advertise QRESYNC in its ENABLED response.
func enableQResync(t *testing.T, c *imapclient.Client, srv *scriptServer, confirmed bool) {
	enableCmd := c.Enable(imap.CapQResync)
	tag, rest := srv.readRequest()
	if rest != "ENABLE QRESYNC" {
		t.Fatalf("ENABLE request = %q, want %q", rest, "ENABLE QRESYNC")
	}
	if confirmed {
		srv.send("* ENABLED QRESYNC\r\n")
	} else {
		srv.send("* ENABLED\r\n")
	}
	srv.send(tag + " OK enable done\r\n")
	if _, err := enableCmd.Wait(); err != nil {
		t.Fatalf("Enable().Wait() = %v", err)
	}
}

func flagsString(flags []imap.Flag) string {
	parts := make([]string, len(flags))
	for i, f := range flags {
		parts[i] = string(f)
	}
	return strings.Join(parts, " ")
}

// TestQResyncNegotiation covers the ENABLE/SELECT negotiation: wire encoding
// of the QRESYNC parameter, the EXAMINE variant, rejection of requests that
// cannot be sent, and the single in-progress SELECT guard.
func TestQResyncNegotiation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// ENABLE sent but the server does not confirm QRESYNC: a QRESYNC
		// selection must fail locally without writing anything.
		client, srv := newScriptedClient(t, nil)
		if err := client.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client, srv, false)
		opts := &imap.SelectOptions{QResync: &imap.QResyncOptions{
			UIDValidity: 11,
			ModSeq:      100,
		}}
		cmd := client.Select("INBOX", opts)
		_, err := cmd.WaitQResync()
		c.Check(err != nil, "QRESYNC select without ENABLED confirmation must fail, got nil")
		requests := srv.snapshotRequests()
		c.Check(len(requests) == 1 && strings.Contains(requests[0], "ENABLE QRESYNC"),
			"unconfirmed QRESYNC must not write a SELECT, requests = %v", requests)
		c.Check(!strings.Contains(strings.Join(requests, "|"), "SELECT"),
			"unconfirmed QRESYNC must not write a SELECT, requests = %v", requests)

		// Parameter validation happens before any byte is written.
		client2, srv2 := newScriptedClient(t, nil)
		if err := client2.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		badCases := []*imap.QResyncOptions{
			{UIDValidity: 0, ModSeq: 100},
			{UIDValidity: 11, ModSeq: 0},
			{UIDValidity: 11, ModSeq: 100, KnownUIDs: func() imap.UIDSet {
				s := imap.UIDSet{}
				s.AddNum(0) // 0 represents "*"
				return s
			}()},
			{UIDValidity: 11, ModSeq: 100, KnownUIDs: imap.SearchRes()}, // "$"
		}
		for i, bad := range badCases {
			_, err := client2.Select("INBOX", &imap.SelectOptions{QResync: bad}).Wait()
			c.Check(err != nil, "validation case %d must fail before writing", i)
		}
		c.Check(len(srv2.snapshotRequests()) == 0,
			"invalid QRESYNC parameters must not be sent, requests = %v", srv2.snapshotRequests())

		// Confirmed ENABLE: the QRESYNC parameter is encoded into the
		// select option list, including a compact known-UID set.
		client3, srv3 := newScriptedClient(t, nil)
		if err := client3.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client3, srv3, true)
		knownUIDs := imap.UIDSetNum(7, 8, 9) // encoded compactly as 7:9
		selectCmd := client3.Select("INBOX", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 11, ModSeq: 100, KnownUIDs: knownUIDs},
		})
		tag, rest := srv3.readRequest()
		c.Check(rest == "SELECT INBOX (QRESYNC 11 100 7:9)",
			"QRESYNC select wire encoding = %q", rest)
		srv3.send("* 3 EXISTS\r\n* OK [UIDVALIDITY 11] uids valid\r\n" +
			"* OK [HIGHESTMODSEQ 100] high\r\n")
		srv3.send(tag + " OK [READ-WRITE] selected\r\n")
		data, err := selectCmd.WaitQResync()
		c.Check(err == nil, "confirmed QRESYNC select failed: %v", err)
		c.Check(data != nil && data.Select != nil && data.Select.NumMessages == 3,
			"QRESYNC select metadata = %+v", data)
		c.Check(data != nil && len(data.Changes) == 0,
			"select without deltas should have no changes, got %d", dataChangesLen(data))

		// An empty KnownUIDs set omits the fourth parameter.
		emptyCmd := client3.Select("INBOX", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 11, ModSeq: 100},
		})
		tag, rest = srv3.readRequest()
		c.Check(rest == "SELECT INBOX (QRESYNC 11 100)",
			"QRESYNC without known UIDs wire encoding = %q", rest)
		srv3.send("* OK [CLOSED] closed\r\n* 3 EXISTS\r\n")
		srv3.send(tag + " OK selected\r\n")
		if _, err := emptyCmd.WaitQResync(); err != nil {
			t.Fatalf("empty KnownUIDs select failed: %v", err)
		}

		// CONDSTORE and QRESYNC share a single parenthesized list.
		bothCmd := client3.Select("INBOX", &imap.SelectOptions{
			CondStore: true,
			QResync:   &imap.QResyncOptions{UIDValidity: 11, ModSeq: 100},
		})
		tag, rest = srv3.readRequest()
		c.Check(rest == "SELECT INBOX (CONDSTORE QRESYNC 11 100)",
			"CONDSTORE+QRESYNC wire encoding = %q", rest)
		c.Check(strings.Count(rest, "(") == 1 && strings.Count(rest, ")") == 1,
			"CONDSTORE+QRESYNC must use a single option list, got %q", rest)
		srv3.send("* OK [CLOSED] closed\r\n")
		srv3.send(tag + " OK selected\r\n")
		if _, err := bothCmd.WaitQResync(); err != nil {
			t.Fatalf("CONDSTORE+QRESYNC select failed: %v", err)
		}

		// ReadOnly still sends EXAMINE, with the QRESYNC parameter.
		client4, srv4 := newScriptedClient(t, nil)
		if err := client4.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client4, srv4, true)
		examineCmd := client4.Select("INBOX", &imap.SelectOptions{
			ReadOnly: true,
			QResync:  &imap.QResyncOptions{UIDValidity: 11, ModSeq: 100},
		})
		tag, rest = srv4.readRequest()
		c.Check(rest == "EXAMINE INBOX (QRESYNC 11 100)",
			"read-only QRESYNC wire encoding = %q", rest)
		srv4.send("* 2 EXISTS\r\n" + tag + " OK [READ-ONLY] examined\r\n")
		examineData, err := examineCmd.WaitQResync()
		c.Check(err == nil, "EXAMINE QRESYNC failed: %v", err)
		c.Check(examineData != nil && examineData.Select != nil && examineData.Select.ReadOnly,
			"EXAMINE should report ReadOnly, data = %+v", examineData)

		// A second SELECT/EXAMINE while one is in progress fails immediately
		// and is never written.
		client5, srv5 := newScriptedClient(t, nil)
		if err := client5.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client5, srv5, true)
		first := client5.Select("INBOX", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 11, ModSeq: 100},
		})
		firstTag, firstRest := srv5.readRequest()
		c.Check(strings.HasPrefix(firstRest, "SELECT "), "first select request = %q", firstRest)

		secondErr := make(chan error, 1)
		go func() {
			_, e := client5.Select("Other", nil).Wait()
			secondErr <- e
		}()
		select {
		case e := <-secondErr:
			c.Check(e != nil, "concurrent SELECT must return a command error")
		case <-time.After(5 * time.Second):
			t.Fatal("concurrent SELECT did not return immediately")
		}
		for _, req := range srv5.snapshotRequests() {
			c.Check(!strings.Contains(req, "Other"),
				"second SELECT must not be written, requests = %v", srv5.snapshotRequests())
		}
		srv5.send("* 1 EXISTS\r\n" + firstTag + " OK selected\r\n")
		if _, err := first.WaitQResync(); err != nil {
			t.Fatalf("first select failed: %v", err)
		}
	})
}

// TestQResyncMailboxBoundary covers the boundary between the mailbox being
// closed and the mailbox being selected, including the failure paths.
func TestQResyncMailboxBoundary(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// First selection from the authenticated state: no CLOSED response
		// is expected or needed.
		client, srv := newScriptedClient(t, nil)
		if err := client.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client, srv, true)
		first := client.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100},
		})
		tag, rest := srv.readRequest()
		c.Check(rest == `SELECT "B" (QRESYNC 22 100)`, "first select request = %q", rest)
		srv.send("* 3 EXISTS\r\n* OK [UIDVALIDITY 22] uids valid\r\n" +
			"* VANISHED (EARLIER) 4:5\r\n" +
			"* 1 FETCH (UID 7 FLAGS (\\Flagged) MODSEQ (101))\r\n")
		srv.send(tag + " OK [READ-WRITE] selected\r\n")
		data, err := first.WaitQResync()
		c.Check(err == nil, "first selection failed: %v", err)
		c.Check(data != nil && len(data.Changes) == 2,
			"first selection should collect 2 deltas, got %d", dataChangesLen(data))
		mbox := client.Mailbox()
		c.Check(mbox != nil && mbox.Name == "B",
			"after first selection mailbox = %+v", mbox)

		// Switching mailboxes: trailing responses for A arrive before
		// OK [CLOSED]; they must update A's snapshot / fire callbacks but
		// never enter B's SelectData or change set.
		var (
			mu             sync.Mutex
			existsValues   []uint32
			flagsValues    [][]imap.Flag
			unilateralUIDs []imap.UID
		)
		opts := &imapclient.Options{
			UnilateralDataHandler: &imapclient.UnilateralDataHandler{
				Mailbox: func(d *imapclient.UnilateralDataMailbox) {
					mu.Lock()
					defer mu.Unlock()
					if d.NumMessages != nil {
						existsValues = append(existsValues, *d.NumMessages)
					}
					if d.Flags != nil {
						flagsValues = append(flagsValues, d.Flags)
					}
				},
				Fetch: func(msg *imapclient.FetchMessageData) {
					buf, err := msg.Collect()
					if err != nil {
						t.Errorf("collect unilateral fetch: %v", err)
						return
					}
					mu.Lock()
					unilateralUIDs = append(unilateralUIDs, buf.UID)
					mu.Unlock()
				},
			},
		}
		client2, srv2 := newScriptedClient(t, opts)
		if err := client2.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client2, srv2, true)

		selectA := client2.Select("A", nil)
		tagA, _ := srv2.readRequest()
		srv2.send("* 4 EXISTS\r\n* FLAGS (\\Seen)\r\n" + tagA + " OK selected A\r\n")
		if _, err := selectA.Wait(); err != nil {
			t.Fatalf("select A: %v", err)
		}

		switchCmd := client2.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100, KnownUIDs: imap.UIDSetNum(7)},
		})
		tagB, restB := srv2.readRequest()
		c.Check(restB == `SELECT "B" (QRESYNC 22 100 7)`, "switch request = %q", restB)
		// Trailing notifications for A, then the boundary, then B.
		// The stale A resp-code before CLOSED must not land in B's data.
		srv2.send("* 5 EXISTS\r\n* FLAGS (\\Seen \\Deleted)\r\n" +
			"* 1 FETCH (UID 3 FLAGS (\\Seen) MODSEQ (50))\r\n" +
			"* OK [UIDVALIDITY 99] stale A code\r\n")
		srv2.send("* OK [CLOSED] previous mailbox closed\r\n")
		srv2.send("* 3 EXISTS\r\n* FLAGS (\\Answered)\r\n* OK [UIDVALIDITY 22] uids valid\r\n")
		srv2.send("* VANISHED (EARLIER) 4:5\r\n" +
			"* 1 FETCH (UID 7 FLAGS (\\Flagged) MODSEQ (101))\r\n")
		srv2.send(tagB + " OK [READ-WRITE] selected B\r\n")
		bData, err := switchCmd.WaitQResync()
		c.Check(err == nil, "switch selection failed: %v", err)
		c.Check(bData != nil && bData.Select.NumMessages == 3,
			"B must start from its own EXISTS (3), got %+v", bData)
		c.Check(bData != nil && bData.Select.UIDValidity == 22,
			"pre-CLOSED A UIDVALIDITY must not leak into B, got %+v", bData)
		c.Check(bData != nil && len(bData.Changes) == 2,
			"B change set must contain only B deltas, got %d", dataChangesLen(bData))
		if bData != nil && len(bData.Changes) == 2 {
			c.Check(bData.Changes[0].Vanished != nil && bData.Changes[0].Vanished.Earlier &&
				bData.Changes[0].Vanished.UIDs.String() == "4:5",
				"first B change = %+v", bData.Changes[0])
			c.Check(bData.Changes[1].Fetch != nil && bData.Changes[1].Fetch.UID == 7,
				"second B change = %+v", bData.Changes[1])
		}
		mu.Lock()
		c.Check(containsUint32(existsValues, 5),
			"A trailing EXISTS must reach the mailbox callback, got %v", existsValues)
		c.Check(len(flagsValues) >= 1 && flagsString(flagsValues[len(flagsValues)-1]) == "\\Seen \\Deleted",
			"A trailing FLAGS must reach the mailbox callback, got %v", flagsValues)
		c.Check(containsUID(unilateralUIDs, 3),
			"A trailing FETCH (UID 3) must go to the unilateral callback, got %v", unilateralUIDs)
		mu.Unlock()
		mboxB := client2.Mailbox()
		c.Check(mboxB != nil && mboxB.Name == "B" && mboxB.NumMessages == 3,
			"after switch mailbox = %+v", mboxB)

		// Failure after CLOSED: authenticated state, A no longer exposed,
		// followed by a successful re-selection.
		client3, srv3 := newScriptedClient(t, nil)
		if err := client3.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client3, srv3, true)
		selA := client3.Select("A", nil)
		tagA3, _ := srv3.readRequest()
		srv3.send("* 4 EXISTS\r\n" + tagA3 + " OK selected A\r\n")
		if _, err := selA.Wait(); err != nil {
			t.Fatal(err)
		}
		failCmd := client3.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100},
		})
		tagFail, _ := srv3.readRequest()
		srv3.send("* OK [CLOSED] previous mailbox closed\r\n")
		srv3.send(tagFail + " NO [SERVERBUG] selection failed\r\n")
		failData, err := failCmd.WaitQResync()
		c.Check(err != nil, "NO response must return an error")
		c.Check(failData == nil, "failed selection must return nil data, got %+v", failData)
		c.Check(client3.State() == imap.ConnStateAuthenticated,
			"failed selection must leave authenticated state, got %v", client3.State())
		c.Check(client3.Mailbox() == nil,
			"failed selection must not expose the old mailbox A, got %+v", client3.Mailbox())

		retry := client3.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100},
		})
		tagRetry, _ := srv3.readRequest()
		srv3.send("* 2 EXISTS\r\n* OK [UIDVALIDITY 22] uids valid\r\n" + tagRetry + " OK selected B\r\n")
		retryData, err := retry.WaitQResync()
		c.Check(err == nil, "re-selection after failure failed: %v", err)
		c.Check(retryData != nil && retryData.Select.NumMessages == 2,
			"re-selection metadata = %+v", retryData)
		c.Check(client3.State() == imap.ConnStateSelected && client3.Mailbox() != nil &&
			client3.Mailbox().Name == "B",
			"re-selection must select B, state = %v mailbox = %+v", client3.State(), client3.Mailbox())

		// Failure before CLOSED: the old selection still ends.
		client4, srv4 := newScriptedClient(t, nil)
		if err := client4.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client4, srv4, true)
		selA4 := client4.Select("A", nil)
		tagA4, _ := srv4.readRequest()
		srv4.send("* 4 EXISTS\r\n" + tagA4 + " OK selected A\r\n")
		if _, err := selA4.Wait(); err != nil {
			t.Fatal(err)
		}
		failNoClosed := client4.Select("C", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 33, ModSeq: 100},
		})
		tagNoClosed, _ := srv4.readRequest()
		srv4.send(tagNoClosed + " NO [NONEXISTENT] no such mailbox\r\n")
		_, err = failNoClosed.WaitQResync()
		c.Check(err != nil, "NO without CLOSED must return an error")
		c.Check(client4.State() == imap.ConnStateAuthenticated && client4.Mailbox() == nil,
			"failure before CLOSED must still end the old selection, state = %v mailbox = %+v",
			client4.State(), client4.Mailbox())

		// Buffered deltas of a failed selection must not leak to callbacks
		// or to the next selection.
		var leaked int
		leakOpts := &imapclient.Options{
			UnilateralDataHandler: &imapclient.UnilateralDataHandler{
				Fetch: func(msg *imapclient.FetchMessageData) {
					mu.Lock()
					leaked++
					mu.Unlock()
					msg.Collect()
				},
				Vanished: func(data *imap.VanishedData) {
					mu.Lock()
					leaked++
					mu.Unlock()
				},
			},
		}
		client5, srv5 := newScriptedClient(t, leakOpts)
		if err := client5.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client5, srv5, true)
		selA5 := client5.Select("A", nil)
		tagA5, _ := srv5.readRequest()
		srv5.send("* 4 EXISTS\r\n" + tagA5 + " OK selected A\r\n")
		if _, err := selA5.Wait(); err != nil {
			t.Fatal(err)
		}
		badSelect := client5.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100},
		})
		tagBad, _ := srv5.readRequest()
		srv5.send("* OK [CLOSED] closed\r\n")
		srv5.send("* 1 FETCH (UID 7 FLAGS (\\Seen) MODSEQ (101))\r\n" +
			"* VANISHED 9\r\n")
		srv5.send(tagBad + " BAD invalid parameters\r\n")
		badData, err := badSelect.WaitQResync()
		c.Check(err != nil && badData == nil, "BAD response must return nil data and error")
		cleanSelect := client5.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100},
		})
		tagClean, _ := srv5.readRequest()
		srv5.send("* 2 EXISTS\r\n" + tagClean + " OK selected B\r\n")
		cleanData, err := cleanSelect.WaitQResync()
		c.Check(err == nil, "clean re-selection failed: %v", err)
		c.Check(cleanData != nil && len(cleanData.Changes) == 0,
			"failed selection deltas must not leak into the next one, got %d", dataChangesLen(cleanData))
		mu.Lock()
		c.Check(leaked == 0, "buffered deltas must not reach unilateral callbacks, leaked = %d", leaked)
		mu.Unlock()
	})
}

// TestQResyncDeltaCollection covers the QRESYNC delta content: historical vs
// real-time VANISHED, repeated FETCH updates for one UID, compact UID sets,
// unilateral VANISHED, and the continued availability of ordinary flows.
func TestQResyncDeltaCollection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var (
			mu               sync.Mutex
			unilateralVanish []*imap.VanishedData
		)
		opts := &imapclient.Options{
			UnilateralDataHandler: &imapclient.UnilateralDataHandler{
				Vanished: func(data *imap.VanishedData) {
					mu.Lock()
					unilateralVanish = append(unilateralVanish, data)
					mu.Unlock()
				},
			},
		}
		client, srv := newScriptedClient(t, opts)
		if err := client.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		enableQResync(t, client, srv, true)

		selectCmd := client.Select("B", &imap.SelectOptions{
			QResync: &imap.QResyncOptions{UIDValidity: 22, ModSeq: 100, KnownUIDs: imap.UIDSetNum(7, 30)},
		})
		tag, rest := srv.readRequest()
		c.Check(rest == `SELECT "B" (QRESYNC 22 100 7,30)`, "delta select request = %q", rest)
		// New mailbox starts at 5 messages. Three historical removals do
		// not count; one real-time removal reduces the count by one.
		srv.send("* 5 EXISTS\r\n* OK [UIDVALIDITY 22] uids valid\r\n")
		srv.send("* VANISHED (EARLIER) 10:12\r\n") // 3 historical UIDs, compact range
		srv.send("* 1 FETCH (UID 7 FLAGS (\\Seen) MODSEQ (101))\r\n")
		srv.send("* VANISHED 30\r\n") // 1 real-time removal
		srv.send("* 2 FETCH (UID 7 FLAGS (\\Seen \\Flagged) MODSEQ (102))\r\n")
		// A huge compact range must be stored as one range, never expanded.
		srv.send("* VANISHED (EARLIER) 1:4294967295\r\n")
		srv.send(tag + " OK [READ-WRITE] selected B\r\n")
		data, err := selectCmd.WaitQResync()
		c.Check(err == nil, "delta selection failed: %v", err)
		c.Check(data != nil && data.Select.NumMessages == 4,
			"EARLIER must not decrement, real-time VANISHED must: count = %d, want 4",
			dataSelectNumMessages(data))

		c.Check(data != nil && len(data.Changes) == 5,
			"expected 5 ordered changes, got %d", dataChangesLen(data))
		if data != nil && len(data.Changes) == 5 {
			// Every change carries exactly one kind of data.
			for i, ch := range data.Changes {
				c.Check((ch.Fetch != nil) != (ch.Vanished != nil),
					"change %d must contain exactly one data kind: %+v", i, ch)
			}

			hist := data.Changes[0].Vanished
			c.Check(hist != nil && hist.Earlier && hist.UIDs.String() == "10:12",
				"historical VANISHED = %+v", hist)
			if hist != nil {
				nums, ok := hist.UIDs.Nums()
				c.Check(ok && len(nums) == 3, "historical set must compactly hold 3 UIDs, got %v", nums)
			}

			firstFetch := data.Changes[1].Fetch
			c.Check(firstFetch != nil && firstFetch.UID == 7 && firstFetch.ModSeq == 101 &&
				flagsString(firstFetch.Flags) == "\\Seen",
				"first FETCH delta = %+v", firstFetch)

			realtime := data.Changes[2].Vanished
			c.Check(realtime != nil && !realtime.Earlier && realtime.UIDs.String() == "30",
				"real-time VANISHED = %+v", realtime)

			// The same UID appears twice, in response order, without merging.
			secondFetch := data.Changes[3].Fetch
			c.Check(secondFetch != nil && secondFetch.UID == 7 && secondFetch.ModSeq == 102 &&
				flagsString(secondFetch.Flags) == "\\Seen \\Flagged",
				"second FETCH delta for UID 7 = %+v", secondFetch)

			huge := data.Changes[4].Vanished
			c.Check(huge != nil && huge.Earlier && len(huge.UIDs) == 1 &&
				huge.UIDs.String() == "1:4294967295",
				"huge range must stay a single compact range, got %+v", huge)
		}

		// An unsolicited, non-EARLIER VANISHED is unilateral data and
		// decreases the live count by the number of UIDs, not by their
		// numeric values.
		srv.send("* VANISHED 100:101\r\n")
		noop := client.Noop()
		noopTag, noopRest := srv.readRequest()
		c.Check(noopRest == "NOOP", "expected NOOP request, got %q", noopRest)
		srv.send(noopTag + " OK noop done\r\n")
		c.Check(noop.Wait() == nil, "NOOP failed")
		mu.Lock()
		c.Check(len(unilateralVanish) == 1 && !unilateralVanish[0].Earlier &&
			unilateralVanish[0].UIDs.String() == "100:101",
			"unilateral VANISHED callback = %+v", unilateralVanish)
		if len(unilateralVanish) == 1 {
			nums, ok := unilateralVanish[0].UIDs.Nums()
			c.Check(ok && len(nums) == 2, "unilateral set must count 2 actual UIDs, got %v", nums)
		}
		mu.Unlock()
		mbox := client.Mailbox()
		c.Check(mbox != nil && mbox.NumMessages == 2,
			"mailbox count must drop by the 2 actual UIDs (4 -> 2), got %+v", mbox)

		// Ordinary flows without QRESYNC keep working.
		client2, srv2 := newScriptedClient(t, nil)
		if err := client2.WaitGreeting(); err != nil {
			t.Fatal(err)
		}
		plain := client2.Select("INBOX", nil)
		plainTag, plainRest := srv2.readRequest()
		c.Check(plainRest == "SELECT INBOX", "plain select request = %q", plainRest)
		srv2.send("* 3 EXISTS\r\n* FLAGS (\\Seen)\r\n" + plainTag + " OK selected\r\n")
		plainData, err := plain.Wait()
		c.Check(err == nil && plainData != nil && plainData.NumMessages == 3,
			"plain select = %+v err = %v", plainData, err)
		// WaitQResync is usable on a plain select and returns no changes.
		plain2 := client2.Select("INBOX", nil)
		plainTag2, _ := srv2.readRequest()
		srv2.send("* OK [CLOSED] closed\r\n* 3 EXISTS\r\n" + plainTag2 + " OK selected\r\n")
		qrData, err := plain2.WaitQResync()
		c.Check(err == nil && qrData != nil && len(qrData.Changes) == 0,
			"plain select via WaitQResync = %+v err = %v", qrData, err)

		// An ordinary UID FETCH is still decoded through the shared path.
		fetchCmd := client2.Fetch(imap.UIDSetNum(7), &imap.FetchOptions{
			UID: true, Flags: true, ModSeq: true,
		})
		fetchTag, fetchRest := srv2.readRequest()
		c.Check(strings.HasPrefix(fetchRest, "UID FETCH 7 (") &&
			strings.Contains(fetchRest, "FLAGS") && strings.Contains(fetchRest, "MODSEQ"),
			"UID FETCH request = %q", fetchRest)
		srv2.send("* 1 FETCH (UID 7 FLAGS (\\Seen) MODSEQ (200))\r\n" + fetchTag + " OK fetched\r\n")
		msgs, err := fetchCmd.Collect()
		c.Check(err == nil && len(msgs) == 1 && msgs[0].UID == 7 && msgs[0].ModSeq == 200,
			"ordinary FETCH result = %+v err = %v", msgs, err)

		// An ordinary unilateral EXPUNGE decreases the count by one.
		srv2.send("* 1 EXPUNGE\r\n")
		noop2 := client2.Noop()
		noop2Tag, _ := srv2.readRequest()
		srv2.send(noop2Tag + " OK noop done\r\n")
		c.Check(noop2.Wait() == nil, "second NOOP failed")
		mbox2 := client2.Mailbox()
		c.Check(mbox2 != nil && mbox2.NumMessages == 2,
			"EXPUNGE must reduce the mailbox count (3 -> 2), got %+v", mbox2)
	})
}

func dataChangesLen(data *imapclient.QResyncData) int {
	if data == nil {
		return -1
	}
	return len(data.Changes)
}

func dataSelectNumMessages(data *imapclient.QResyncData) uint32 {
	if data == nil || data.Select == nil {
		return 0
	}
	return data.Select.NumMessages
}

func containsUint32(values []uint32, want uint32) bool {
	for _, v := range values {
		if v == want {
			return true
		}
	}
	return false
}

func containsUID(values []imap.UID, want imap.UID) bool {
	for _, v := range values {
		if v == want {
			return true
		}
	}
	return false
}
