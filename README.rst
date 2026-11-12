What
====

This is a robots.txt exclusion protocol implementation for Go language (golang).


Build
=====

To build and run tests run `go test` in source directory.


Contribute
==========

Warm welcome.

* If desired, add your name in README.rst, section Who.
* Run `script/test && script/clean && echo ok`
* You can ignore linter warnings, but everything else must pass.
* Send your change as pull request or just a regular patch to current maintainer (see section Who).

Thank you.


Usage
=====

As usual, no special installation is required, just

    import "github.com/temoto/robotstxt"

run `go get` and you're ready.

1. Parse
^^^^^^^^

First of all, you need to parse robots.txt data. You can do it with
functions `FromBytes(body []byte) (*RobotsData, error)` or same for `string`::

    robots, err := robotstxt.FromBytes([]byte("User-agent: *\nDisallow:"))
    robots, err := robotstxt.FromString("User-agent: *\nDisallow:")

As of 2012-10-03, `FromBytes` is the most efficient method, everything else
is a wrapper for this core function.

There are few convenient constructors for various purposes:

* `FromResponse(*http.Response) (*RobotsData, error)` to init robots data
from HTTP response. It *does not* call `response.Body.Close()`::

    robots, err := robotstxt.FromResponse(resp)
    resp.Body.Close()
    if err != nil {
        log.Println("Error parsing robots.txt:", err.Error())
    }

* `FromStatusAndBytes(statusCode int, body []byte) (*RobotsData, error)` or
`FromStatusAndString` if you prefer to read bytes (string) yourself.
Passing status code applies following logic in line with Google's interpretation
of robots.txt files:

    * status 2xx  -> parse body with `FromBytes` and apply rules listed there.
    * status 4xx  -> allow all (even 401/403, as recommended by Google).
    * other (5xx) -> disallow all, consider this a temporary unavailability.

2. Query
^^^^^^^^

Parsing robots.txt content builds a kind of logic database, which you can
query with `(r *RobotsData) TestAgent(url, agent string) (bool)`.

Explicit passing of agent is useful if you want to query for different agents. For
single agent users there is an efficient option: `RobotsData.FindGroup(userAgent string)`
returns a structure with `.Test(path string)` method and `.CrawlDelay time.Duration`.

Simple query with explicit user agent. Each call will scan all rules.

::

    allow := robots.TestAgent("/", "FooBot")

Or query several paths against same user agent for performance.

::

    group := robots.FindGroup("BarBot")
    group.Test("/")
    group.Test("/download.mp3")
    group.Test("/news/article-2012-1")

3. Compiled URL matcher
^^^^^^^^^^^^^^^^^^^^^^^

Crawlers that test many URLs against the same group can compile the group's
rules once and reuse the result. Rules that share a normalized prefix also
share their matching progress, so a single query costs proportionally to the
request path length, not to the number of rules. The compiled matcher is
immutable and safe for concurrent queries.

::

    group := robots.FindGroup("BarBot")
    matcher, err := group.CompileURLMatcher()
    if err != nil {
        log.Println("Error compiling rules:", err.Error())
    }
    allow, err := matcher.Test("https://example.com/news/article-2012-1?ref=rss")

`URLMatcher.Test` takes a full absolute `http`/`https` URL with a host and
returns an error for anything else (relative URLs, other schemes, invalid
percent-encodings). The matched string is the escaped path (an empty path
becomes `/`) plus the query when present; a present-but-empty query keeps
its `?`, and the fragment never participates. Dot segments are not cleaned
and query parameters are neither decoded nor reordered.

Differences from the legacy `Group.Test` / `TestAgent` entries, which are
unchanged and keep operating on raw path strings:

* Rules always match from the first byte of the path.
* A bare `*` matches zero or more octets. A bare `$` is an end-of-URL
  marker only as the last byte of a rule; elsewhere it is a literal `$`.
* Percent-encoded octets are single matching units: `%2A` and `%24` are
  always literal `*` and `$`, never operators, and `*` can never swallow
  half of an encoded octet. Rules with malformed percent-encodings fail
  at compile time; the legacy queries of the same group still work.
* Encoded and raw forms of the same octet are equivalent (hex case is
  normalized, so rule `/café` matches `/caf%C3%A9`), except that an
  encoded slash `%2F` is not a path separator and an encoded `%3F` does
  not start the query.
* Among matching rules, the one whose literal part covers the most octets
  wins (`*` and the end marker do not count); ties are resolved in favour
  of `Allow`. Rule order never changes the outcome. When nothing matches,
  crawling is allowed.


Who
===

Honorable contributors (in undefined order):

    * Ilya Grigorik (igrigorik)
    * Martin Angers (PuerkitoBio)
    * Micha Gorelick (mynameisfiber)

Initial commit and other: Sergey Shepelev temotor@gmail.com


Flair
=====

.. image:: https://travis-ci.org/temoto/robotstxt.svg?branch=master
    :target: https://travis-ci.org/temoto/robotstxt

.. image:: https://codecov.io/gh/temoto/robotstxt/branch/master/graph/badge.svg
    :target: https://codecov.io/gh/temoto/robotstxt

.. image:: https://goreportcard.com/badge/github.com/temoto/robotstxt
    :target: https://goreportcard.com/report/github.com/temoto/robotstxt
