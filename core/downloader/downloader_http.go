package downloader

import (
    "bytes"

    "github.com/PuerkitoBio/goquery"
    "github.com/bitly/go-simplejson"
    //    iconv "github.com/djimenez/iconv-go"
    "github.com/hu17889/go_spider/core/common/mlog"
    "github.com/hu17889/go_spider/core/common/page"
    "github.com/hu17889/go_spider/core/common/request"
    "github.com/hu17889/go_spider/core/common/util"
    //    "golang.org/x/text/encoding/simplifiedchinese"
    //    "golang.org/x/text/transform"
    "context"
    "errors"
    "fmt"
    "io"
    "io/ioutil"
    "net/http"
    "net/http/httptrace"
    "net/url"
    //"fmt"
    "golang.org/x/net/html/charset"
    //    "regexp"
    //    "golang.org/x/net/html"
    "strings"
	"compress/gzip"
    "sync"
    "time"
)

// The HttpDownloader download page by package net/http.
// The "html" content is contained in dom parser of package goquery.
// The "json" content is saved.
// The "jsonp" content is modified to json.
// The "text" content will save body plain text only.
// The page result is saved in Page.
type HttpDownloader struct {
    // capMu guards slots. The channel is the physical request permit pool:
    // set while the downloader is idle, read while requests are running.
    capMu sync.Mutex
    slots chan struct{}
}

func NewHttpDownloader() *HttpDownloader {
    return &HttpDownloader{}
}

// SetMaxConcurrentRequests caps how many physical HTTP requests this downloader
// keeps in flight at once. Ordinary downloads, mirror primaries and mirror
// candidates all take one slot, and a slot is held until the response body is
// closed. 0 (the default) means unlimited.
//
// Configure the limit while the downloader has no active calls.
func (this *HttpDownloader) SetMaxConcurrentRequests(n uint) *HttpDownloader {
    this.capMu.Lock()
    defer this.capMu.Unlock()
    if n == 0 {
        this.slots = nil
    } else {
        this.slots = make(chan struct{}, n)
    }
    return this
}

// GetMaxConcurrentRequests reports the configured physical request limit,
// where 0 means unlimited.
func (this *HttpDownloader) GetMaxConcurrentRequests() uint {
    this.capMu.Lock()
    defer this.capMu.Unlock()
    return uint(cap(this.slots))
}

// acquireRequestSlot reserves one physical request slot. It blocks while the
// limit is exhausted and reports ok==false if cancel fires first, so a
// candidate still waiting for quota is dropped without ever sending.
func (this *HttpDownloader) acquireRequestSlot(cancel <-chan struct{}) (func(), bool) {
    this.capMu.Lock()
    slots := this.slots
    this.capMu.Unlock()
    if slots == nil {
        return func() {}, true
    }
    select {
    case slots <- struct{}{}:
        var once sync.Once
        return func() { once.Do(func() { <-slots }) }, true
    case <-cancel:
        return nil, false
    }
}

func (this *HttpDownloader) Download(req *request.Request) *page.Page {
    var mtype string
    var p = page.NewPage(req)
    mtype = req.GetResponceType()

    // A racing configuration is validated before any address is contacted,
    // including response types the downloader cannot parse.
    if len(req.GetMirrorURLs()) != 0 {
        if err := validateMirrorConfig(req); err != nil {
            p.SetStatus(true, err.Error())
            return p
        }
    }

    switch mtype {
    case "html":
        return this.downloadHtml(p, req)
    case "json":
        fallthrough
    case "jsonp":
        return this.downloadJson(p, req)
    case "text":
        return this.downloadText(p, req)
    default:
        mlog.LogInst().LogError("error request type:" + mtype)
    }
    return p
}

