package imapclient_test

import (
	"bufio"
	"errors"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/emersion/go-imap/v2"
	"github.com/emersion/go-imap/v2/imapclient"
)

// multiSearchServer is a minimal scripted IMAP server running on one end of
// a net.Pipe.
type multiSearchServer struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

// newMultiSearchClient creates a client connected to a scripted server. The
// script runs in a separate goroutine and is responsible for checking
// request bytes and sending responses.
func newMultiSearchClient(t *testing.T, script func(s *multiSearchServer)) *imapclient.Client {
	t.Helper()

	clientConn, serverConn := net.Pipe()
	s := &multiSearchServer{t: t, conn: serverConn, r: bufio.NewReader(serverConn)}

	done := make(chan struct{})
	go func() {
		defer close(done)
		defer serverConn.Close()
		if !s.writeLine("* OK [CAPABILITY IMAP4rev1 MULTISEARCH ESEARCH] test server ready") {
			return
		}
		script(s)
		// Keep the connection open until the client is done with it, so
		// that connection state assertions aren't raced by the server
		// hanging up. Any further client request is unexpected.
		s.conn.SetReadDeadline(time.Time{})
		for {
			line, err := s.readLine()
			if err != nil {
				break
			}
			s.t.Errorf("server: unexpected request after script end: %q", line)
		}
	}()

	client := imapclient.New(clientConn, nil)
	if err := client.WaitGreeting(); err != nil {
		t.Fatalf("WaitGreeting() = %v", err)
	}
	t.Cleanup(func() {
		client.Close()
		<-done
	})
	return client
}

func (s *multiSearchServer) readLine() (string, error) {
	line, err := s.r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r"), nil
}

// expectLine reads a single client request line and compares it with the
// expected wire bytes. On mismatch the script must abort.
func (s *multiSearchServer) expectLine(want string) bool {
	got, err := s.readLine()
	if err != nil {
		s.t.Errorf("server: readLine() = %v, want %q", err, want)
		return false
	}
	if got != want {
		s.t.Errorf("server: request = %q, want %q", got, want)
		return false
	}
	return true
}

func (s *multiSearchServer) writeLine(line string) bool {
	if _, err := s.conn.Write([]byte(line + "\r\n")); err != nil {
		s.t.Errorf("server: writeLine(%q) = %v", line, err)
		return false
	}
	return true
}

