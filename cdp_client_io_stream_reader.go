package cdp

import (
	"context"

	"github.com/mafredri/cdp/protocol/io"
)

// NewIOStreamReader returns a reader for io.Stream that implements io.Reader
// from the standard library.
func (c *Client) NewIOStreamReader(ctx context.Context, handle io.StreamHandle) *io.StreamReader {
	return io.NewStreamReader(ctx, c.IO, handle)
}

// NewSequentialIOStreamReader returns a reader for io.Stream that implements
// io.Reader from the standard library and omits the offset on every IO.read
// request. Use this for sequential streams such as the ones returned by
// Fetch.takeResponseBodyAsStream, which reject positioned reads.
func (c *Client) NewSequentialIOStreamReader(ctx context.Context, handle io.StreamHandle) *io.StreamReader {
	return io.NewSequentialStreamReader(ctx, c.IO, handle)
}
