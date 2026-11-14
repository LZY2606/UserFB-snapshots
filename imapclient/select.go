package imapclient

import (
	"fmt"
	"sync"

	"github.com/emersion/go-imap/v2"
	"github.com/emersion/go-imap/v2/internal"
)

// Select sends a SELECT or EXAMINE command.
//
// A nil options pointer is equivalent to a zero options value.
func (c *Client) Select(mailbox string, options *imap.SelectOptions) *SelectCommand {
	cmdName := "SELECT"
	if options != nil && options.ReadOnly {
		cmdName = "EXAMINE"
	}

	var qresyncOpts *imap.QResyncOptions
	if options != nil {
		qresyncOpts = options.QResync
	}
	cmd := &SelectCommand{mailbox: mailbox, qresync: qresyncOpts != nil}

	if qresyncOpts != nil {
		// Validate everything before touching the wire, so that an invalid
		// request never leaves the connection waiting for a response.
		if err := validateQResyncOptions(qresyncOpts); err != nil {
			return failedSelectCommand(cmd, err)
		}
	}

	c.mutex.Lock()
	if qresyncOpts != nil && !c.enabled.Has(imap.CapQResync) {
		c.mutex.Unlock()
		return failedSelectCommand(cmd, fmt.Errorf("imapclient: cannot use QRESYNC: extension not confirmed via ENABLE"))
	}
	// Check-and-set is atomic: only one SELECT/EXAMINE may be in progress
	// at a time. A second one fails immediately without writing anything.
	if c.selectInProgress {
		c.mutex.Unlock()
		return failedSelectCommand(cmd, fmt.Errorf("imapclient: cannot send SELECT/EXAMINE while another SELECT/EXAMINE is in progress"))
	}
	// If a mailbox is currently selected, its trailing responses arrive
	// before the OK [CLOSED] boundary and must not be collected into the
	// new mailbox. Otherwise, responses belong to the new mailbox from the
	// start.
	cmd.newMailbox = c.state != imap.ConnStateSelected
	c.selectInProgress = true
	c.mutex.Unlock()

	enc := c.beginCommand(cmdName, cmd)
	enc.SP().Mailbox(mailbox)

	hasCondStore := options != nil && options.CondStore
	if qresyncOpts != nil || hasCondStore {
		// All select parameters go into a single parenthesized list.
		listEnc := enc.SP().BeginList()
		if hasCondStore {
			listEnc.Item().Atom("CONDSTORE")
		}
		if qresyncOpts != nil {
			itemEnc := listEnc.Item()
			itemEnc.Atom("QRESYNC").SP().
				Number(qresyncOpts.UIDValidity).SP().
				ModSeq(qresyncOpts.ModSeq)
			if len(qresyncOpts.KnownUIDs) > 0 {
				itemEnc.SP().NumSet(qresyncOpts.KnownUIDs)
			}
		}
		listEnc.End()
	}

	enc.end()
	return cmd
}

func validateQResyncOptions(qresync *imap.QResyncOptions) error {
	if qresync.UIDValidity == 0 {
		return fmt.Errorf("imapclient: QRESYNC UIDVALIDITY must be non-zero")
	}
	if qresync.ModSeq == 0 {
		return fmt.Errorf("imapclient: QRESYNC mod-sequence must be non-zero")
	}
	if qresync.KnownUIDs.Dynamic() {
		return fmt.Errorf("imapclient: QRESYNC known UIDs must not contain \"*\" or reference the last SEARCH result (\"$\")")
	}
	return nil
}

// failedSelectCommand returns a SelectCommand that is already completed with
// an error, without sending anything to the server.
func failedSelectCommand(cmd *SelectCommand, err error) *SelectCommand {
	done := make(chan error, 1)
	done <- err
	close(done)
	cmd.commandBase = commandBase{done: done, err: err}
	return cmd
}

