[![Go Reference](https://pkg.go.dev/badge/marionette_client.svg)](https://pkg.go.dev/github.com/njasm/marionette_client)
[![CI](https://github.com/njasm/marionette_client/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/njasm/marionette_client/actions/workflows/ci.yml)
[![Coverage Status](https://coveralls.io/repos/github/njasm/marionette_client/badge.svg?branch=master)](https://coveralls.io/github/njasm/marionette_client?branch=master)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://choosealicense.com/licenses/mit/)

# marionette_client
Mozilla Gecko Marionette client for Go.

## What is Marionette
Marionette is an automation driver for Mozilla's Gecko engine. It can remotely control either the UI or the internal
JavaScript of a Gecko application such as Firefox. It can control both the chrome (menus and browser functions) and the
content loaded in a browsing context, allowing callers to reproduce user actions and inspect the DOM.

Marionette shares much of its API with WebDriver and adds commands for interacting with Gecko's chrome interface.

## Resources
- [Marionette documentation](https://firefox-source-docs.mozilla.org/testing/marionette/)
- [W3C WebDriver specification](https://w3c.github.io/webdriver/)
- See [Marionette protocol support](PROTOCOL_SUPPORT.md) for the current command inventory and implementation gaps.

## Examples
This is an incomplete list. See the tests for more examples.

#### Instantiate the client
```go
client := NewClient()
// An empty host and zero port use Marionette's defaults.
client.Connect("", 0)
// Let Marionette generate the session ID with its default capabilities.
client.NewSession("", nil)
```

#### Navigate to page
```go
client.Navigate("http://localhost:8080/")
```

#### Perform mouse actions
```go
import (
	"time"

	marionette "github.com/njasm/marionette_client"
)

target, err := client.FindElement(marionette.Id, "button-id")
if err != nil {
	return err
}

_, err = client.PerformActions(marionette.MouseActions("mouse",
	marionette.PointerMove(0, 0, 100*time.Millisecond, marionette.ElementOrigin(target)),
	marionette.PointerDown(0),
	marionette.Pause(50*time.Millisecond),
	marionette.PointerUp(0),
))
if err != nil {
	return err
}

// Release any depressed buttons and clear Firefox's stored input-source state.
if _, err = client.ReleaseActions(); err != nil {
	return err
}
```

The example moves the mouse to the center of `target`, presses the primary button, pauses, and releases it. Pointer
moves can also use `ViewportOrigin()` for viewport-relative coordinates or `PointerOriginCurrent()` for coordinates
relative to the current pointer position. `PerformActions` validates source IDs, origins, durations, buttons, and empty
sequences before sending `WebDriver:PerformActions` to Firefox.
`ReleaseActions` sends `WebDriver:ReleaseActions`, which releases any depressed buttons and clears all stored input
sources. It is useful for cleanup after a sequence fails before its matching button release.

#### Send special keys
WebDriver special keys are exported as string constants, so they can be mixed with ordinary text. `KeyNull` releases
all modifiers in an element key sequence.

```go
input, err := client.FindElement(marionette.Id, "search")
if err != nil {
	return err
}

// Select all text, release Control, and delete the selection.
if err = input.SendKeys(marionette.KeyControl, "a", marionette.KeyNull, marionette.KeyBackspace); err != nil {
	return err
}
```

The constants cover the complete WebDriver key set exposed by Selenium's Python `Keys`, including modifiers, arrows,
navigation and editing keys, number-pad keys, `F1` through `F12`, Meta/Command, right-side modifiers, and aliases.

Keyboard sources can also be synchronized with other sources through `PerformActions`:

```go
_, err = client.PerformActions(marionette.KeyboardActions("keyboard",
	marionette.KeyDownAction(marionette.KeyControl),
	marionette.KeyDownAction("a"),
	marionette.KeyUpAction("a"),
	marionette.KeyUpAction(marionette.KeyControl),
))
```

#### Change Contexts
```go
client.SetContext(Context(CHROME))
// or
client.SetContext(Context(CONTENT))
	
```

#### Find Element
```go
element, err := client.FindElement(Id, "html-element-id-attribute")
if err != nil {
	// handle your errors
}

// else
println(element.Id())
println(element.Enabled())
println(element.Selected())
println(element.Displayed())
println(element.TagName())
println(element.Text())
println(element.Attribute("id"))
println(element.Property("id"))
println(element.CssValue("text-decoration"))
	
// width, height, x and y
rect, err := element.Rect()
if err != nil {
    // handle your errors
}

fmt.Printf("%#v", rect)
	
// size
size, err := element.Size()
if err != nil {
	// handle your errors
}

fmt.Printf("width: %f, height: %f", size.Width, size.Height)

//location
point, err := element.Location()
if err != nil {
    // handle your errors
}

fmt.Printf("x: %v, y: %v", point.X, point.Y)
```

#### Find Elements
```go
collection, err := element.FindElements(TagName, "li")
if err != nil {
	// handle your errors
}

// else
for _, e := range collection {
	println(e.Id())
   	println(e.Enabled())
   	println(e.Selected())
   	println(e.Displayed())
   	println(e.TagName())
   	println(e.Text())
   	println(e.Attribute("id"))
   	println(e.CssValue("text-decoration"))
   	e.Click()
}
```

#### Shadow DOM
```go
// Get the shadow root of a custom element
host, err := client.FindElement(marionette.CssSelector, "my-custom-element")
if err != nil {
	return err
}

shadowRoot, err := host.GetShadowRoot()
if err != nil {
	return err
}

// Find elements inside the shadow root
button, err := client.FindElementFromShadowRoot(shadowRoot.Id(), marionette.CssSelector, "button")
if err != nil {
	return err
}

items, err := client.FindElementsFromShadowRoot(shadowRoot.Id(), marionette.CssSelector, "li")
if err != nil {
	return err
}
```

#### Computed accessibility properties
```go
element, err := client.FindElement(marionette.CssSelector, "[role='button']")
if err != nil {
	return err
}

label := element.ComputedLabel()
role := element.ComputedRole()
fmt.Printf("accessible label: %s, role: %s\n", label, role)
```

#### Print to PDF
```go
pdf, err := client.Print(map[string]any{
	"orientation": "landscape",
	"scale":       0.8,
	"background":  true,
})
if err != nil {
	return err
}

// pdf is a base64-encoded string
decoded, err := base64.StdEncoding.DecodeString(pdf)
if err != nil {
	return err
}

if err = os.WriteFile("page.pdf", decoded, 0644); err != nil {
	return err
}
```

#### Execute JS Script
```go
script := "function mySum(a, b) { return a + b; }; return mySum(arguments[0], arguments[1]);"
args := []int{1, 3} // arguments to be passed to the function
timeout := 1000     // milliseconds
sandbox := false    // new Sandbox
r, err := client.ExecuteScript(script, args, timeout, sandbox)
if err == nil {
    println(r.Value) // 4 
}
```

#### Wait(), Until() Expected condition is true.
```go
client.Navigate("http://localhost:8080/content-loaded-later.html")

timeout := 10 * time.Second
condition := ElementIsPresent(Id, "loaded-element")
ok, webElement, err := Wait(client).For(timeout).Until(condition)

if !ok {
	log.Printf("%#v", err)
	// do your error stuff
	return
}

// cool, we've the element, let's click on it!
webElement.Click()
```

#### Connection integrity and reconnect
Commands share one transport and run as complete, serialized request/response
transactions. Every reply is matched to its command's message ID, so callers
reading the title, URL or sending input concurrently never receive another
command's response. Each command is written at most once; there is no
background multiplexing and input actions are never re-sent.

```go
import (
	"errors"
	"fmt"
	"log"

	marionette "github.com/njasm/marionette_client"
)

transport := &marionette.MarionetteTransport{}
client := marionette.NewClient()
client.Transport(transport)

// Connect succeeds only after a complete gecko/protocol-version-3 handshake.
if err := client.Connect("127.0.0.1", 2828); err != nil {
	log.Fatalf("connect failed: %v", err)
}

title, err := client.Title()
if err != nil {
	var driverErr *marionette.DriverError
	if errors.As(err, &driverErr) {
		// A well-formed driver error is a normal reply and leaves the
		// connection usable for the next command.
		fmt.Println("driver reported:", driverErr.ErrorType)
	} else {
		// A corrupted frame, wrong message ID or I/O failure discards the
		// connection. Reconnect before issuing further commands; the new
		// connection numbers its messages from 1 again.
		if err = client.Connect("127.0.0.1", 2828); err != nil {
			log.Fatalf("reconnect failed: %v", err)
		}
	}
} else {
	fmt.Println(title)
}

// Close is idempotent, interrupts any command blocked waiting on a reply
// (without waiting for the read timeout), and can be followed by Connect again.
_ = transport.Close()
```

## Running tests

Run the complete suite with:

```sh
make test
```

The integration test harness starts its own headless Firefox instance; do not start Firefox or reserve Marionette port
`2828` beforehand. For each complete test run, the harness creates a fresh temporary profile, supplies that profile to
Firefox with `--profile`, and launches it with `--headless`, `--marionette`, `--remote-allow-system-access`,
`--no-remote`, and `--new-instance`. It waits for Marionette to become ready and stops Firefox during test cleanup.

By default, the harness runs `firefox` from `PATH`. Set `FIREFOX_BIN` to use a specific executable and optionally set
`FIREFOX_VERSION` to require a matching version:

```sh
FIREFOX_BIN=/path/to/firefox FIREFOX_VERSION=155.0.1 make test
```

This automatic Firefox lifecycle applies only to the repository's tests. Applications using the library must start or
otherwise provide a Marionette-enabled Firefox instance before calling `Client.Connect`.