/*
// The acceptableCharset is test for whether Content-Type is UTF-8 or not
func (this *HttpDownloader) acceptableCharset(contentTypes []string) bool {
    // each type is like [text/html; charset=UTF-8]
    // we want the UTF-8 only
    for _, cType := range contentTypes {
        if strings.Index(cType, "UTF-8") != -1 || strings.Index(cType, "utf-8") != -1 {
            return true
        }
    }
    return false
}


// The getCharset used for parsing the header["Content-Type"] string to get charset of the page.
func (this *HttpDownloader) getCharset(header http.Header) string {
    reg, err := regexp.Compile("charset=(.*)$")
    if err != nil {
        mlog.LogInst().LogError(err.Error())
        return ""
    }

    var charset string
    for _, cType := range header["Content-Type"] {
        substrings := reg.FindStringSubmatch(cType)
        if len(substrings) == 2 {
            charset = substrings[1]
        }
    }

    return charset
}




// Use golang.org/x/text/encoding. Get page body and change it to utf-8
func (this *HttpDownloader) changeCharsetEncoding(charset string, sor io.ReadCloser) string {
    ischange := true
    var tr transform.Transformer
    cs := strings.ToLower(charset)
    if cs == "gbk" {
        tr = simplifiedchinese.GBK.NewDecoder()
    } else if cs == "gb18030" {
        tr = simplifiedchinese.GB18030.NewDecoder()
    } else if cs == "hzgb2312" || cs == "gb2312" || cs == "hz-gb2312" {
        tr = simplifiedchinese.HZGB2312.NewDecoder()
    } else {
        ischange = false
    }

    var destReader io.Reader
    if ischange {
        transReader := transform.NewReader(sor, tr)
        destReader = transReader
    } else {
        destReader = sor
    }

    var sorbody []byte
    var err error
    if sorbody, err = ioutil.ReadAll(destReader); err != nil {
        mlog.LogInst().LogError(err.Error())
        return ""
    }
    bodystr := string(sorbody)

    return bodystr
}

// Use go-iconv. Get page body and change it to utf-8

func (this *HttpDownloader) changeCharsetGoIconv(charset string, sor io.ReadCloser) string {
    var err error
    var converter *iconv.Converter
    if charset != "" && strings.ToLower(charset) != "utf-8" && strings.ToLower(charset) != "utf8" {
        converter, err = iconv.NewConverter(charset, "utf-8")
        if err != nil {
            mlog.LogInst().LogError(err.Error())
            return ""
        }
        defer converter.Close()
    }

    var sorbody []byte
    if sorbody, err = ioutil.ReadAll(sor); err != nil {
        mlog.LogInst().LogError(err.Error())
        return ""
    }
    bodystr := string(sorbody)

    var destbody string
    if converter != nil {
        // convert to utf8
        destbody, err = converter.ConvertString(bodystr)
        if err != nil {
            mlog.LogInst().LogError(err.Error())
            return ""
        }
    } else {
        destbody = bodystr
    }
    return destbody
}
*/

// Charset auto determine. Use golang.org/x/net/html/charset. Get page body and change it to utf-8
func (this *HttpDownloader) changeCharsetEncodingAuto(contentTypeStr string, sor io.ReadCloser) string {
    var err error
    destReader, err := charset.NewReader(sor, contentTypeStr)

    if err != nil {
        mlog.LogInst().LogError(err.Error())
        destReader = sor
    }

    var sorbody []byte
    if sorbody, err = ioutil.ReadAll(destReader); err != nil {
        mlog.LogInst().LogError(err.Error())
        // For gb2312, an error will be returned.
        // Error like: simplifiedchinese: invalid GBK encoding
        // return ""
    }
    //e,name,certain := charset.DetermineEncoding(sorbody,contentTypeStr)
    bodystr := string(sorbody)

    return bodystr
}

