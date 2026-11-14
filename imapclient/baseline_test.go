package imapclient_test

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/emersion/go-imap/v2"
	"github.com/emersion/go-imap/v2/imapclient"
)

// Exercise the existing public collection path without a real mail account.
func TestCondstoreCollection(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	if err := serverConn.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		t.Fatal(err)
	}
	serverDone := make(chan error, 1)
	go func() {
		defer serverConn.Close()
		if _, err := io.WriteString(serverConn, "* PREAUTH [CAPABILITY IMAP4rev1 ENABLE CONDSTORE QRESYNC] local fixture\r\n"); err != nil {
			serverDone <- err
			return
		}
		r := bufio.NewReader(serverConn)
		for i := 0; i < 2; i++ {
			line, err := r.ReadString('\n')
			if err != nil {
				serverDone <- err
				return
			}
			parts := strings.SplitN(strings.TrimSuffix(line, "\r\n"), " ", 2)
			if len(parts) != 2 {
				serverDone <- fmt.Errorf("bad command %q", line)
				return
			}
			var response string
			if i == 0 {
				if parts[1] != `SELECT INBOX (CONDSTORE)` {
					serverDone <- fmt.Errorf("unexpected SELECT %q", line)
					return
				}
				response = "* FLAGS (\\Seen \\Answered)\r\n* 2 EXISTS\r\n* OK [UIDVALIDITY 11] stable\r\n* OK [UIDNEXT 10] next\r\n* OK [HIGHESTMODSEQ 100] high\r\n" + parts[0] + " OK [READ-WRITE] selected\r\n"
			} else {
				if !strings.HasPrefix(parts[1], "UID FETCH 7 (UID ") || !strings.Contains(parts[1], "FLAGS") || !strings.Contains(parts[1], "MODSEQ") {
					serverDone <- fmt.Errorf("unexpected FETCH %q", line)
					return
				}
				response = "* 1 FETCH (UID 7 FLAGS (\\Seen) MODSEQ (101))\r\n" + parts[0] + " OK fetched\r\n"
			}
			if _, err := io.WriteString(serverConn, response); err != nil {
				serverDone <- err
				return
			}
		}
		// Keep the selected session alive until the client closes it.
		_, err := io.Copy(io.Discard, r)
		serverDone <- err
	}()
	client := imapclient.New(clientConn, nil)
	defer func() {
		client.Close()
		if err := <-serverDone; err != nil {
			t.Error(err)
		}
	}()
	if err := client.WaitGreeting(); err != nil {
		t.Fatal(err)
	}
	data, err := client.Select("INBOX", &imap.SelectOptions{CondStore: true}).Wait()
	if err != nil {
		t.Fatal(err)
	}
	if data.NumMessages != 2 || data.UIDValidity != 11 || data.UIDNext != 10 || data.HighestModSeq != 100 {
		t.Fatalf("SELECT metadata: %+v", data)
	}
	if got := client.Mailbox(); got == nil || got.Name != "INBOX" || got.NumMessages != 2 {
		t.Fatalf("selected mailbox: %+v", got)
	}
	messages, err := client.Fetch(imap.UIDSetNum(7), &imap.FetchOptions{Flags: true, ModSeq: true}).Collect()
	if err != nil {
		t.Fatal(err)
	}
	if len(messages) != 1 || messages[0].UID != 7 || messages[0].ModSeq != 101 || len(messages[0].Flags) != 1 || messages[0].Flags[0] != imap.FlagSeen {
		t.Fatalf("FETCH metadata: %+v", messages)
	}
	t.Log("CHECK_CONDSTORE_COLLECTION_OK uid=7 modseq=101 mailbox=INBOX count=2")
}