// Unselect sends an UNSELECT command.
//
// This command requires support for IMAP4rev2 or the UNSELECT extension.
func (c *Client) Unselect() *Command {
	cmd := &unselectCommand{}
	c.beginCommand("UNSELECT", cmd).end()
	return &cmd.Command
}

// UnselectAndExpunge sends a CLOSE command.
//
// CLOSE implicitly performs a silent EXPUNGE command.
func (c *Client) UnselectAndExpunge() *Command {
	cmd := &unselectCommand{}
	c.beginCommand("CLOSE", cmd).end()
	return &cmd.Command
}

func (c *Client) handleFlags() error {
	flags, err := internal.ExpectFlagList(c.dec)
	if err != nil {
		return err
	}

	selectCmd := findPendingCmdByType[*SelectCommand](c)
	if selectCmd != nil && selectCmd.collectingNewMailbox() {
		selectCmd.data.Flags = flags
		return nil
	}

	// Before the OK [CLOSED] boundary, the response still belongs to the
	// mailbox being closed: update its snapshot and notify the caller.
	c.mutex.Lock()
	if c.state == imap.ConnStateSelected {
		c.mailbox = c.mailbox.copy()
		c.mailbox.Flags = flags
	}
	c.mutex.Unlock()

	if handler := c.options.unilateralDataHandler().Mailbox; handler != nil {
		handler(&UnilateralDataMailbox{Flags: flags})
	}

	return nil
}

func (c *Client) handleExists(num uint32) error {
	selectCmd := findPendingCmdByType[*SelectCommand](c)
	if selectCmd != nil && selectCmd.collectingNewMailbox() {
		selectCmd.data.NumMessages = num
		return nil
	}

	// Before the OK [CLOSED] boundary, the response still belongs to the
	// mailbox being closed: update its snapshot and notify the caller.
	c.mutex.Lock()
	if c.state == imap.ConnStateSelected {
		c.mailbox = c.mailbox.copy()
		c.mailbox.NumMessages = num
	}
	c.mutex.Unlock()

	if handler := c.options.unilateralDataHandler().Mailbox; handler != nil {
		handler(&UnilateralDataMailbox{NumMessages: &num})
	}
	return nil
}

// SelectCommand is a SELECT command.
type SelectCommand struct {
	commandBase
	mailbox string
	data    imap.SelectData

	// qresync is set when the command was sent with a QRESYNC parameter.
	qresync bool

	// newMailbox becomes true once the boundary of the previous mailbox
	// (OK [CLOSED], or the start of the command when there was no selected
	// mailbox) has been crossed. From that point on, responses belong to
	// the mailbox being selected.
	newMailbox bool

	// fetchWG tracks the goroutines fully decoding buffered FETCH delta
	// responses, so that WaitQResync never returns partially decoded data.
	fetchWG sync.WaitGroup

	// changes buffers the QRESYNC delta responses (FETCH and VANISHED)
	// belonging to the newly selected mailbox, in response order.
	changes []QResyncChange
}

// Wait blocks until the command has completed.
func (cmd *SelectCommand) Wait() (*imap.SelectData, error) {
	return &cmd.data, cmd.wait()
}

// WaitQResync blocks until the command has completed and returns the mailbox
// metadata together with the QRESYNC delta responses.
//
// The delta responses are fully decoded while they are received, so the
// caller doesn't need to drain any data before the command completes. On a
// NO or BAD response, nil and an error are returned, and no buffered data is
// exposed.
func (cmd *SelectCommand) WaitQResync() (*QResyncData, error) {
	if err := cmd.wait(); err != nil {
		return nil, err
	}
	// Make sure every buffered FETCH response has been fully decoded.
	cmd.fetchWG.Wait()
	return &QResyncData{
		Select:  &cmd.data,
		Changes: cmd.changes,
	}, nil
}

// collectingNewMailbox reports whether responses currently being decoded
// belong to the newly selected mailbox (rather than to a mailbox being
// closed).
func (cmd *SelectCommand) collectingNewMailbox() bool {
	return cmd.newMailbox
}

type unselectCommand struct {
	Command
}
