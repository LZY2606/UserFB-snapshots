package imapclient

import (
	"fmt"
	"strings"

	"github.com/emersion/go-imap/v2"
)

// QResyncChange is a single QRESYNC delta response received during a
// SELECT/EXAMINE command (RFC 7162).
//
// Exactly one of Fetch and Vanished is non-nil. The order of the changes
// matches the order of the corresponding server responses.
type QResyncChange struct {
	// Fetch contains a FETCH delta response (UID, FLAGS and MODSEQ are
	// populated). Multiple updates for the same UID are kept as separate
	// changes.
	Fetch *FetchMessageBuffer
	// Vanished contains a VANISHED response, describing removed UIDs.
	Vanished *imap.VanishedData
}

// QResyncData is the data returned by a QRESYNC-enabled SELECT or EXAMINE
// command.
type QResyncData struct {
	// Select contains the mailbox metadata.
	Select *imap.SelectData
	// Changes contains the QRESYNC delta responses, in response order.
	Changes []QResyncChange
}

// handleVanished decodes a VANISHED response (RFC 7162).
func (c *Client) handleVanished() error {
	earlier := false
	if c.dec.Special('(') {
		var token string
		if !c.dec.ExpectAtom(&token) || !strings.EqualFold(token, "EARLIER") ||
			!c.dec.ExpectSpecial(')') || !c.dec.ExpectSP() {
			return fmt.Errorf("in VANISHED: %w", c.dec.Err())
		}
		earlier = true
	}

	var uids imap.UIDSet
	if !c.dec.ExpectUIDSet(&uids) {
		return fmt.Errorf("in VANISHED: %w", c.dec.Err())
	}

	data := &imap.VanishedData{Earlier: earlier, UIDs: uids}

	// A VANISHED response belonging to a mailbox being selected is part of
	// its QRESYNC delta set.
	if selectCmd := findPendingCmdByType[*SelectCommand](c); selectCmd != nil &&
		selectCmd.collectingNewMailbox() && selectCmd.qresync {
		selectCmd.changes = append(selectCmd.changes, QResyncChange{Vanished: data})
		if !earlier {
			// A real-time removal reduces the new mailbox's message
			// count; an EARLIER response describes history and does not.
			selectCmd.decrementNumMessages(uids)
		}
		return nil
	}

	// Outside of a QRESYNC selection, a VANISHED response is unilateral
	// data. Only a non-EARLIER response reduces the current count.
	if !earlier {
		c.decrementMailboxByUIDSet(uids)
	}
	if handler := c.options.unilateralDataHandler().Vanished; handler != nil {
		handler(data)
	}
	return nil
}

// decrementMailboxByUIDSet decreases the current mailbox message count by the
// number of UIDs in the set. The UIDs are never interpreted as sequence
// numbers and the set is never expanded: the range lengths are summed
// arithmetically.
func (c *Client) decrementMailboxByUIDSet(uids imap.UIDSet) {
	n, ok := countUIDs(uids)
	if !ok {
		// A dynamic set ("*") cannot be counted without knowing the
		// mailbox maximum; a non-EARLIER VANISHED response cannot contain
		// one.
		return
	}

	c.mutex.Lock()
	if c.state == imap.ConnStateSelected && c.mailbox != nil {
		c.mailbox = c.mailbox.copy()
		if n > uint64(c.mailbox.NumMessages) {
			c.mailbox.NumMessages = 0
		} else {
			c.mailbox.NumMessages -= uint32(n)
		}
	}
	c.mutex.Unlock()
}

// decrementNumMessages decreases the selected mailbox metadata held by a
// pending SelectCommand by the number of UIDs in the set.
func (cmd *SelectCommand) decrementNumMessages(uids imap.UIDSet) {
	n, ok := countUIDs(uids)
	if !ok {
		return
	}
	if n > uint64(cmd.data.NumMessages) {
		cmd.data.NumMessages = 0
	} else {
		cmd.data.NumMessages -= uint32(n)
	}
}

// countUIDs returns the number of UIDs contained in the set without expanding
// the ranges into a slice. The second return value is false if the set
// contains dynamic values ("*") which cannot be counted without knowing the
// mailbox maximum.
func countUIDs(uids imap.UIDSet) (uint64, bool) {
	var total uint64
	for _, r := range uids {
		if r.Start == 0 || r.Stop == 0 {
			return 0, false
		}
		total += uint64(r.Stop) - uint64(r.Start) + 1
	}
	return total, true
}
