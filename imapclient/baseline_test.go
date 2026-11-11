package imapclient

import (
	"bufio"
	"github.com/emersion/go-imap/v2/internal/imapwire"
	"strings"
	"testing"
)

func TestBaselineSource(t *testing.T) {
	for _, tc := range []struct {
		wire  string
		valid bool
	}{
		{`(TAG "A1") UID ALL 3:5 COUNT 3`, true},
		{`(TAG "A1" MAILBOX "Archive" UIDVALIDITY 7) UID ALL 3:5 COUNT 3`, false},
	} {
		dec := imapwire.NewDecoder(bufio.NewReader(strings.NewReader(tc.wire+"\r\n")), imapwire.ConnSideClient)
		tag, data, err := readESearchResponse(dec)
		if (err == nil) != tc.valid {
			t.Fatalf("wire=%q err=%v", tc.wire, err)
		}
		if tc.valid && (tag != "A1" || data.Count != 3 || data.All.String() != "3:5") {
			t.Fatalf("ordinary search=%+v", data)
		}
	}
	t.Log("BASELINE_IMAP_SINGLE_SOURCE_OK MULTIMAILBOX_CORRELATOR_UNSUPPORTED")
}