func TestMultiSearchSourceDispatch(t *testing.T) {
	t.Run("Interleaved", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			if !s.expectLine(`T1 SELECT INBOX`) {
				return
			}
			if !s.writeLine(`* FLAGS (\Answered \Flagged \Deleted \Seen \Draft)`) {
				return
			}
			if !s.writeLine(`* 3 EXISTS`) {
				return
			}
			if !s.writeLine(`* 0 RECENT`) {
				return
			}
			if !s.writeLine(`* OK [UIDVALIDITY 11] UIDs valid`) {
				return
			}
			if !s.writeLine(`T1 OK [READ-WRITE] SELECT completed`) {
				return
			}
			if !s.expectLine(`T2 SEARCH RETURN (COUNT) ESEARCH IN (MAILBOXES (INBOX "Archive")) TEXT "hello"`) {
				return
			}
			if !s.expectLine(`T3 UID SEARCH RETURN (MIN) TEXT "world"`) {
				return
			}
			if !s.expectLine(`T4 SEARCH ESEARCH IN (MAILBOXES ("Sent")) FLAGGED`) {
				return
			}
			// Interleave the ESEARCH responses of the three in-flight
			// commands, completing the last issued command first
			if !s.writeLine(`* ESEARCH (TAG "T4" MAILBOX "Sent" UIDVALIDITY 9) UID ALL 7`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T2" MAILBOX "INBOX" UIDVALIDITY 11) UID COUNT 2`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T3") UID MIN 5`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T2" MAILBOX "Archive" UIDVALIDITY 12) UID COUNT 3`) {
				return
			}
			if !s.writeLine(`T4 OK SEARCH completed`) {
				return
			}
			if !s.writeLine(`T2 OK SEARCH completed`) {
				return
			}
			if !s.writeLine(`T3 OK SEARCH completed`) {
				return
			}
		})
		defer client.Close()

		if _, err := client.Select("INBOX", nil).Wait(); err != nil {
			t.Fatalf("Select().Wait() = %v", err)
		}
		mboxBefore := client.Mailbox()
		if mboxBefore == nil || mboxBefore.Name != "INBOX" {
			t.Fatalf("Mailbox() = %+v", mboxBefore)
		}
		stateBefore := client.State()

		msCmd1 := client.MultiSearch([]string{"INBOX", "Archive"}, &imap.SearchCriteria{
			Text: []string{"hello"},
		}, &imap.SearchOptions{ReturnCount: true})
		uidCmd := client.UIDSearch(&imap.SearchCriteria{
			Text: []string{"world"},
		}, &imap.SearchOptions{ReturnMin: true})
		msCmd2 := client.MultiSearch([]string{"Sent"}, &imap.SearchCriteria{
			Flag: []imap.Flag{imap.FlagFlagged},
		}, nil)

		// The last issued command completes first
		res2, err := msCmd2.Wait()
		if err != nil {
			t.Fatalf("MultiSearch(Sent).Wait() = %v", err)
		}
		if len(res2) != 1 {
			t.Fatalf("MultiSearch(Sent) returned %v results, want 1", len(res2))
		}
		if res2[0].Mailbox != "Sent" || res2[0].UIDValidity != 9 {
			t.Errorf("MultiSearch(Sent) source = %q/%v, want Sent/9", res2[0].Mailbox, res2[0].UIDValidity)
		}
		if uidSet, ok := res2[0].SearchData.All.(imap.UIDSet); !ok {
			t.Errorf("MultiSearch(Sent) All = %T, want imap.UIDSet", res2[0].SearchData.All)
		} else if uids := res2[0].SearchData.AllUIDs(); len(uids) != 1 || uids[0] != 7 || len(uidSet) != 1 {
			t.Errorf("MultiSearch(Sent) UIDs = %v, want [7]", uids)
		}

		res1, err := msCmd1.Wait()
		if err != nil {
			t.Fatalf("MultiSearch(INBOX, Archive).Wait() = %v", err)
		}
		if len(res1) != 2 {
			t.Fatalf("MultiSearch(INBOX, Archive) returned %v results, want 2", len(res1))
		}
		// Results are ordered by arrival, not by request order
		if res1[0].Mailbox != "INBOX" || res1[0].UIDValidity != 11 || res1[0].SearchData.Count != 2 {
			t.Errorf("result[0] = %q/%v count=%v, want INBOX/11 count=2", res1[0].Mailbox, res1[0].UIDValidity, res1[0].SearchData.Count)
		}
		if res1[1].Mailbox != "Archive" || res1[1].UIDValidity != 12 || res1[1].SearchData.Count != 3 {
			t.Errorf("result[1] = %q/%v count=%v, want Archive/12 count=3", res1[1].Mailbox, res1[1].UIDValidity, res1[1].SearchData.Count)
		}

		uidData, err := uidCmd.Wait()
		if err != nil {
			t.Fatalf("UIDSearch().Wait() = %v", err)
		}
		if uidData.Min != 5 {
			t.Errorf("UIDSearch() Min = %v, want 5", uidData.Min)
		}

		// The MULTISEARCH commands must not change the connection state
		// nor the selected mailbox
		if client.State() != stateBefore {
			t.Errorf("State() = %v, want %v", client.State(), stateBefore)
		}
		if client.Mailbox() != mboxBefore {
			t.Errorf("Mailbox() changed from %+v to %+v", mboxBefore, client.Mailbox())
		}
	})

	t.Run("EmptyResult", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			if !s.expectLine(`T1 SEARCH ESEARCH IN (MAILBOXES (INBOX "Archive")) TEXT "nothing"`) {
				return
			}
			// No matching message: the server doesn't send any ESEARCH
			// response at all
			if !s.writeLine(`T1 OK SEARCH completed`) {
				return
			}
		})
		defer client.Close()

		results, err := client.MultiSearch([]string{"INBOX", "Archive"}, &imap.SearchCriteria{
			Text: []string{"nothing"},
		}, nil).Wait()
		if err != nil {
			t.Fatalf("MultiSearch().Wait() = %v", err)
		}
		if len(results) != 0 {
			t.Errorf("MultiSearch() returned %v results, want 0", len(results))
		}
	})

	t.Run("MailboxEncoding", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			// Mailbox names must go through the mailbox encoder:
			// quoting for the name with a space, modified UTF-7 for the
			// non-ASCII name
			if !s.expectLine(`T1 SEARCH ESEARCH IN (MAILBOXES ("Archive 2024" "&X1JoYw-")) ALL`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T1" MAILBOX "Archive 2024" UIDVALIDITY 100) UID ALL 1:2`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T1" MAILBOX "&X1JoYw-" UIDVALIDITY 200) UID ALL 3`) {
				return
			}
			if !s.writeLine(`T1 OK SEARCH completed`) {
				return
			}
		})
		defer client.Close()

		results, err := client.MultiSearch([]string{"Archive 2024", "归档"}, &imap.SearchCriteria{}, nil).Wait()
		if err != nil {
			t.Fatalf("MultiSearch().Wait() = %v", err)
		}
		if len(results) != 2 {
			t.Fatalf("MultiSearch() returned %v results, want 2", len(results))
		}
		if results[0].Mailbox != "Archive 2024" || results[0].UIDValidity != 100 {
			t.Errorf("result[0] source = %q/%v, want \"Archive 2024\"/100", results[0].Mailbox, results[0].UIDValidity)
		}
		if got := results[0].SearchData.AllUIDs(); len(got) != 2 || got[0] != 1 || got[1] != 2 {
			t.Errorf("result[0] UIDs = %v, want [1 2]", got)
		}
		if results[1].Mailbox != "归档" || results[1].UIDValidity != 200 {
			t.Errorf("result[1] source = %q/%v, want 归档/200", results[1].Mailbox, results[1].UIDValidity)
		}
		if got := results[1].SearchData.AllUIDs(); len(got) != 1 || got[0] != 3 {
			t.Errorf("result[1] UIDs = %v, want [3]", got)
		}
	})

	t.Run("SameUIDDistinctSource", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			if !s.expectLine(`T1 SEARCH ESEARCH IN (MAILBOXES ("A" "B")) ALL`) {
				return
			}
			// Both mailboxes contain a message with UID 3: these are two
			// distinct sources
			if !s.writeLine(`* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 10) UID ALL 3`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T1" MAILBOX "B" UIDVALIDITY 20) UID ALL 3`) {
				return
			}
			if !s.writeLine(`T1 OK SEARCH completed`) {
				return
			}
		})
		defer client.Close()

		results, err := client.MultiSearch([]string{"A", "B"}, &imap.SearchCriteria{}, nil).Wait()
		if err != nil {
			t.Fatalf("MultiSearch().Wait() = %v", err)
		}
		if len(results) != 2 {
			t.Fatalf("MultiSearch() returned %v results, want 2", len(results))
		}
		for i, want := range []struct {
			mailbox     string
			uidValidity uint32
		}{{"A", 10}, {"B", 20}} {
			res := results[i]
			if res.Mailbox != want.mailbox || res.UIDValidity != want.uidValidity {
				t.Errorf("result[%v] source = %q/%v, want %q/%v", i, res.Mailbox, res.UIDValidity, want.mailbox, want.uidValidity)
			}
			if uids := res.SearchData.AllUIDs(); len(uids) != 1 || uids[0] != 3 {
				t.Errorf("result[%v] UIDs = %v, want [3]", i, uids)
			}
		}
	})

	t.Run("MalformedSource", func(t *testing.T) {
		for _, tc := range []struct {
			name    string
			esearch string
		}{
			{"MissingUIDValidity", `* ESEARCH (TAG "T1" MAILBOX "A") UID ALL 3`},
			{"MissingMailbox", `* ESEARCH (TAG "T1" UIDVALIDITY 7) UID ALL 3`},
			{"MissingTag", `* ESEARCH (MAILBOX "A" UIDVALIDITY 7) UID ALL 3`},
			{"DuplicateMailbox", `* ESEARCH (TAG "T1" MAILBOX "A" MAILBOX "B" UIDVALIDITY 7) UID ALL 3`},
			{"DuplicateUIDValidity", `* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 7 UIDVALIDITY 8) UID ALL 3`},
			{"ZeroUIDValidity", `* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 0) UID ALL 3`},
			{"MissingUIDMarker", `* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 7) ALL 3`},
			{"MissingUIDMarkerEmpty", `* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 7)`},
		} {
			t.Run(tc.name, func(t *testing.T) {
				client := newMultiSearchClient(t, func(s *multiSearchServer) {
					if !s.expectLine(`T1 SEARCH ESEARCH IN (MAILBOXES ("A")) ALL`) {
						return
					}
					if !s.writeLine(tc.esearch) {
						return
					}
				})
				defer client.Close()

				_, err := client.MultiSearch([]string{"A"}, &imap.SearchCriteria{}, nil).Wait()
				if err == nil {
					t.Fatalf("MultiSearch().Wait() = nil, want error for malformed response %q", tc.esearch)
				}
			})
		}
	})

	t.Run("NoResponse", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			if !s.expectLine(`T1 SEARCH ESEARCH IN (MAILBOXES ("A" "B")) TEXT "x"`) {
				return
			}
			if !s.expectLine(`T2 SEARCH ESEARCH IN (MAILBOXES ("C")) TEXT "y"`) {
				return
			}
			if !s.writeLine(`* ESEARCH (TAG "T1" MAILBOX "A" UIDVALIDITY 1) UID ALL 5`) {
				return
			}
			if !s.writeLine(`T1 NO cannot search B`) {
				return
			}
			// The other in-flight command is unaffected and completes
			if !s.writeLine(`* ESEARCH (TAG "T2" MAILBOX "C" UIDVALIDITY 2) UID ALL 6`) {
				return
			}
			if !s.writeLine(`T2 OK SEARCH completed`) {
				return
			}
		})
		defer client.Close()

		cmd1 := client.MultiSearch([]string{"A", "B"}, &imap.SearchCriteria{Text: []string{"x"}}, nil)
		cmd2 := client.MultiSearch([]string{"C"}, &imap.SearchCriteria{Text: []string{"y"}}, nil)

		res1, err := cmd1.Wait()
		if err == nil {
			t.Fatalf("MultiSearch(A, B).Wait() = nil, want NO error")
		}
		var imapErr *imap.Error
		if !errors.As(err, &imapErr) || imapErr.Type != imap.StatusResponseTypeNo {
			t.Errorf("MultiSearch(A, B).Wait() = %v, want NO status", err)
		}
		// Results received before the error are preserved
		if len(res1) != 1 || res1[0].Mailbox != "A" || res1[0].UIDValidity != 1 {
			t.Errorf("MultiSearch(A, B) results = %+v, want 1 result for A/1", res1)
		}

		res2, err := cmd2.Wait()
		if err != nil {
			t.Fatalf("MultiSearch(C).Wait() = %v", err)
		}
		if len(res2) != 1 || res2[0].Mailbox != "C" || res2[0].UIDValidity != 2 {
			t.Errorf("MultiSearch(C) results = %+v, want 1 result for C/2", res2)
		}
	})

	t.Run("RejectedBeforeWrite", func(t *testing.T) {
		client := newMultiSearchClient(t, func(s *multiSearchServer) {
			// The client must not write any request for invalid
			// invocations
			s.conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
			if line, err := s.readLine(); err == nil {
				s.t.Errorf("server: unexpected request bytes: %q", line)
			}
		})
		defer client.Close()

		if _, err := client.MultiSearch(nil, &imap.SearchCriteria{}, nil).Wait(); err == nil {
			t.Errorf("MultiSearch(nil).Wait() = nil, want error")
		}
		if _, err := client.MultiSearch([]string{}, &imap.SearchCriteria{}, nil).Wait(); err == nil {
			t.Errorf("MultiSearch(empty).Wait() = nil, want error")
		}
		options := &imap.SearchOptions{ReturnSave: true}
		if _, err := client.MultiSearch([]string{"INBOX"}, &imap.SearchCriteria{}, options).Wait(); err == nil {
			t.Errorf("MultiSearch(ReturnSave).Wait() = nil, want error")
		}
	})
}
