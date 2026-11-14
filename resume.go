package sitemap

import (
	"bytes"
	"context"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	// sitemapNamespace is the only XML namespace accepted for a document
	// root. A root without any namespace is accepted as well.
	sitemapNamespace = "http://www.sitemaps.org/schemas/sitemap/0.9"

	// resumeCheckpointVersion is the on-disk checkpoint format version.
	resumeCheckpointVersion = 1

	// maxCapturedRootTag bounds the amount of raw input retained while the
	// document root is being located. The root start tag itself is tiny; the
	// limit only guards against a hostile stream with an endless prologue.
	maxCapturedRootTag = 1 << 20
)

// ErrResumeMismatch reports that a conditional download cannot be continued
// from the saved checkpoint: the remote representation, or the byte range the
// server returned for it, is not the one which was checkpointed. No sitemap
// entry is delivered when this error is returned, and the checkpoint file is
// left untouched.
//
// Callers can test for it with errors.Is.
var ErrResumeMismatch = errors.New("sitemap: resume checkpoint does not match the remote representation")

// resumeCheckpoint is the durable state required to continue a download in a
// later process. Offsets are byte offsets into the original HTTP response body,
// never entry indexes, character positions, or counts of bytes handed out by
// an underlying buffered reader.
type resumeCheckpoint struct {
	Version       int    `json:"version"`
	ETag          string `json:"etag"`
	ContentLength int64  `json:"contentLength"`
	RequestMethod string `json:"requestMethod"`
	RequestURL    string `json:"requestURL"`
	DocumentURL   string `json:"documentURL"`
	Offset        int64  `json:"offset"`
	RootTag       string `json:"rootTag"`
	Complete      bool   `json:"complete"`
}

// ResumeFromSite downloads a plain, UTF-8 sitemap urlset and delivers its
// entries to consumer, persisting progress at checkpointPath so a later,
// possibly separate, process can continue where the previous one stopped.
//
// Only ordinary urlset documents are understood. Sitemap indexes are not
// followed, compressed files are not handled, and no address other than the
// requested one (and redirects the HTTP client follows) is fetched. The
// document root must be an <urlset> in the sitemap namespace or in no
// namespace. Entries must be direct children of that root. Other subtrees are
// skipped, and a <url> from an extension namespace is not treated as an entry.
// Entry values and date parsing are exactly those of Parse.
//
// If no checkpoint exists, rawURL is fetched with a GET request. It must be an
// absolute HTTP(S) URL. The response must be 200, carry a strong ETag and a
// positive Content-Length, and must not be encoded (Accept-Encoding: identity is
// requested explicitly). The document address recorded is the URL of the
// actual response, falling back to the requested URL when the response does
// not carry one.
//
// An entry counts as acknowledged only when consumer returns nil. After every
// acknowledged entry the checkpoint is replaced atomically with the byte offset
// immediately following that entry's </...url> end tag, together with the raw
// root start tag needed to restore namespace context. A consumer error stops
// delivery and leaves the previous acknowledged position in place; the failed
// entry is not committed. A failed checkpoint write stops delivery immediately
// and removes the temporary file of that write.
//
// When an incomplete checkpoint exists, ResumeFromSite requests
// "Range: bytes=<offset>-" with "If-Range: <ETag>" from the recorded document
// address. The response must be a 206 whose ETag is identical, whose
// Content-Range describes exactly one range starting at the saved offset and
// ending at total-1, and whose Content-Length equals the remaining range
// length. Identity of the representation and of those headers is validated
// before any entry is delivered. A 200, a 416, a changed document address, or
// any identity/header mismatch fails with an error matching ErrResumeMismatch;
// the checkpoint is not rewritten and nothing is delivered from scratch. Other
// HTTP failures, read errors and context cancellation retain their original
// cause for errors.Is.
//
// The resumed body is the raw suffix of the original document. Its root context
// is restored internally; the synthetic root start tag is never counted in
// reported byte offsets. A completed checkpoint is marked only after the root
// has closed, the remaining XML has been read completely to EOF, and the
// received byte count agrees with the declared length. Calling ResumeFromSite
// again with a completed checkpoint performs no HTTP request and delivers
// nothing.
//
// Delivery is at least once. If the process stops after the consumer returned
// nil but before the new checkpoint is published, that entry can be delivered
// again by the next call. It is not exactly-once with respect to any external
// system the consumer writes to, so consumers should make their processing
// idempotent. Concurrent updates to the same checkpoint path are not
// coordinated.
func ResumeFromSite(ctx context.Context, client *http.Client, rawURL, checkpointPath string, consumer EntryConsumer) error {
	if consumer == nil {
		return errors.New("sitemap: ResumeFromSite requires a non-nil EntryConsumer")
	}
	if ctx == nil {
		ctx = context.Background()
	}

	parsed, err := parseSitemapURL(rawURL)
	if err != nil {
		return err
	}

	if client == nil {
		client = http.DefaultClient
	}

	checkpoint, exists, err := loadResumeCheckpoint(checkpointPath)
	if err != nil {
		return err
	}
	if exists {
		if err := checkpoint.validate(); err != nil {
			return err
		}
		if checkpoint.RequestURL != parsed.String() {
			return fmt.Errorf("%w: checkpoint belongs to request %q, not %q",
				ErrResumeMismatch, checkpoint.RequestURL, parsed.String())
		}
		if checkpoint.Complete {
			return nil
		}
		return continueFromCheckpoint(ctx, client, checkpoint, checkpointPath, consumer)
	}

	return startFromBeginning(ctx, client, parsed, checkpointPath, consumer)
}