func (this *HttpDownloader) changeCharsetEncodingAutoGzipSupport(contentTypeStr string, sor io.ReadCloser) string {
	var err error
	gzipReader, err := gzip.NewReader(sor)
	if err != nil {
		mlog.LogInst().LogError(err.Error())
		return ""
	}
	defer gzipReader.Close()
	destReader, err := charset.NewReader(gzipReader, contentTypeStr)

	if err != nil {
		mlog.LogInst().LogError(err.Error())
		destReader = sor
	}

	var sorbody []byte
	if sorbody, err = ioutil.ReadAll(destReader); err != nil {
		mlog.LogInst().LogError(err.Error())
		// For gb2312, an error will be returned.
		// Error like: simplifiedchinese: invalid GBK encoding
		// return ""
	}
	//e,name,certain := charset.DetermineEncoding(sorbody,contentTypeStr)
	bodystr := string(sorbody)

	return bodystr
}



// readCandidateBody is the strict body reader used by racing candidates. The
// raw body is read to EOF first, so a truncated transfer is always an error
// even when the bytes received so far would parse. Decompression and charset
// conversion then happen in memory, and their errors fail the candidate too.
func readCandidateBody(resp *http.Response) (string, error) {
    raw, err := ioutil.ReadAll(resp.Body)
    if err != nil {
        return "", err
    }
    isGzip := strings.EqualFold(resp.Header.Get("Content-Encoding"), "gzip")
    if len(raw) == 0 && !isGzip {
        // A complete, empty 2xx representation is valid (notably for text).
        return "", nil
    }

    if isGzip {
        gzipReader, err := gzip.NewReader(bytes.NewReader(raw))
        if err != nil {
            return "", err
        }
        defer gzipReader.Close()
        if raw, err = ioutil.ReadAll(gzipReader); err != nil {
            return "", err
        }
    }

    destReader, err := charset.NewReader(bytes.NewReader(raw), resp.Header.Get("Content-Type"))
    if err != nil {
        return "", err
    }
    body, err := ioutil.ReadAll(destReader)
    if err != nil {
        return "", err
    }
    return string(body), nil
}

// newHTTPClient builds a client that honours the request's redirect callback
// and proxy.
func newHTTPClient(req *request.Request) (*http.Client, error) {
    client := &http.Client{CheckRedirect: req.GetRedirectFunc()}
    if proxystr := req.GetProxyHost(); len(proxystr) != 0 {
        proxy, err := url.Parse(proxystr)
        if err != nil {
            return nil, err
        }
        client.Transport = &http.Transport{Proxy: http.ProxyURL(proxy)}
    }
    return client, nil
}

// newHTTPRequest builds an independent request for target. Headers and cookies
// are copied, so concurrent candidates never mutate the caller's Request or
// share mutable state with one another.
func newHTTPRequest(ctx context.Context, target string, req *request.Request) (*http.Request, error) {
    httpreq, err := http.NewRequestWithContext(ctx, req.GetMethod(), target, strings.NewReader(req.GetPostdata()))
    if err != nil {
        return nil, err
    }
    if header := req.GetHeader(); header != nil {
        httpreq.Header = header.Clone()
    }
    for _, cookie := range req.GetCookies() {
        httpreq.AddCookie(cookie)
    }
    return httpreq, nil
}

// withSentTrace fires onSent once the request has actually been written to the
// wire, which is when the mirror delay starts counting.
func withSentTrace(httpreq *http.Request, onSent func()) *http.Request {
    trace := &httptrace.ClientTrace{
        WroteRequest: func(httptrace.WroteRequestInfo) { onSent() },
    }
    return httpreq.WithContext(httptrace.WithClientTrace(httpreq.Context(), trace))
}

// isNormalRedirectErr reports the historical "normal" sentinel returned from a
// CheckRedirect callback, in which case the previous response is still used.
func isNormalRedirectErr(err error) bool {
    var uerr *url.Error
    if errors.As(err, &uerr) && uerr.Err != nil {
        return uerr.Err.Error() == "normal"
    }
    return false
}

// raceContext is shared by every candidate of one Download call.
type raceContext struct {
    ctx    context.Context
    cancel context.CancelFunc

    mu        sync.Mutex
    committed bool

    sent     chan struct{}
    sentOnce sync.Once

    primaryFailed chan struct{}
    failOnce      sync.Once
}

