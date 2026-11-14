package imap

// QResyncOptions contains the parameters of a QRESYNC SELECT parameter
// (RFC 7162).
//
// It requires the QRESYNC extension to be enabled via ENABLE.
type QResyncOptions struct {
	// UIDValidity is the UIDVALIDITY of the mailbox at the time of the
	// last synchronization. Must be non-zero.
	UIDValidity uint32
	// ModSeq is the highest mod-sequence of the mailbox at the time of
	// the last synchronization. Must be non-zero.
	ModSeq uint64
	// KnownUIDs contains the UIDs known to the client at the time of the
	// last synchronization. It must not contain the dynamic "*" value or
	// reference the last SEARCH result ("$"). An empty set omits the
	// parameter entirely.
	KnownUIDs UIDSet
}

// VanishedData is the data returned by a VANISHED response (RFC 7162).
type VanishedData struct {
	// Earlier indicates that the UIDs have been removed at some point in
	// the past. An Earlier VANISHED response does not decrease the
	// current number of messages in the mailbox.
	Earlier bool
	// UIDs is the compact set of UIDs that have been removed.
	UIDs UIDSet
}
