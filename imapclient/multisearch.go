package imapclient

import (
	"fmt"

	"github.com/emersion/go-imap/v2"
)

// MultiSearchData contains the result of a MULTISEARCH command for a single
// mailbox.
type MultiSearchData struct {
	// Mailbox is the name of the mailbox this result belongs to, as
	// reported by the server in the ESEARCH correlator.
	Mailbox string
	// UIDValidity is the UIDVALIDITY value of the mailbox, as reported by
	// the server in the ESEARCH correlator.
	UIDValidity uint32
	// SearchData contains the search results. The numbers are always UIDs,
	// never message sequence numbers: they must not be interpreted in the
	// context of the currently selected mailbox.
	SearchData imap.SearchData
}

// MultiSearch sends a SEARCH command with an ESEARCH IN source option,
// searching multiple mailboxes with a single command (RFC 7377
// MULTISEARCH).
//
// The server must advertise the MULTISEARCH capability. Unlike Search and
// UIDSearch, this command doesn't operate on the currently selected mailbox
// and doesn't change the connection state.
//
// Only an explicit, non-empty list of mailboxes is supported. The RETURN
// (SAVE) option (SearchOptions.ReturnSave) is not supported.
//
// A nil options pointer is equivalent to a zero options value.
func (c *Client) MultiSearch(mailboxes []string, criteria *imap.SearchCriteria, options *imap.SearchOptions) *MultiSearchCommand {
	cmd := &MultiSearchCommand{data: []*MultiSearchData{}}

	if len(mailboxes) == 0 {
		cmd.err = fmt.Errorf("imapclient: MultiSearch requires at least one mailbox")
		return cmd
	}
	if options != nil && options.ReturnSave {
		cmd.err = fmt.Errorf("imapclient: MultiSearch doesn't support RETURN (SAVE)")
		return cmd
	}

	// See Client.search for the charset selection rationale
	var charset string
	if !c.Caps().Has(imap.CapIMAP4rev2) && !c.enabled.Has(imap.CapUTF8Accept) && !searchCriteriaIsASCII(criteria) {
		charset = "UTF-8"
	}

	enc := c.beginCommand("SEARCH", cmd)
	if returnOpts := returnSearchOptions(options); len(returnOpts) > 0 {
		enc.SP().Atom("RETURN").SP().List(len(returnOpts), func(i int) {
			enc.Atom(returnOpts[i])
		})
	}
	enc.SP().Atom("ESEARCH").SP().Atom("IN").SP().Special('(').Atom("MAILBOXES").SP().Special('(')
	for i, mailbox := range mailboxes {
		if i > 0 {
			enc.SP()
		}
		enc.Mailbox(mailbox)
	}
	enc.Special(')').Special(')').SP()
	if charset != "" {
		enc.Atom("CHARSET").SP().Atom(charset).SP()
	}
	writeSearchKey(enc.Encoder, criteria)
	enc.end()
	return cmd
}

// MultiSearchCommand is a MULTISEARCH command.
type MultiSearchCommand struct {
	commandBase
	data []*MultiSearchData
}

// Wait blocks until the command has completed.
//
// The results are ordered by arrival: each ESEARCH response received from
// the server produces one item. Mailboxes without any matching message are
// not included in the results.
//
// On error, the results received prior to the error are still returned.
func (cmd *MultiSearchCommand) Wait() ([]*MultiSearchData, error) {
	return cmd.data, cmd.wait()
}