func newRaceContext() *raceContext {
    ctx, cancel := context.WithCancel(context.Background())
    return &raceContext{
        ctx:           ctx,
        cancel:        cancel,
        sent:          make(chan struct{}),
        primaryFailed: make(chan struct{}),
    }
}

// markSent records that the primary request was actually handed to the client.
func (r *raceContext) markSent() {
    r.sentOnce.Do(func() { close(r.sent) })
}

func (r *raceContext) markPrimaryFailed() {
    r.failOnce.Do(func() { close(r.primaryFailed) })
}

// commit claims the race for the calling candidate and reports true only for
// the first caller. Every later caller loses. It also aborts live exchanges
// and releases candidates still waiting for a slot.
func (r *raceContext) commit() bool {
    r.mu.Lock()
    if r.committed {
        r.mu.Unlock()
        return false
    }
    r.committed = true
    r.mu.Unlock()
    r.cancel()
    return true
}

// stopped returns a channel closed once the race is decided or aborted.
func (r *raceContext) stopped() <-chan struct{} {
    return r.ctx.Done()
}

// stop aborts the race and every request started by it.
func (r *raceContext) stop() {
    r.cancel()
}

// tryStart reports whether a candidate may still begin sending. commit flips
// the flag under the same lock, so a candidate that observes true and sends
// does so before the race is decided, or exits before calling the server.
func (r *raceContext) tryStart() bool {
    r.mu.Lock()
    defer r.mu.Unlock()
    return !r.committed
}

type candidateResult struct {
    page   *page.Page
    target string
}

// fetchCandidate performs one complete physical download of target. It only
// returns a successful Page when the response is 2xx, the body was read in full
// and the configured RespType parsed; losers and failures return a failed Page.
// The Page always carries the original logical Request.
func (this *HttpDownloader) fetchCandidate(race *raceContext, req *request.Request, target string, primary bool) *page.Page {
    p := page.NewPage(req)

    release, ok := this.acquireRequestSlot(race.stopped())
    if !ok {
        p.SetStatus(true, "candidate canceled while waiting for a request slot")
        return p
    }
    defer release()

    if !race.tryStart() {
        p.SetStatus(true, "candidate canceled before it was sent")
        return p
    }

    client, err := newHTTPClient(req)
    if err != nil {
        p.SetStatus(true, err.Error())
        return p
    }
    httpreq, err := newHTTPRequest(race.ctx, target, req)
    if err != nil {
        p.SetStatus(true, err.Error())
        return p
    }

    if primary {
        httpreq = withSentTrace(httpreq, race.markSent)
    }

    resp, err := client.Do(httpreq)
    if err != nil {
        if !isNormalRedirectErr(err) || resp == nil {
            if primary {
                // Even a failed primary has been dispatched: start the mirror
                // clock rather than leaving the launcher waiting forever.
                race.markSent()
            }
            p.SetStatus(true, err.Error())
            return p
        }
    }
    if primary {
        // The trace reports the normal case, but a custom RoundTripper may not
        // emit it; response headers are equally proof the primary was sent.
        race.markSent()
    }
    // The physical slot is held until the body is closed.
    defer resp.Body.Close()

    p.SetHeader(resp.Header)
    p.SetCookies(resp.Cookies())

    if resp.StatusCode < 200 || resp.StatusCode > 299 {
        p.SetStatus(true, fmt.Sprintf("HTTP status %d from %s", resp.StatusCode, target))
        return p
    }

    destbody, err := readCandidateBody(resp)
    if err != nil {
        p.SetStatus(true, fmt.Sprintf("reading body from %s: %v", target, err))
        return p
    }

    switch req.GetResponceType() {
    case "html":
        doc, err := goquery.NewDocumentFromReader(strings.NewReader(destbody))
        if err != nil {
            p.SetStatus(true, err.Error())
            return p
        }
        body, err := doc.Html()
        if err != nil {
            p.SetStatus(true, err.Error())
            return p
        }
        p.SetBodyStr(body).SetHtmlParser(doc)
    case "json", "jsonp":
        body := destbody
        if req.GetResponceType() == "jsonp" {
            body = util.JsonpToJson(destbody)
        }
        js, err := simplejson.NewJson([]byte(body))
        if err != nil {
            p.SetStatus(true, err.Error())
            return p
        }
        p.SetBodyStr(body).SetJson(js)
    case "text":
        p.SetBodyStr(destbody)
    default:
        p.SetStatus(true, "error request type:"+req.GetResponceType())
        return p
    }

    // Claim the race before reporting back, so candidates still queued for a
    // physical slot are released instead of being sent after a winner exists.
    if !race.commit() {
        lost := page.NewPage(req)
        lost.SetStatus(true, "candidate lost the race to another address")
        return lost
    }

    p.SetFetchedURL(resp.Request.URL.String())
    p.SetStatus(false, "")
    return p
}

