package imapclient

import (
	"bufio"
	"github.com/emersion/go-imap/v2/internal/imapwire"
	"strings"
	"testing"
)

func TestBaselineSource(t *testing.T) {
	for _, tc := range []struct {
		wire        string
		valid       bool
		mailbox     string
		uidValidity uint32
	}{
		{`(TAG "A1") UID ALL 3:5 COUNT 3`, true, "", 0},
		{`(TAG "A1" MAILBOX "Archive" UIDVALIDITY 7) UID ALL 3:5 COUNT 3`, true, "Archive", 7},
	} {
		dec := imapwire.NewDecoder(bufio.NewReader(strings.NewReader(tc.wire+"\r\n")), imapwire.ConnSideClient)
		source, data, err := readESearchResponse(dec)
		if (err == nil) != tc.valid {
			t.Fatalf("wire=%q err=%v", tc.wire, err)
		}
		if !tc.valid {
			continue
		}
		if source.tag != "A1" || data.Count != 3 || data.All.String() != "3:5" {
			t.Fatalf("ordinary search=%+v", data)
		}
		if tc.mailbox != "" {
			if !source.multi || source.mailbox != tc.mailbox || source.uidValidity != tc.uidValidity {
				t.Fatalf("multi-mailbox correlator=%+v", source)
			}
		} else if source.multi {
			t.Fatalf("unexpected multi-mailbox correlator=%+v", source)
		}
	}
	t.Log("BASELINE_IMAP_SINGLE_SOURCE_OK MULTIMAILBOX_CORRELATOR_SUPPORTED")
}
