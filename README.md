# oggvorbis
a native go ogg/vorbis decoder

[![GoDoc](https://godocs.io/github.com/jfreymuth/oggvorbis?status.svg)](https://godocs.io/github.com/jfreymuth/oggvorbis)

## Usage

This package provides the type oggvorbis.Reader, which can be used to read .ogg files.

	r, err := oggvorbis.NewReader(reader)
	// handle error

	fmt.Println(r.SampleRate())
	fmt.Println(r.Channels())

	buffer := make([]float32, 8192)
	for {
		n, err := r.Read(buffer)

		// use buffer[:n]

		if err == io.EOF {
			break
		}
		if err != nil {
			// handle error
		}
	}

The reader also provides methods for seeking, these will only work if the reader
was created from an io.ReadSeeker.

There are also convenience functions to read an entire (small) file, similar to ioutil.ReadAll.

	data, format, err := oggvorbis.ReadAll(reader)

## Chained ogg streams

Recording archives often concatenate several independently encoded ogg/vorbis
logical streams into one physical file. The same `Reader`, `Read`, `Position`,
`Length`, `SetPosition`, `ReadAll` and `GetLength` API handles both a single
stream and such a chain — there is no separate chained-stream interface.

- Positions and lengths are **global frame coordinates** over the whole chain:
  each segment starts at local frame zero and is placed right after the
  previous segment's last valid frame. Coordinates count frames, not
  interleaved float values.
- `Read` returns the per-segment decodes concatenated, and one call may cross a
  segment boundary; an end-of-stream granule only trims that segment's tail.
- All segments must share the first segment's sample rate and channel count
  (no resampling or channel conversion). Setup, codebooks, block sizes,
  bitrates and comments may differ. `SampleRate`, `Channels`, `Bitrate` and
  `CommentHeader` always describe the first segment.
- Bad CRCs, continuation-flag mismatches, serial changes before end of stream,
  a missing end-of-stream page before another beginning-of-stream page, broken
  pages and sequence gaps are all errors; nothing is skipped silently.
- A final segment without an end-of-stream page is still accepted when the
  physical EOF follows a complete page with no unfinished packet (the legacy
  `eof_issue.ogg` case). Truncated pages, dangling packets or a following
  beginning-of-stream page are not accepted.

### Index cost for seekable inputs

With a plain `io.Reader`, input is consumed in order with at most one page of
readahead, the whole chain is never buffered, and `Length` stays zero until
the physical EOF is confirmed.

With an `io.ReadSeeker`, `NewReader` scans the container once, CRC-checks
every page and builds an index of page offsets/granules, per-segment header
packets and trimmed lengths. It never decodes audio ahead and never keeps
compressed page bodies, so index size grows with the number of pages,
segments and header packets — not with the audio's duration — while the PCM
working set stays bounded by one decoder's block size. `SetPosition`
binary-searches the index and re-reads only the pages needed to rebuild a
multi-page packet and the vorbis preroll; unrelated prefixes are never
rescanned.