func isHTTPAbsoluteURL(u *url.URL) bool {
    return u != nil && (u.Scheme == "http" || u.Scheme == "https") && u.Host != ""
}

// validateMirrorConfig rejects any racing configuration that cannot be honoured.
func validateMirrorConfig(req *request.Request) error {
    if req.GetMethod() != "GET" {
        return fmt.Errorf("mirror racing requires method GET, got %q", req.GetMethod())
    }
    if req.GetPostdata() != "" {
        return errors.New("mirror racing requires empty Postdata")
    }
    switch req.GetResponceType() {
    case "html", "json", "jsonp", "text":
    default:
        return fmt.Errorf("mirror racing does not support response type %q", req.GetResponceType())
    }
    if req.GetMirrorDelay() < 0 {
        return errors.New("MirrorDelay must not be negative")
    }
    primary, err := url.Parse(req.GetUrl())
    if err != nil || !isHTTPAbsoluteURL(primary) {
        return fmt.Errorf("primary URL %q is not an absolute http/https URL", req.GetUrl())
    }
    for _, mirror := range req.GetMirrorURLs() {
        u, err := url.Parse(mirror)
        if err != nil || !isHTTPAbsoluteURL(u) {
            return fmt.Errorf("mirror URL %q is not an absolute http/https URL", mirror)
        }
    }
    return nil
}

// mirrorCandidates returns the primary followed by the de-duplicated mirrors in
// their original order. The primary address is not repeated.
func mirrorCandidates(req *request.Request) []string {
    primary := req.GetUrl()
    seen := map[string]bool{primary: true}
    out := []string{primary}
    for _, mirror := range req.GetMirrorURLs() {
        if seen[mirror] {
            continue
        }
        seen[mirror] = true
        out = append(out, mirror)
    }
    return out
}

func mirrorFailurePage(req *request.Request, order []string, failures map[string]string) *page.Page {
    p := page.NewPage(req)
    var b strings.Builder
    b.WriteString("all mirror candidates failed")
    for _, target := range order {
        reason := failures[target]
        if reason == "" {
            reason = "candidate did not run"
        }
        b.WriteString(fmt.Sprintf("; %s: %s", target, reason))
    }
    p.SetStatus(true, b.String())
    return p
}

