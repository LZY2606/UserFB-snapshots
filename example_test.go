package selenium_test

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/tebeka/selenium"
)

// This example shows how to navigate to a http://play.golang.org page, input a
// short program, run it, and inspect its output.
//
// If you want to actually run this example:
//
//   1. Ensure the file paths at the top of the function are correct.
//   2. Remove the word "Example" from the comment at the bottom of the
//      function.
//   3. Run:
//      go test -test.run=Example$ github.com/tebeka/selenium
func Example() {
	// Start a Selenium WebDriver server instance (if one is not already
	// running).
	const (
		// These paths will be different on your system.
		seleniumPath    = "vendor/selenium-server-standalone-3.4.jar"
		geckoDriverPath = "vendor/geckodriver-v0.18.0-linux64"
		port            = 8080
	)
	opts := []selenium.ServiceOption{
		selenium.StartFrameBuffer(),           // Start an X frame buffer for the browser to run in.
		selenium.GeckoDriver(geckoDriverPath), // Specify the path to GeckoDriver in order to use Firefox.
		selenium.Output(os.Stderr),            // Output debug information to STDERR.
	}
	selenium.SetDebug(true)
	service, err := selenium.NewSeleniumService(seleniumPath, port, opts...)
	if err != nil {
		panic(err) // panic is used only as an example and is not otherwise recommended.
	}
	defer service.Stop()

	// Connect to the WebDriver instance running locally.
	caps := selenium.Capabilities{"browserName": "firefox"}
	wd, err := selenium.NewRemote(caps, fmt.Sprintf("http://localhost:%d/wd/hub", port))
	if err != nil {
		panic(err)
	}
	defer wd.Quit()

	// Navigate to the simple playground interface.
	if err := wd.Get("http://play.golang.org/?simple=1"); err != nil {
		panic(err)
	}

	// Get a reference to the text box containing code.
	elem, err := wd.FindElement(selenium.ByCSSSelector, "#code")
	if err != nil {
		panic(err)
	}
	// Remove the boilerplate code already in the text box.
	if err := elem.Clear(); err != nil {
		panic(err)
	}

	// Enter some new code in text box.
	err = elem.SendKeys(`
		package main
		import "fmt"

		func main() {
			fmt.Println("Hello WebDriver!")
		}
	`)
	if err != nil {
		panic(err)
	}

	// Click the run button.
	btn, err := wd.FindElement(selenium.ByCSSSelector, "#run")
	if err != nil {
		panic(err)
	}
	if err := btn.Click(); err != nil {
		panic(err)
	}

	// Wait for the program to finish running and get the output.
	outputDiv, err := wd.FindElement(selenium.ByCSSSelector, "#output")
	if err != nil {
		panic(err)
	}

	var output string
	for {
		output, err = outputDiv.Text()
		if err != nil {
			panic(err)
		}
		if output != "Waiting for remote server..." {
			break
		}
		time.Sleep(time.Millisecond * 100)
	}

	fmt.Printf("%s", strings.Replace(output, "\n\n", "\n", -1))
	// Example Output:
	// Hello WebDriver!
	//
	// Program exited.

	// The following shows an example of using the Actions API.
	// Please refer to the WC3 Actions spec for more detailed information.
	if err := wd.Get("http://play.golang.org/?simple=1"); err != nil {
		panic(err)
	}

	// Create a point which will be used as an offset to click on the
	// code editor text box element on the page.
	offset := selenium.Point{X: 100, Y: 100}

	// Call StorePointerActions to store a number of Pointer actions which
	// will be executed sequentially.
	// "mouse1" is used as a unique virtual device identifier for this
	// and future actions.
	// selenium.MousePointer is used to identify the type of the pointer.
	// The stored action chain will move the pointer and click on the code
	// editor text box on the page.
	wd.StorePointerActions("mouse1",
		selenium.MousePointer,
		// using selenium.FromViewport as the move origin
		// which calculates the offset from 0,0.
		// the other valid option is selenium.FromPointer.
		selenium.PointerMoveAction(0, offset, selenium.FromViewport),
		selenium.PointerPauseAction(250),
		selenium.PointerDownAction(selenium.LeftButton),
		selenium.PointerPauseAction(250),
		selenium.PointerUpAction(selenium.LeftButton),
	)

	// Call StoreKeyActions to store a number of Key actions which
	// will be executed sequentially.
	// "keyboard1" is used as a unique virtual device identifier
	// for this and future actions.
	// The stored action chain will send keyboard inputs to the browser.
	wd.StoreKeyActions("keyboard1",
		selenium.KeyDownAction(selenium.ControlKey),
		selenium.KeyPauseAction(50),
		selenium.KeyDownAction("a"),
		selenium.KeyPauseAction(50),
		selenium.KeyUpAction("a"),
		selenium.KeyUpAction(selenium.ControlKey),
		selenium.KeyDownAction("h"),
		selenium.KeyDownAction("e"),
		selenium.KeyDownAction("l"),
		selenium.KeyDownAction("l"),
		selenium.KeyDownAction("o"),
	)

	// Call PerformActions to execute stored action - based on
	// the order of the previous calls, PointerActions will be
	// executed first and then KeyActions.
	if err := wd.PerformActions(); err != nil {
		panic(err)
	}

	// Call ReleaseActions to release any PointerDown or
	// KeyDown Actions that haven't been released through an Action.
	if err := wd.ReleaseActions(); err != nil {
		panic(err)
	}

}

