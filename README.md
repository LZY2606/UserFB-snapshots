# mpd-parser

[![Build Status](https://travis-ci.org/videojs/mpd-parser.svg?branch=master)](https://travis-ci.org/videojs/mpd-parser)
[![Greenkeeper badge](https://badges.greenkeeper.io/videojs/mpd-parser.svg)](https://greenkeeper.io/)
[![Slack Status](http://slack.videojs.com/badge.svg)](http://slack.videojs.com)

[![NPM](https://nodei.co/npm/mpd-parser.png?downloads=true&downloadRank=true)](https://nodei.co/npm/mpd-parser/)

mpd parser

## Table of Contents

<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Installation](#installation)
- [Usage](#usage)
  - [Parsed Output](#parsed-output)
  - [SegmentTimeline Timing](#segmenttimeline-timing)
- [Including the Parser](#including-the-parser)
  - [`<script>` Tag](#script-tag)
  - [Browserify](#browserify)
  - [RequireJS/AMD](#requirejsamd)
- [License](#license)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->
## Installation

```sh
npm install --save mpd-parser
```

## Usage

```js
// get your manifest in whatever way works best
// for example, by reading the file from the filesystem in node
// or using fetch in a browser like so:

const manifestUri = 'https://example.com/dash.xml';
const res = await fetch(manifestUri);
const manifest = await res.text();

// A callback function to handle events like errors or warnings
const eventHandler = ({ type, message }) => console.log(`${type}: ${message}`);

var parsedManifest = mpdParser.parse(manifest, { manifestUri, eventHandler });
```

If dealing with a live stream, then on subsequent calls to parse, the previously parsed
manifest object should be provided as an option to `parse` using the `previousManifest`
option:

```js
const newParsedManifest = mpdParser.parse(
  manifest,
  // parsedManifest comes from the prior example
  { manifestUri, previousManifest: parsedManifest }
);
```

### Parsed Output

The parser ouputs a plain javascript object with the following structure:

```js
Manifest {
  allowCache: boolean,
  contentSteering: {
    defaultServiceLocation: string,
    proxyServerURL: string,
    queryBeforeStart: boolean,
    serverURL: string
  },
  endList: boolean,
  mediaSequence: number,
  discontinuitySequence: number,
  playlistType: string,
  playlists: [
    {
      attributes: {},
      Manifest
    }
  ],
  mediaGroups: {
    AUDIO: {
      'GROUP-ID': {
        default: boolean,
        autoselect: boolean,
        language: string,
        uri: string,
        instreamId: string,
        characteristics: string,
        forced: boolean
      }
    },
    VIDEO: {},
    'CLOSED-CAPTIONS': {},
    SUBTITLES: {}
  },
  dateTimeString: string,
  dateTimeObject: Date,
  targetDuration: number,
  totalDuration: number,
  discontinuityStarts: [number],
  segments: [
    {
      byterange: {
        length: number,
        offset: number
      },
      duration: number,
      attributes: {},
      discontinuity: number,
      uri: string,
      timeline: number,
      key: {
        method: string,
        uri: string,
        iv: string
      },
      map: {
        uri: string,
        byterange: {
          length: number,
          offset: number
        }
      },
      'cue-out': string,
      'cue-out-cont': string,
      'cue-in': string
    }
  ]
}
```

### SegmentTimeline Timing

A `SegmentTimeline` describes media segments on the Representation's **media
clock**, not on the presentation clock used by `Period@start` and
`mediaPresentationDuration`:

- `S@t` and `S@d` are expressed in `@timescale` units, and a `$Time$`
  template identifier is substituted with the segment's media time. These
  values are never converted to presentation seconds inside the segment URL.
- `presentationTimeOffset` (`@presentationTimeOffset`) only participates in
  the conversion to the presentation-facing `presentationTime`:

  ```
  presentationTime = periodStart + (S@t - presentationTimeOffset) / timescale
  ```

  It does not change the media time origin or the value used in `$Time$`.

For a `static` MPD, a trailing `S` with `r="-1"` is expanded up to the end of
the Period that contains it. The Period end is taken from, in order:

1. the Period's own `@duration` (`@start + @duration`),
2. the next Period's `@start` when the Period has no `@duration`,
3. `mediaPresentationDuration` for the final Period.

The boundary is converted from presentation seconds onto the media clock
(`presentationTimeOffset + (periodEnd - periodStart) * timescale`) before the
repeat group is expanded. Only segments whose media start is strictly before
the Period end are emitted, so no extra resource is produced at the end
boundary itself. A final segment whose start does not evenly divide the
remaining duration is still emitted with its full original `S@d`; the parser
does not rewrite that duration. If the repeat group is followed by another `S`
with an explicit `@t`, that `@t` terminates the group instead of the Period
end. These rules apply identically to `SegmentTemplate` and `SegmentList`.

`dynamic` MPDs are unaffected: their availability-window rules continue to
determine how a trailing `r="-1"` expands.

Manifests that cannot be expanded safely throw a named error rather than
returning an empty segment list:

- `PERIOD_END_UNSPECIFIED`: a trailing `r="-1"` in a static MPD with no
  derivable Period end,
- `SEGMENT_DURATION_INVALID`: an `S@d` that is missing or not positive,
- `TIMESCALE_INVALID`: a non-positive `@timescale`,
- `SEGMENT_TIME_UNSAFE_INTEGER`: a time, duration or repeat count that cannot
  be represented as a safe integer.

## Including the Parser

To include mpd-parser on your website or web application, use any of the following methods.

### `<script>` Tag

This is the simplest case. Get the script in whatever way you prefer and include it on your page.

```html
<script src="//path/to/mpd-parser.min.js"></script>
<script>
  var mpdParser = window['mpd-parser'];
  var parsedManifest = mpdParser.parse(manifest, { manifestUri });
</script>
```

### Browserify

When using with Browserify, install mpd-parser via npm and `require` the parser as you would any other module.

```js
var mpdParser = require('mpd-parser');

var parsedManifest = mpdParser.parse(manifest, { manifestUri });
```

With ES6:
```js
import { parse } from 'mpd-parser';

const parsedManifest = parse(manifest, { manifestUri });
```

### RequireJS/AMD

When using with RequireJS (or another AMD library), get the script in whatever way you prefer and `require` the parser as you normally would:

```js
require(['mpd-parser'], function(mpdParser) {
  var parsedManifest = mpdParser.parse(manifest, { manifestUri });
});
```

## License

Apache-2.0. Copyright (c) Brightcove, Inc