// downloadMirror races the primary address against the configured mirrors.
//
// The primary is sent immediately. After MirrorDelay, measured from the moment
// the primary was actually sent, or as soon as the primary fails, all mirrors
// are opened at once. The first candidate to complete a 2xx response with a
// fully read body that parses as the requested RespType wins. The remaining
// candidates are canceled, and the winner is returned only after every local
// goroutine has exited and its physical slot has been returned.
func (this *HttpDownloader) downloadMirror(req *request.Request) *page.Page {
    if err := validateMirrorConfig(req); err != nil {
        p := page.NewPage(req)
        p.SetStatus(true, err.Error())
        return p
    }

    order := mirrorCandidates(req)
    race := newRaceContext()
    defer race.stop()

    // One result slot per address: candidate goroutines never block on an
    // unreceived send while the coordinator stops listening after a winner.
    results := make(chan candidateResult, len(order))
    var wg sync.WaitGroup
    launch := func(target string, primary bool) {
        wg.Add(1)
        go func() {
            defer wg.Done()
            p := this.fetchCandidate(race, req, target, primary)
            if p.IsSucc() {
                race.commit()
            } else if primary {
                race.markPrimaryFailed()
            }
            results <- candidateResult{page: p, target: target}
        }()
    }

    launch(order[0], true)

    // The mirror launcher waits for the primary to be sent, then for the delay.
    // A primary that fails early opens the mirrors at once; a race that has
    // already ended launches nothing.
    wg.Add(1)
    go func() {
        defer wg.Done()
        select {
        case <-race.sent:
        case <-race.primaryFailed:
            // The primary failed before it could even be sent.
        case <-race.stopped():
            return
        }

        if delay := req.GetMirrorDelay(); delay > 0 {
            timer := time.NewTimer(delay)
            select {
            case <-timer.C:
            case <-race.primaryFailed:
                // Failed mid-flight: open the mirrors now.
            case <-race.stopped():
                timer.Stop()
                return
            }
            timer.Stop()
        }

        for _, target := range order[1:] {
            if !race.tryStart() {
                return
            }
            launch(target, false)
        }
    }()

    failures := make(map[string]string)
    var winner *page.Page
    for remaining := len(order); remaining > 0; remaining-- {
        result := <-results
        if result.page.IsSucc() {
            winner = result.page
            break
        }
        failures[result.target] = result.page.Errormsg()
    }

    // Deciding the race stops unsent candidates and cancels exchanges still in
    // flight, so a slow remote body cannot delay the return. wg.Wait also makes
    // sure every physical slot has been returned before we hand back.
    race.stop()
    wg.Wait()

    if winner == nil {
        return mirrorFailurePage(req, order, failures)
    }
    return winner
}

// choose http GET/method to download
func connectByHttp(p *page.Page, req *request.Request) (*http.Response, error) {
    client := &http.Client{
        CheckRedirect: req.GetRedirectFunc(),
    }

    httpreq, err := http.NewRequest(req.GetMethod(), req.GetUrl(), strings.NewReader(req.GetPostdata()))
    if header := req.GetHeader(); header != nil {
        httpreq.Header = req.GetHeader()
    }

    if cookies := req.GetCookies(); cookies != nil {
        for i := range cookies {
            httpreq.AddCookie(cookies[i])
        }
    }

    var resp *http.Response
    if resp, err = client.Do(httpreq); err != nil {
        if e, ok := err.(*url.Error); ok && e.Err != nil && e.Err.Error() == "normal" {
            //  normal
        } else {
            mlog.LogInst().LogError(err.Error())
            p.SetStatus(true, err.Error())
            //fmt.Printf("client do error %v \r\n", err)
            return nil, err
        }
    }

    return resp, nil
}

// choose a proxy server to excute http GET/method to download
func connectByHttpProxy(p *page.Page, in_req *request.Request) (*http.Response, error) {
    request, _ := http.NewRequest("GET", in_req.GetUrl(), nil)
    proxy, err := url.Parse(in_req.GetProxyHost())
    if err != nil {
        return nil, err
    }
    client := &http.Client{
        Transport: &http.Transport{
            Proxy: http.ProxyURL(proxy),
        },
    }
    resp, err := client.Do(request)
    if err != nil {
        return nil, err
    }
    return resp, nil

}

