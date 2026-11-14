This project is not maintained and it is effectively abandoned, if you have interest in this code you should probably clone it.

webdriver
=========

The package implements a WebDriver that communicate with a browser using the JSON Wire Protocol (See https://code.google.com/p/selenium/wiki/JsonWireProtocol).
This is a pure go library and doesn't require a running Selenium driver. It currently supports Firefox (using the WebDriver extension) and Chrome (using the standalone server chromedriver). It should be fairly easy to add other browser that directly implement the wire protocol.

**Version: 0.1**  
Tests are partial and have been run only on Linux (with firefox webdriver 2.32.0 and chromedriver 2.1).

**Install:**  
$ go get github.com/fedesog/webdriver

**Requires:**
* chromedriver (for chrome):  
https://code.google.com/p/chromedriver/  
* webdriver.xpi (for firefox): that is founds in the selenium-server-standalone file  
https://code.google.com/p/selenium/


Example:
--------

    chromeDriver := webdriver.NewChromeDriver("/path/to/chromedriver")
    err := chromeDriver.Start()
    if err != nil {
    	log.Println(err)
    }
    desired := webdriver.Capabilities{"Platform": "Linux"}
    required := webdriver.Capabilities{}
    session, err := chromeDriver.NewSession(desired, required)
    if err != nil {
    	log.Println(err)
    }
    err = session.Url("http://golang.org")
    if err != nil {
    	log.Println(err)
    }
    time.Sleep(10 * time.Second)
    session.Delete()
    chromeDriver.Stop()

W3C input leases
----------------

A stateful W3C interaction (a held modifier key, a drag, a multi-part key
sequence) spans several remote actions. When two goroutines share one session
their actions can interleave and corrupt each other's input state, and an HTTP
timeout does not prove that the browser did not press the key. The input lease
gives one caller exclusive ownership of the session W3C input state and makes
an uncertain result explicit instead of guessing.

The API is available on sessions whose server implements the W3C
`/session/:id/actions` endpoint; session creation is unchanged:

    lease, err := session.AcquireInputs(ctx)
    if err != nil {
    	// context.Canceled while queued, ErrInputUncertain, or
    	// ErrInputLeaseClosed on a deleted session generation
    	return err
    }
    // actions is the raw W3C sources array; it is wrapped as {"actions": ...}
    // and POSTed to /session/:id/actions verbatim.
    err = lease.Perform(ctx, actions)
    ...
    // DELETE /session/:id/actions releases the remote input state and opens
    // the latch for the next caller.
    err = lease.Release(ctx)

Contract:

* Only one live lease exists per real session. All value copies of the same
  `Session`, including views returned by `Sessions()`, share one latch;
  different drivers (and session generations recreated after `Delete`) are
  independent. `AcquireInputs` blocks until the latch is open or a reset
  finishes, honors the context while waiting, sends no remote command itself,
  and can never be taken by an already canceled context.
* `Perform` and `Release` on one lease run serially in arrival order and honor
  the context while queued locally. Validation failures (null, an empty array,
  malformed or non-array JSON) are returned before any packet is sent and leave
  the lease usable; the contents of individual sources are checked remotely.
* Once a `Perform` request is actually on the wire, any error or context
  cancellation means the remote outcome is unknown. The request is sent
  exactly once: there is no retry, no prefix guessing and no automatic
  compensation. The lease is retired, the session is quarantined, queued
  callers receive `ErrInputUncertain`, and the original HTTP, context or
  `CommandError` cause is returned unchanged.
* The legacy `Click`, `SendKeys`, `ButtonDown` and similar entry points are
  unchanged and are not gated by the lease; the lease cannot stop a caller that
  bypasses it.

Recovering after a timeout or uncertain result:

    if err := lease.Perform(ctx, actions); err != nil {
    	// The lease is already retired and the session is quarantined.
    	// Do not send more input into an unknown keyboard/mouse state.
    	for {
    		err := session.ResetInputs(ctx) // DELETE /session/:id/actions
    		if err == nil {
    			break // quarantine cleared; a new lease is admissible
    		}
    		if errors.Is(err, ErrInputBusy) {
    			// another caller still holds a live lease; do not preempt it
    			return err
    		}
    		// the reset itself failed: quarantine is preserved, back off and
    		// retry the explicit remote reset
    		time.Sleep(backoff)
    	}
    }
    lease, err = session.AcquireInputs(ctx) // real next lease, not a flag check

`ResetInputs` returns `ErrInputBusy` while a live lease exists instead of
preempting it. Resets run exclusively: concurrent resets queue and
`AcquireInputs` waits until the remote DELETE fully completes. A successful
reset clears quarantine; a failed reset keeps it, so input stays stopped until
recovery is confirmed by an explicit successful reset.