func parseSitemapURL(rawURL string) (*url.URL, error) {
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return nil, fmt.Errorf("sitemap: invalid URL %q: %w", rawURL, err)
	}
	if !parsed.IsAbs() || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
		return nil, fmt.Errorf("sitemap: URL %q must be an absolute HTTP(S) URL", rawURL)
	}
	return parsed, nil
}

func startFromBeginning(ctx context.Context, client *http.Client, parsed *url.URL, checkpointPath string, consumer EntryConsumer) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, parsed.String(), nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept-Encoding", "identity")

	response, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("sitemap: GET %s: %w", parsed, err)
	}
	if response.Body != nil {
		defer response.Body.Close()
	}

	if err := validateFullResponse(response); err != nil {
		return err
	}
	if response.Body == nil {
		return errors.New("sitemap: response has no body")
	}

	contentLength, _ := responseContentLength(response)
	checkpoint := &resumeCheckpoint{
		Version:       resumeCheckpointVersion,
		ETag:          headerValue(response.Header, "Etag"),
		ContentLength: contentLength,
		RequestMethod: http.MethodGet,
		RequestURL:    parsed.String(),
		DocumentURL:   finalDocumentURL(response, parsed.String()),
	}

	source := &resumeSource{
		reader:       response.Body,
		ctx:          ctx,
		captureLimit: maxCapturedRootTag,
	}
	run := &resumeRun{
		ctx:            ctx,
		decoder:        xml.NewDecoder(source),
		source:         source,
		checkpoint:     checkpoint,
		checkpointPath: checkpointPath,
		consumer:       consumer,
		fresh:          true,
	}
	return run.execute()
}

func continueFromCheckpoint(ctx context.Context, client *http.Client, checkpoint *resumeCheckpoint, checkpointPath string, consumer EntryConsumer) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, checkpoint.DocumentURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept-Encoding", "identity")
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-", checkpoint.Offset))
	req.Header.Set("If-Range", checkpoint.ETag)

	response, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("sitemap: ranged GET %s: %w", checkpoint.DocumentURL, err)
	}
	if response.Body != nil {
		defer response.Body.Close()
	}

	if err := validateRangeResponse(checkpoint, response); err != nil {
		return err
	}
	if response.Body == nil {
		return errors.New("sitemap: response has no body")
	}

	// The body is only the suffix of the original document. Prepending the
	// captured root start tag restores the root element and every namespace
	// declaration made on it, without requiring the server to wrap the suffix.
	syntheticRoot := checkpoint.RootTag
	source := &resumeSource{reader: response.Body, ctx: ctx}
	combined := io.MultiReader(strings.NewReader(syntheticRoot), source)
	run := &resumeRun{
		ctx:            ctx,
		decoder:        xml.NewDecoder(combined),
		source:         source,
		baseOffset:     checkpoint.Offset,
		syntheticBytes: int64(len(syntheticRoot)),
		checkpoint:     checkpoint,
		checkpointPath: checkpointPath,
		consumer:       consumer,
		fresh:          false,
	}
	return run.execute()
}

