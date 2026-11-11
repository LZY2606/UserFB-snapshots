# warc

warc provides primitives for reading and writing [WARC files](http://bibnum.bnf.fr/WARC/)
in Go. This version is based on [edsu's warc library](https://github.com/edsu/warc),
but many changes were made:

This package works with WARC files in plain text, GZip compression and BZip2 compression out of the box.
The record content is exposed via `io.Reader` interfaces. Types and functions were renamed
to follow [Go's naming conventions](https://blog.golang.org/package-names).
All external dependencies were removed. A Writer was added.

### Example

The following example reads a WARC file from `stdin` and prints
the header values of each record to `stdout`.

```go
reader, err := warc.NewReader(os.Stdin)
if err != nil {
	panic(err)
}
defer reader.Close()

for {
	record, err := reader.ReadRecord()
	if err != nil {
		break
	}
	fmt.Println("Record:")
	for key, value := range record.Header {
		fmt.Printf("%v = %v\n", key, value)
	}
}
```

The next example writes a WARC record to `stdout`.

```go
writer := warc.NewWriter(os.Stdout)
record := warc.NewRecord()
record.Header.Set("warc-type", "resource")
record.Header.Set("content-type", "plain/text")
record.Content = strings.NewReader("Hello, World!")
if _, err := writer.WriteRecord(record); err != nil {
	panic(err)
}
```

### Random access to individual captures

Wrapping a single `Writer` in one `gzip.Writer` compresses several records
into the same gzip member, so no record boundary survives in the file.
`NewIndexedWriter` instead compresses each record into its own standalone
gzip member and reports the physical byte interval (`Member`) it occupies.
An archive index can store those intervals and replay any single capture
with `ReadMemberAt`, without scanning or decompressing the rest of the file.

The following example writes two captures to disk and then replays only
the second one:

```go
file, err := os.Create("captures.warc.gz")
if err != nil {
	panic(err)
}
defer file.Close()

writer := warc.NewIndexedWriter(file)

var index []warc.Member
for _, payload := range []string{"first capture", "second capture"} {
	record := warc.NewRecord()
	record.Header.Set("warc-type", "resource")
	record.Header.Set("content-type", "text/plain")
	record.Content = strings.NewReader(payload)
	member, err := writer.WriteRecord(record)
	if err != nil {
		panic(err)
	}
	// Persist member.Offset and member.Length in your archive index.
	index = append(index, member)
}
if err := file.Close(); err != nil {
	panic(err)
}

// Later, possibly in another process: reopen the archive and replay
// just the second capture.
archive, err := os.Open("captures.warc.gz")
if err != nil {
	panic(err)
}
defer archive.Close()

record, err := warc.ReadMemberAt(archive, index[1])
if err != nil {
	panic(err)
}
content, err := ioutil.ReadAll(record.Content)
if err != nil {
	panic(err)
}
fmt.Println(string(content)) // second capture
```

Each member is a complete gzip member (header and trailer included); the
returned `Length` is the compressed size on disk, not the WARC
`Content-Length`. Reads performed by `ReadMemberAt` never leave the given
interval, so truncated or corrupt members are rejected without borrowing
bytes from neighboring captures.

### Performance

Parsing WARC files is as fast as it can get. The real overhead stems from
the underlying compression algorithms. So if you are about to parse the same
file several times for whatever reason, consider decompressing it first.

### License

warc is released under CC0 license.
You can find a copy of the CC0 License in the [LICENSE](./LICENSE) file.
