# Legacy Encodings

Older sites do not always serve UTF-8. A Windows-1252 page, for example,
stores `café` as the raw byte `0xe9`; reading those bytes as UTF-8 produces
garbage, while a form filled with a proper Unicode `café` must be sent back
encoded as Windows-1252 rather than UTF-8. Surf now connects document
decoding and form submission so both directions use the same charset.

## Supported character sets

Surf currently supports three encodings and their case-insensitive aliases:

| Canonical name     | Also accepted                 |
|--------------------|-------------------------------|
| `utf-8`            | `UTF8`                        |
| `windows-1252`     | `latin1`, `ISO-8859-1`        |
| `shift_jis`        | `SJIS`                        |

The document charset is selected from the decompressed response bytes in
this order:

1. a UTF-8 byte order mark (the BOM is stripped and never enters the DOM),
2. a valid `charset` in the HTTP `Content-Type` header,
3. a valid `<meta charset>` or `http-equiv="content-type"` declaration
   within the first 1024 entity bytes,
4. otherwise UTF-8.

Declarations that name an unsupported charset are ignored and the search
continues, so a bogus header or meta tag does not stop a later valid one
from being used.

## Reading a legacy page

After a page is opened, `DocumentCharset()` returns the canonical charset
name (or `utf-8` before any page has been opened), and the DOM holds
decoded Unicode text. `Download()` still returns the decompressed original
bytes in the page's own encoding.

```go
bow := surf.NewBrowser()
if err := bow.Open("http://legacy.example/order.html"); err != nil {
	panic(err)
}

// "utf-8", "windows-1252" or "shift_jis"
fmt.Println(bow.DocumentCharset())

// Title and form values are Unicode regardless of the page encoding.
fmt.Println(bow.Title())
```

## Submitting a form in the document charset

A form remembers the charset of the document it came from. Field names,
ordinary values and the clicked button value have their line endings
normalized to CRLF and are then transcoded before the request is sent.
Runes the target charset cannot represent are emitted as decimal numeric
character references, so `😀` becomes `&#128512;`.

```go
form, err := bow.Form("form")
if err != nil {
	panic(err)
}
if err := form.Input("q", "café"); err != nil {
	panic(err)
}
if err := form.Submit(); err != nil {
	panic(err)
}
```

For a Windows-1252 document the body is encoded as Windows-1252 and
percent-encoded on the transcoded bytes, e.g. `q=caf%E9` — it is never the
UTF-8 form `q=caf%C3%A9`, and the Unicode strings are never handed directly
to `url.Values.Encode`.

### accept-charset

The submission charset is the first supported entry of the form's
whitespace separated `accept-charset` list. When no entry is supported the
captured document charset is used instead.

```html
<form accept-charset="bogus UTF8">...</form>
```

The unknown `bogus` label is skipped and the form is submitted as UTF-8.
The charset choice is fixed when the form is created: navigating the
browser to another page afterwards does not change it, and forms owned by
different browsers never affect each other.

### The `_charset_` control

When every successful control named `_charset_` is a hidden input, Surf
substitutes the canonical name of the chosen charset for its value. The
substitution only affects the request — `Form.Get("_charset_")` keeps
returning the original value. If any `_charset_` control is not hidden, the
whole group is submitted as ordinary fields instead.

### multipart/form-data

Ordinary multipart text fields follow the same transcoding, CRLF and
numeric-reference rules. Attachment contents and file names keep their
existing byte handling and are not re-encoded.

## History and compatibility

`Back()` restores a visited page together with its charset. The `Browsable`
interface is unchanged: a custom implementation without encoding
information is treated as UTF-8 and its forms use the original UTF-8
submission path. Direct `Browser.OpenForm`, `Browser.PostForm` and
`Browser.PostMultipart` calls also keep their existing UTF-8 convention —
charset conversion only applies to forms obtained from a decoded page.