func validateFullResponse(response *http.Response) error {
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("sitemap: unexpected HTTP status %d", response.StatusCode)
	}
	if !isStrongETag(headerValue(response.Header, "Etag")) {
		return errors.New("sitemap: response requires a strong ETag")
	}
	if length, ok := responseContentLength(response); !ok || length <= 0 {
		return errors.New("sitemap: response requires a positive Content-Length")
	}
	if encoding := strings.TrimSpace(headerValue(response.Header, "Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
		return fmt.Errorf("sitemap: unsupported Content-Encoding %q", encoding)
	}
	return nil
}

func validateRangeResponse(checkpoint *resumeCheckpoint, response *http.Response) error {
	if response.StatusCode == http.StatusOK || response.StatusCode == http.StatusRequestedRangeNotSatisfiable {
		return fmt.Errorf("%w: server answered range request with %d", ErrResumeMismatch, response.StatusCode)
	}
	if response.StatusCode != http.StatusPartialContent {
		return fmt.Errorf("sitemap: unexpected HTTP status %d for range request", response.StatusCode)
	}

	if got := finalDocumentURL(response, checkpoint.DocumentURL); got != checkpoint.DocumentURL {
		return fmt.Errorf("%w: document address changed from %q to %q", ErrResumeMismatch, checkpoint.DocumentURL, got)
	}

	etag := headerValue(response.Header, "Etag")
	if !isStrongETag(etag) || etag != checkpoint.ETag {
		return fmt.Errorf("%w: ETag changed from %q to %q", ErrResumeMismatch, checkpoint.ETag, etag)
	}

	if encoding := strings.TrimSpace(headerValue(response.Header, "Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
		return fmt.Errorf("%w: unsupported Content-Encoding %q", ErrResumeMismatch, encoding)
	}

	start, end, total, ok := parseContentRange(headerValue(response.Header, "Content-Range"))
	if !ok {
		return fmt.Errorf("%w: missing or malformed Content-Range", ErrResumeMismatch)
	}
	if start != checkpoint.Offset || total != checkpoint.ContentLength || end != total-1 {
		return fmt.Errorf("%w: Content-Range %d-%d/%d does not continue offset %d of %d",
			ErrResumeMismatch, start, end, total, checkpoint.Offset, checkpoint.ContentLength)
	}

	length, ok := responseContentLength(response)
	if !ok || length != end-start+1 {
		return fmt.Errorf("%w: Content-Length %d does not match range length %d",
			ErrResumeMismatch, length, end-start+1)
	}

	return nil
}

// parseContentRange accepts exactly one "bytes start-end/total" range.
func parseContentRange(value string) (start, end, total int64, ok bool) {
	value = strings.TrimSpace(value)
	unit, spec, found := strings.Cut(value, " ")
	if !found || !strings.EqualFold(unit, "bytes") {
		return 0, 0, 0, false
	}
	spec = strings.TrimSpace(spec)
	if strings.Contains(spec, ",") {
		return 0, 0, 0, false
	}
	slash := strings.IndexByte(spec, '/')
	if slash < 0 || strings.Count(spec, "/") != 1 {
		return 0, 0, 0, false
	}
	rangePart, totalPart := spec[:slash], spec[slash+1:]
	if totalPart == "*" {
		return 0, 0, 0, false
	}
	dash := strings.IndexByte(rangePart, '-')
	if dash <= 0 || strings.Count(rangePart, "-") != 1 {
		return 0, 0, 0, false
	}
	var err error
	if start, err = strconv.ParseInt(rangePart[:dash], 10, 64); err != nil {
		return 0, 0, 0, false
	}
	if end, err = strconv.ParseInt(rangePart[dash+1:], 10, 64); err != nil {
		return 0, 0, 0, false
	}
	if total, err = strconv.ParseInt(totalPart, 10, 64); err != nil {
		return 0, 0, 0, false
	}
	if start < 0 || end < start || total <= 0 || end >= total {
		return 0, 0, 0, false
	}
	return start, end, total, true
}

func isStrongETag(value string) bool {
	if len(value) < 2 || value[0] != '"' || value[len(value)-1] != '"' {
		return false
	}
	// A single strong entity-tag has no further quotes in its opaque part.
	return !strings.ContainsRune(value[1:len(value)-1], '"')
}

// headerValue looks a header up without relying on the map key being
// canonicalized, so in-memory transports may spell headers loosely.
func headerValue(header http.Header, name string) string {
	if value := header.Get(name); strings.TrimSpace(value) != "" {
		return strings.TrimSpace(value)
	}
	for key, values := range header {
		if len(values) > 0 && strings.EqualFold(key, name) {
			return strings.TrimSpace(values[0])
		}
	}
	return ""
}

func responseContentLength(response *http.Response) (int64, bool) {
	if value := strings.TrimSpace(headerValue(response.Header, "Content-Length")); value != "" {
		length, err := strconv.ParseInt(value, 10, 64)
		if err != nil || length < 0 {
			return 0, false
		}
		return length, true
	}
	if response.ContentLength >= 0 {
		return response.ContentLength, true
	}
	return 0, false
}

func finalDocumentURL(response *http.Response, fallback string) string {
	if response.Request != nil && response.Request.URL != nil {
		return response.Request.URL.String()
	}
	return fallback
}

func (checkpoint *resumeCheckpoint) validate() error {
	if checkpoint.Version != resumeCheckpointVersion {
		return fmt.Errorf("sitemap: unsupported resume checkpoint version %d", checkpoint.Version)
	}
	if !isStrongETag(checkpoint.ETag) {
		return errors.New("sitemap: resume checkpoint has no strong ETag")
	}
	if checkpoint.ContentLength <= 0 {
		return errors.New("sitemap: resume checkpoint has no positive content length")
	}
	if checkpoint.RequestMethod != http.MethodGet || checkpoint.RequestURL == "" || checkpoint.DocumentURL == "" {
		return errors.New("sitemap: resume checkpoint has no valid request identity")
	}
	if checkpoint.Offset < 0 || checkpoint.Offset > checkpoint.ContentLength {
		return fmt.Errorf("sitemap: resume checkpoint offset %d is outside the %d byte document",
			checkpoint.Offset, checkpoint.ContentLength)
	}
	if !checkpoint.Complete {
		// A checkpoint is only ever published after an entry was delivered, so
		// an offset of zero cannot describe a real entry boundary.
		if checkpoint.Offset <= 0 || checkpoint.Offset >= checkpoint.ContentLength {
			return fmt.Errorf("sitemap: incomplete resume checkpoint has no resumable position (offset %d of %d)",
				checkpoint.Offset, checkpoint.ContentLength)
		}
		if checkpoint.RootTag == "" {
			return errors.New("sitemap: resume checkpoint does not carry root context")
		}
	} else if checkpoint.Offset != checkpoint.ContentLength {
		return errors.New("sitemap: completed resume checkpoint does not cover the whole document")
	}
	return nil
}

func loadResumeCheckpoint(path string) (*resumeCheckpoint, bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, false, nil
		}
		return nil, false, fmt.Errorf("sitemap: read resume checkpoint: %w", err)
	}

	var checkpoint resumeCheckpoint
	if err := json.Unmarshal(data, &checkpoint); err != nil {
		return nil, false, fmt.Errorf("sitemap: corrupt resume checkpoint %q: %w", path, err)
	}
	return &checkpoint, true, nil
}

