package links

import (
	"bufio"
	"bytes"
	"io"
	"net/url"
	"strings"

	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"
)

const (
	keySRC    = "src"
	keySRCS   = "srcset"
	keyHREF   = "href"
	keyDATA   = "data"
	keyACTION = "action"
	keyPOSTER = "poster"
	keyREL    = "rel"

	relStyleSheet = "stylesheet"
)

// HTMLHandler is a callback for found links.
type HTMLHandler func(atom.Atom, string)

// TokenFilter is a callback for token filtration.
type TokenFilter func(html.Token) bool

// HTMLParams holds config for ExtractHTML.
type HTMLParams struct {
	Filter       TokenFilter
	HandleHTML   HTMLHandler
	HandleStatic URLHandler
	// HandleCSS, when set, receives every reference found in an inline style
	// block: imports are flagged as such, and both imports and resources are
	// reported. When nil, the legacy ExtractCSS path is used.
	HandleCSS func(CSSReference)
	// HandleStyleSheet, when set, receives external stylesheet links (rel
	// contains "stylesheet", ascii case-insensitive), reported in addition to
	// the regular HandleHTML call. Only meaningful with ScanCSS.
	HandleStyleSheet HTMLHandler
	Brute            bool
	ScanJS           bool
	ScanCSS          bool
}

// AllowALL - stub that implements TokenFilter, it allows all tokens.
func AllowALL(_ html.Token) bool { return true }

// ExtractHTML extract urls from html.
func ExtractHTML(r io.Reader, base *url.URL, cfg HTMLParams) {
	var (
		tkns        = html.NewTokenizer(r)
		key         = keySRC
		tok         html.Token
		isJS, isCSS bool
	)

	for {
		switch tkns.Next() {
		case html.ErrorToken:
			return

		case html.StartTagToken, html.SelfClosingTagToken:
			if tok = tkns.Token(); cfg.Filter(tok) {
				isJS, isCSS = false, false

				if cfg.ScanCSS && cfg.HandleStyleSheet != nil &&
					tok.DataAtom == atom.Link && isStyleSheetLink(tok) {
					// A stylesheet link is a style entry: it goes through
					// HandleStyleSheet only and is never treated as a plain
					// static resource.
					if href := rawAttr(tok, keyHREF); href != "" {
						cfg.HandleStyleSheet(atom.Link, href)
					}
				} else {
					isJS, isCSS = extractToken(base, tok, &key, cfg.HandleHTML)
				}
			}

		case html.TextToken:
			switch {
			case cfg.ScanJS && isJS:
				ExtractJS(bytes.NewReader(tkns.Text()), cfg.HandleStatic)
			case cfg.ScanCSS && isCSS && cfg.HandleCSS != nil:
				ExtractCSSReferences(bytes.NewReader(tkns.Text()), cfg.HandleCSS)
			case cfg.ScanCSS && isCSS:
				ExtractCSS(bytes.NewReader(tkns.Text()), cfg.HandleStatic)
			}

			isJS, isCSS = false, false

		case html.CommentToken:
			if cfg.Brute {
				extractComment(tkns.Token().Data, cfg.HandleHTML)
			}
		}
	}
}

func extractComment(s string, h HTMLHandler) {
	ss := bufio.NewScanner(strings.NewReader(s))
	ss.Split(bufio.ScanWords)

	const (
		prefixHTTP  = "http://"
		prefixHTTPS = "https://"
		endBytes    = `<(')>"`
	)

	var (
		buf, low []byte
		pos, end int
	)

	for ss.Scan() {
		buf = ss.Bytes()
		low = bytes.ToLower(buf)

		if pos = bytes.Index(low, []byte(prefixHTTP)); pos == -1 {
			pos = bytes.Index(low, []byte(prefixHTTPS))
		}

		if pos == -1 {
			continue
		}

		if end = bytes.IndexAny(low[pos:], endBytes); end > -1 {
			buf = buf[:pos+end]
		}

		if uri := bytes.TrimSpace(buf[pos:]); len(uri) > 0 {
			suri := string(uri)
			if u, err := url.Parse(suri); err == nil && u.Host != "" {
				h(atom.A, suri)
			}
		}
	}
}

func extractToken(
	base *url.URL,
	tok html.Token,
	key *string,
	handle HTMLHandler,
) (js, css bool) {
	var (
		poster string
		uri    string
	)

	switch tok.DataAtom {
	case atom.A, atom.Link:
		uri = extractTag(base, &tok, keyHREF)

	case atom.Img, atom.Image, atom.Iframe, atom.Track:
		uri = extractTag(base, &tok, keySRC)

	case atom.Script:
		uri = extractTag(base, &tok, keySRC)
		js = uri == ""

	case atom.Style:
		css = true

	case atom.Form:
		uri = extractTag(base, &tok, keyACTION)

	case atom.Object:
		uri = extractTag(base, &tok, keyDATA)

	case atom.Video:
		poster = extractTag(base, &tok, keyPOSTER)

		fallthrough

	case atom.Audio:
		*key = keySRC
		uri = extractTag(base, &tok, keySRC)

	case atom.Picture:
		*key = keySRCS

	case atom.Source:
		uri = extractTag(base, &tok, *key)
	}

	handleNotEmpty(handle, tok.DataAtom, uri)
	handleNotEmpty(handle, tok.DataAtom, poster)

	return js, css
}

func handleNotEmpty(h HTMLHandler, a atom.Atom, s string) {
	if s != "" {
		h(a, s)
	}
}

// rawAttr returns the raw value of a token attribute, or "" when absent.
func rawAttr(tok html.Token, key string) (rv string) {
	for i := 0; i < len(tok.Attr); i++ {
		if a := &tok.Attr[i]; a.Key == key {
			return a.Val
		}
	}

	return ""
}

// isStyleSheetLink reports whether a link token has a rel attribute whose
// ASCII case-insensitive, whitespace-separated tokens contain "stylesheet".
func isStyleSheetLink(tok html.Token) (yes bool) {
	for i := 0; i < len(tok.Attr); i++ {
		a := &tok.Attr[i]
		if !strings.EqualFold(a.Key, keyREL) {
			continue
		}

		for _, token := range strings.FieldsFunc(a.Val, isASCIISpace) {
			if strings.EqualFold(token, relStyleSheet) {
				return true
			}
		}
	}

	return false
}

func isASCIISpace(r rune) bool {
	switch r {
	case ' ', '\t', '\n', '\r', '\f':
		return true
	}

	return false
}

func extractTag(
	base *url.URL,
	tok *html.Token,
	key string,
) (rv string) {
	for i := 0; i < len(tok.Attr); i++ {
		if a := &tok.Attr[i]; a.Key == key {
			if res, ok := cleanURL(base, a.Val); ok {
				return res
			}
		}
	}

	return rv
}