// Download file and change the charset of page charset.
func (this *HttpDownloader) downloadFile(p *page.Page, req *request.Request) (*page.Page, string) {
    var err error
    var urlstr string
    if urlstr = req.GetUrl(); len(urlstr) == 0 {
        mlog.LogInst().LogError("url is empty")
        p.SetStatus(true, "url is empty")
        return p, ""
    }

    release, ok := this.acquireRequestSlot(nil)
    if !ok {
        p.SetStatus(true, "request canceled")
        return p, ""
    }
    defer release()

    var resp *http.Response

    if proxystr := req.GetProxyHost(); len(proxystr) != 0 {
        //using http proxy
        //fmt.Print("HttpProxy Enter ",proxystr,"\n")
        resp, err = connectByHttpProxy(p, req)
    } else {
        //normal http download
        //fmt.Print("Http Normal Enter \n",proxystr,"\n")
        resp, err = connectByHttp(p, req)
    }

    if err != nil {
        return p, ""
    }

    //b, _ := ioutil.ReadAll(resp.Body)
    //fmt.Printf("Resp body %v \r\n", string(b))

    p.SetHeader(resp.Header)
    p.SetCookies(resp.Cookies())

    // The final URL after redirects, useful for resolving relative links.
    p.SetFetchedURL(resp.Request.URL.String())

    // get converter to utf-8
	var bodyStr string
	if resp.Header.Get("Content-Encoding") == "gzip" {
		bodyStr = this.changeCharsetEncodingAutoGzipSupport(resp.Header.Get("Content-Type"), resp.Body)
	} else {
		bodyStr = this.changeCharsetEncodingAuto(resp.Header.Get("Content-Type"), resp.Body)
	}
    //fmt.Printf("utf-8 body %v \r\n", bodyStr)
    defer resp.Body.Close()
    return p, bodyStr
}

func (this *HttpDownloader) downloadHtml(p *page.Page, req *request.Request) *page.Page {
    if len(req.GetMirrorURLs()) != 0 {
        return this.downloadMirror(req)
    }

    var err error
    p, destbody := this.downloadFile(p, req)
    //fmt.Printf("Destbody %v \r\n", destbody)
    if !p.IsSucc() {
        //fmt.Print("Page error \r\n")
        return p
    }
    bodyReader := bytes.NewReader([]byte(destbody))

    var doc *goquery.Document
    if doc, err = goquery.NewDocumentFromReader(bodyReader); err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetFetchedURL("")
        p.SetStatus(true, err.Error())
        return p
    }

    var body string
    if body, err = doc.Html(); err != nil {
        mlog.LogInst().LogError(err.Error())
        p.SetFetchedURL("")
        p.SetStatus(true, err.Error())
        return p
    }

    p.SetBodyStr(body).SetHtmlParser(doc).SetStatus(false, "")

    return p
}

func (this *HttpDownloader) downloadJson(p *page.Page, req *request.Request) *page.Page {
    if len(req.GetMirrorURLs()) != 0 {
        return this.downloadMirror(req)
    }

    var err error
    p, destbody := this.downloadFile(p, req)
    if !p.IsSucc() {
        return p
    }

    var body []byte
    body = []byte(destbody)
    mtype := req.GetResponceType()
    if mtype == "jsonp" {
        tmpstr := util.JsonpToJson(destbody)
        body = []byte(tmpstr)
    }

    var r *simplejson.Json
    if r, err = simplejson.NewJson(body); err != nil {
        mlog.LogInst().LogError(string(body) + "\t" + err.Error())
        p.SetFetchedURL("")
        p.SetStatus(true, err.Error())
        return p
    }

    // json result
    p.SetBodyStr(string(body)).SetJson(r).SetStatus(false, "")

    return p
}

func (this *HttpDownloader) downloadText(p *page.Page, req *request.Request) *page.Page {
    if len(req.GetMirrorURLs()) != 0 {
        return this.downloadMirror(req)
    }

    p, destbody := this.downloadFile(p, req)
    if !p.IsSucc() {
        return p
    }

    p.SetBodyStr(destbody).SetStatus(false, "")
    return p
}