// writeResumeCheckpoint publishes state atomically: the JSON is written to a
// temporary file in the destination directory, synced and closed, and only then
// renamed over the checkpoint. A failure removes the temporary file.
func writeResumeCheckpoint(path string, checkpoint *resumeCheckpoint) error {
	data, err := json.Marshal(checkpoint)
	if err != nil {
		return fmt.Errorf("sitemap: encode resume checkpoint: %w", err)
	}
	data = append(data, '\n')

	directory := filepath.Dir(path)
	prefix := "." + filepath.Base(path) + ".tmp-"
	file, err := os.CreateTemp(directory, prefix)
	if err != nil {
		return fmt.Errorf("sitemap: create resume checkpoint: %w", err)
	}
	temporary := file.Name()
	published := false
	defer func() {
		if !published {
			_ = os.Remove(temporary)
		}
	}()

	if _, err := file.Write(data); err != nil {
		_ = file.Close()
		return fmt.Errorf("sitemap: write resume checkpoint: %w", err)
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		return fmt.Errorf("sitemap: sync resume checkpoint: %w", err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("sitemap: close resume checkpoint: %w", err)
	}
	if err := os.Rename(temporary, path); err != nil {
		return fmt.Errorf("sitemap: publish resume checkpoint: %w", err)
	}
	published = true
	return nil
}

// resumeSource counts the bytes actually received from the HTTP body and,
// during the first download, captures the raw prologue so the root start tag
// can be replayed on resume. It also makes cancellation interrupt reads.
type resumeSource struct {
	reader       io.Reader
	ctx          context.Context
	received     int64
	capture      []byte
	captureLimit int
}

func (source *resumeSource) Read(buffer []byte) (int, error) {
	if source.ctx != nil {
		if err := source.ctx.Err(); err != nil {
			return 0, err
		}
	}
	count, err := source.reader.Read(buffer)
	if count > 0 {
		source.received += int64(count)
		if room := source.captureLimit - len(source.capture); room > 0 {
			if room > count {
				room = count
			}
			source.capture = append(source.capture, buffer[:room]...)
		}
	}
	return count, err
}

func (source *resumeSource) rootTagAt(end int64) (string, error) {
	if end <= 0 || end > int64(len(source.capture)) {
		return "", errors.New("sitemap: cannot capture urlset root tag")
	}
	prologue := source.capture[:end]
	index := bytes.LastIndexByte(prologue, '<')
	if index < 0 {
		return "", errors.New("sitemap: cannot locate urlset root tag")
	}
	return string(prologue[index:]), nil
}

type resumeRun struct {
	ctx            context.Context
	decoder        *xml.Decoder
	source         *resumeSource
	baseOffset     int64
	syntheticBytes int64
	checkpoint     *resumeCheckpoint
	checkpointPath string
	consumer       EntryConsumer
	fresh          bool
}

// absoluteOffset converts the decoder position back into a byte offset in the
// original HTTP body. The synthetic root start tag replayed for a resumed
// download is excluded.
func (run *resumeRun) absoluteOffset() int64 {
	return run.baseOffset + (run.decoder.InputOffset() - run.syntheticBytes)
}

func (run *resumeRun) execute() error {
	if run.fresh {
		if err := run.captureRoot(); err != nil {
			return err
		}
	} else if err := run.restoreRoot(); err != nil {
		return err
	}
	return run.consumeEntries()
}

// captureRoot walks the prologue of a fresh download, validates the root and
// records its raw start tag.
func (run *resumeRun) captureRoot() error {
	for {
		if err := run.ctx.Err(); err != nil {
			return err
		}
		token, err := run.decoder.Token()
		if err == io.EOF {
			return errors.New("sitemap: document has no urlset root element")
		}
		if err != nil {
			return err
		}
		start, ok := token.(xml.StartElement)
		if !ok {
			continue
		}
		if err := run.acceptRoot(start); err != nil {
			return err
		}
		rootTag, err := run.source.rootTagAt(run.decoder.InputOffset())
		if err != nil {
			return err
		}
		run.checkpoint.RootTag = rootTag
		run.source.captureLimit = 0
		run.source.capture = run.source.capture[:0]
		return nil
	}
}

// restoreRoot consumes the synthetic root start tag and verifies that the
// checkpoint's namespace context is still usable.
func (run *resumeRun) restoreRoot() error {
	if err := run.ctx.Err(); err != nil {
		return err
	}
	token, err := run.decoder.Token()
	if err != nil {
		return fmt.Errorf("sitemap: restore urlset root context: %w", err)
	}
	start, ok := token.(xml.StartElement)
	if !ok {
		return errors.New("sitemap: checkpoint root context is malformed")
	}
	if err := run.acceptRoot(start); err != nil {
		return fmt.Errorf("sitemap: checkpoint root context is invalid: %w", err)
	}
	if run.decoder.InputOffset() != run.syntheticBytes {
		return errors.New("sitemap: checkpoint root context does not match its saved length")
	}
	return nil
}

func (run *resumeRun) acceptRoot(start xml.StartElement) error {
	if start.Name.Local != "urlset" {
		return fmt.Errorf("sitemap: document root is %q, only a plain urlset is supported", start.Name.Local)
	}
	switch start.Name.Space {
	case sitemapNamespace, "":
		return nil
	default:
		return fmt.Errorf("sitemap: unsupported urlset namespace %q", start.Name.Space)
	}
}

// isEntryElement reports whether a direct child of the root is a sitemap entry.
// Entries are unprefixed urls of a no-namespace document and urls in the
// sitemap namespace, including urls written with a prefix bound to it. A url
// from any other (extension) namespace is not an entry.
func isEntryElement(name xml.Name) bool {
	return name.Local == "url" && (name.Space == "" || name.Space == sitemapNamespace)
}

func (run *resumeRun) consumeEntries() error {
	for {
		if err := run.ctx.Err(); err != nil {
			return err
		}
		token, err := run.decoder.Token()
		if err == io.EOF {
			return errors.New("sitemap: urlset root was not closed")
		}
		if err != nil {
			return err
		}

		switch element := token.(type) {
		case xml.StartElement:
			if isEntryElement(element.Name) {
				if err := run.deliverEntry(element); err != nil {
					return err
				}
				continue
			}
			// Skip every other subtree, including urls in extension
			// namespaces, without treating any of their children as entries.
			if err := run.decoder.Skip(); err != nil {
				return err
			}
		case xml.EndElement:
			return run.finishAfterRoot()
		}
	}
}

func (run *resumeRun) deliverEntry(start xml.StartElement) error {
	entry := newSitemapEntry()
	if err := run.decoder.DecodeElement(entry, &start); err != nil {
		return err
	}
	if err := run.ctx.Err(); err != nil {
		return err
	}
	if err := run.consumer(entry); err != nil {
		return err
	}

	// The consumer acknowledged this entry. Only now is its position durable.
	// The position comes from the decoder's parsed-token offset, not from the
	// number of bytes the buffered reader has already fetched.
	run.checkpoint.Offset = run.absoluteOffset()
	return writeResumeCheckpoint(run.checkpointPath, run.checkpoint)
}

// finishAfterRoot verifies that nothing but XML miscellany follows the closed
// root and then seals the checkpoint.
func (run *resumeRun) finishAfterRoot() error {
	for {
		if err := run.ctx.Err(); err != nil {
			return err
		}
		token, err := run.decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		switch element := token.(type) {
		case xml.StartElement:
			return fmt.Errorf("sitemap: unexpected <%s> after the closed urlset root", element.Name.Local)
		case xml.CharData:
			if strings.TrimSpace(string(element)) != "" {
				return errors.New("sitemap: unexpected data after the closed urlset root")
			}
		}
	}
	return run.sealCheckpoint()
}

func (run *resumeRun) sealCheckpoint() error {
	received := run.source.received
	if run.fresh {
		if received != run.checkpoint.ContentLength {
			return fmt.Errorf("sitemap: received %d bytes, response declared %d", received, run.checkpoint.ContentLength)
		}
	} else {
		remaining := run.checkpoint.ContentLength - run.baseOffset
		if received != remaining {
			return fmt.Errorf("sitemap: received %d suffix bytes, range declared %d", received, remaining)
		}
	}
	if run.absoluteOffset() != run.checkpoint.ContentLength {
		return fmt.Errorf("sitemap: parsed position %d does not match document length %d",
			run.absoluteOffset(), run.checkpoint.ContentLength)
	}

	run.checkpoint.Offset = run.checkpoint.ContentLength
	run.checkpoint.Complete = true
	return writeResumeCheckpoint(run.checkpointPath, run.checkpoint)
}
