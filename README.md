# The most complete, best-tested WebDriver client for Go

[![GoDoc](https://godoc.org/github.com/tebeka/selenium?status.svg)](https://godoc.org/github.com/tebeka/selenium)
[![Travis](https://travis-ci.org/tebeka/selenium.svg?branch=master)](https://travis-ci.org/tebeka/selenium)
[![Go Report Card](https://goreportcard.com/badge/github.com/tebeka/selenium)](https://goreportcard.com/report/github.com/tebeka/selenium)

## About

This is a [WebDriver][selenium] client for [Go][go]. It supports the
[WebDriver protocol][webdriver] and has been tested with various versions of
[Selenium WebDriver][selenium], Firefox and [Geckodriver][geckodriver], and
Chrome and [ChromeDriver][chromedriver],

`selenium` is currently maintained by Eric Garrido ([@minusnine][minusnine]).

[selenium]: http://seleniumhq.org/
[webdriver]: https://www.w3.org/TR/webdriver/
[go]: http://golang.org/
[server]: http://seleniumhq.org/download/
[geckodriver]: https://github.com/mozilla/geckodriver
[chromedriver]: https://sites.google.com/a/chromium.org/chromedriver/
[minusnine]: http://github.com/minusnine

## Installing

Run

    go get -t -d github.com/tebeka/selenium

to fetch the package.

The package requires a working WebDriver installation, which can include recent
versions of a web browser being driven by Selenium WebDriver.

### Downloading Dependencies

We provide a means to download the ChromeDriver binary, the Firefox binary, the
Selenium WebDriver JARs, and the Sauce Connect proxy binary. This is primarily
intended for testing.

    $ cd vendor
    $ go run init.go --alsologtostderr  --download_browsers --download_latest
    $ cd ..

Re-run this periodically to get up-to-date versions of these binaries.

## Documentation

The API documentation is at https://godoc.org/github.com/tebeka/selenium. See [the example](https://github.com/tebeka/selenium/blob/master/example_test.go) and unit tests(for [sauce](https://github.com/tebeka/selenium/blob/master/sauce_test.go), [selenium](https://github.com/tebeka/selenium/blob/master/selenium_test.go) and [service](https://github.com/tebeka/selenium/blob/master/service_test.go)) for better usage information.

## Downloading files from a Selenium Grid

When the browser runs on a remote Grid node, a file saved by the browser stays
on that node. Selenium Grid can expose these downloads over the WebDriver
protocol: start the Grid with downloads management enabled and request the
`se:downloadsEnabled` capability when creating the session.

```go
caps := selenium.Capabilities{
    "browserName":         "chrome",
    "se:downloadsEnabled": true,
}
wd, err := selenium.NewRemote(caps, "http://grid.example:4444")
```

After the application page confirms that an export has finished, call
`DownloadSnapshot`. It lists the session's downloads once, downloads each one
in order as a base64-encoded ZIP archive, validates and extracts the archives
into a private staging directory, and renames that directory to the
destination only after the whole batch succeeds. On failure nothing is
published and the staging directory is removed. A snapshot never polls, never
replays the export action and never deletes remote files.

```go
files, err := wd.DownloadSnapshot("/var/lib/reports/2026-11-14", selenium.DownloadLimits{
    MaxArchiveBytes:  50 << 20,  // maximum decoded size of one ZIP archive
    MaxExpandedBytes: 200 << 20, // maximum total extracted file bytes
    MaxFiles:         1000,      // maximum total number of regular files
})
```

All limits must be positive; reaching a limit exactly is allowed. The
destination must be an absolute path whose parent directory exists, and the
destination itself must not exist (symlinks included). Entry names are
validated as strict ZIP slash paths — absolute paths, backslashes, volume
prefixes, NUL bytes, `.`/`..` or empty segments, and non-regular entry types
are rejected — and the shared batch namespace rejects duplicate files and
file/directory conflicts. Published files use mode `0600` and directories
`0700`; archive permissions and owners are not restored. Each returned
`DownloadedFile` records the opaque remote name, the absolute published local
path and the number of bytes actually extracted.

## Known Issues

Any issues are usually because the underlying browser automation framework has a
bug or inconsistency. Where possible, we try to cover up these underlying
problems in the client, but sometimes workarounds require higher-level
intervention.

Please feel free to [file an issue][issue] if this client doesn't work as
expected.

[issue]: https://github.com/tebeka/selenium/issues/new

Below are known issues that affect the usage of this API. There are likely
others filed on the respective issue trackers.

### Selenium 2

No longer supported.

### Selenium 3

1.  [Selenium 3 NewSession does not implement the W3C-specified parameters](https://github.com/SeleniumHQ/selenium/issues/2827).

### Geckodriver (Standalone)

1.  [Geckodriver does not support the Log API](https://github.com/mozilla/geckodriver/issues/284)
    because it
    [hasn't been defined in the spec yet](https://github.com/w3c/webdriver/issues/406).
2.  Firefox via Geckodriver (and also through Selenium)
    [doesn't handle clicking on an element](https://github.com/mozilla/geckodriver/issues/1007).
3.  Firefox via Geckodriver doesn't handle sending control characters
    [without appending a terminating null key](https://github.com/mozilla/geckodriver/issues/665).

### Chromedriver

1. [Headless Chrome does not support running extensions](https://crbug.com/706008).

## Breaking Changes

There are a number of upcoming changes that break backward compatibility in an
effort to improve and adapt the existing API. They are listed here:

### 22 August 2017

The `Version` constant was removed as it is unused.

### 18 April 2017

The Log method was changed to accept a typed constant for the type of log to
retrieve, instead of a raw string. The return value was also changed to provide
a more idiomatic type.

## Hacking

Patches are encouraged through GitHub pull requests. Please ensure that:

1.  A test is added for anything more than a trivial change and that the
    existing tests pass. See below for instructions on setting up your test
    environment.
2.  Please ensure that `gofmt` has been run on the changed files before
    committing. Install a pre-commit hook with the following command:

    $ ln -s ../../misc/git/pre-commit .git/hooks/pre-commit

See [the issue tracker][issues] for features that need implementing.

[issues]: https://github.com/tebeka/selenium/issues

### Testing Locally

Install `xvfb` and Java if they is not already installed, e.g.:

    sudo apt-get install xvfb openjdk-11-jre

Run the tests:

    $ go test

*   There is one top-level test for each of:

    1.  Chromium and ChromeDriver.
    2.  A new version of Firefox and Selenium 3.
    3.  HTMLUnit, a Java-based lightweight headless browser implementation.
    4.  A new version of Firefox directly against Geckodriver.

    There are subtests that are shared between both top-level tests.

*   To run only one of the top-level tests, pass one of:

    *   `-test.run=TestFirefoxSelenium3`,
    *   `-test.run=TestFirefoxGeckoDriver`,
    *   `-test.run=TestHTMLUnit`, or
    *   `-test.run=TestChrome`.

    To run a specific subtest, pass `-test.run=Test<Browser>/<subtest>` as
    appropriate. This flag supports regular expressions.

*   If the Chrome or Firefox binaries, the Selenium JAR, the Geckodriver binary,
    or the ChromeDriver binary cannot be found, the corresponding tests will be
    skipped.

*   The binaries and JAR under test can be configured by passing flags to `go
    test`. See the available flags with `go test --arg --help`.

*   Add the argument `-test.v` to see detailed output from the test automation
    framework.

### Testing With Docker

To ensure hermeticity, we also have tests that run under Docker. You will need
an installed and running Docker system.

To run the tests under Docker, run:

    $ go test --docker

This will create a new Docker container and run the tests in it. (Note: flags
supplied to this invocation are not curried through to the `go test` invocation
within the Docker container).

For debugging Docker directly, run the following commands:

    $ docker build -t go-selenium testing/
    $ docker run --volume=$(pwd):/code --workdir=/code -it go-selenium bash
    root@6c7951e41db6:/code# testing/docker-test.sh
    ... lots of testing output ...

### Testing With Sauce Labs

Tests can be run using a browser located in the cloud via Sauce Labs.

To run the tests under Sauce, run:

    $ go test --test.run=TestSauce --test.timeout=20m \
      --experimental_enable_sauce \
      --sauce_user_name=[username goes here] \
      --sauce_access_key=[access key goes here]

The Sauce access key can be obtained via
[the Sauce Labs user settings page](https://saucelabs.com/beta/user-settings).

Test results can be viewed through the
[Sauce Labs Dashboard](https://saucelabs.com/beta/dashboard/tests).

## License

This project is licensed under the [MIT][mit] license.

[mit]: https://raw.githubusercontent.com/tebeka/selenium/master/LICENSE