// This example shows how to perform a two-finger pinch gesture on a map
// canvas while the Shift key is held down, using an action timeline.
//
// If you want to actually run this example, start a WebDriver server on
// port 4444 and serve a page with a <canvas id="map"> element.
func ExampleWebDriver_PerformActionTimeline() {
	// Connect to the WebDriver instance running locally.
	caps := selenium.Capabilities{"browserName": "chrome"}
	wd, err := selenium.NewRemote(caps, "http://localhost:4444/wd/hub")
	if err != nil {
		panic(err) // panic is used only as an example and is not otherwise recommended.
	}
	defer wd.Quit()

	// Get a reference to the map canvas the gesture is performed on.
	canvas, err := wd.FindElement(selenium.ByCSSSelector, "#map")
	if err != nil {
		panic(err)
	}

	tl := selenium.NewActionTimeline()

	// Hold Shift for the whole gesture.
	kbd := tl.AddKeyboard("kbd")
	kbd.KeyDown(0, selenium.ShiftKey)
	kbd.KeyUp(1200*time.Millisecond, selenium.ShiftKey)

	// Two touch contacts start next to each other at the center of the map
	// and move apart, zooming in. Positions are given relative to the
	// canvas rectangle: (0.5, 0.5) is the center of the canvas.
	finger1 := tl.AddPointer("finger-1", selenium.TouchPointer)
	finger1.Move(0, selenium.ElementPoint(canvas, 0.45, 0.5))
	finger1.Down(0, selenium.LeftButton)
	finger1.Move(time.Second, selenium.ElementPoint(canvas, 0.2, 0.5))
	finger1.Up(time.Second, selenium.LeftButton)

	finger2 := tl.AddPointer("finger-2", selenium.TouchPointer)
	finger2.Move(0, selenium.ElementPoint(canvas, 0.55, 0.5))
	finger2.Down(0, selenium.LeftButton)
	finger2.Move(time.Second, selenium.ElementPoint(canvas, 0.8, 0.5))
	finger2.Up(time.Second, selenium.LeftButton)

	// Compile the timeline into a single W3C Actions request and execute
	// it. The canvas rectangle is read once, right before the actions are
	// sent.
	if err := wd.PerformActionTimeline(tl); err != nil {
		panic(err)
	}
}
