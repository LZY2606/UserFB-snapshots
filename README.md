# m3u8-parser
[![Build Status](https://travis-ci.org/videojs/m3u8-parser.svg?branch=master)](https://travis-ci.org/videojs/m3u8-parser)
[![Greenkeeper badge](https://badges.greenkeeper.io/videojs/m3u8-parser.svg)](https://greenkeeper.io/)
[![Slack Status](http://slack.videojs.com/badge.svg)](http://slack.videojs.com)

[![NPM](https://nodei.co/npm/m3u8-parser.png?downloads=true&downloadRank=true)](https://nodei.co/npm/m3u8-parser/)

m3u8 parser

<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Installation](#installation)
- [Usage](#usage)
  - [Constructor Options](#constructor-options)
  - [Parsed Output](#parsed-output)
- [Supported Tags](#supported-tags)
  - [Basic Playlist Tags](#basic-playlist-tags)
  - [Media Segment Tags](#media-segment-tags)
  - [Media Playlist Tags](#media-playlist-tags)
  - [Main Playlist Tags](#main-playlist-tags)
  - [Experimental Tags](#experimental-tags)
    - [EXT-X-CUE-OUT](#ext-x-cue-out)
    - [EXT-X-CUE-OUT-CONT](#ext-x-cue-out-cont)
    - [EXT-X-CUE-IN](#ext-x-cue-in)
  - [Not Yet Supported](#not-yet-supported)
  - [Custom Parsers](#custom-parsers)
- [Including the Parser](#including-the-parser)
  - [`<script>` Tag](#script-tag)
  - [Browserify](#browserify)
  - [RequireJS/AMD](#requirejsamd)
- [License](#license)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->
## Installation

```sh
npm install --save m3u8-parser
```

The npm installation is preferred, but Bower works, too.

```sh
bower install  --save m3u8-parser
```

## Usage

```js
var manifest = [
  '#EXTM3U',
  '#EXT-X-VERSION:3',
  '#EXT-X-TARGETDURATION:6',
  '#EXT-X-MEDIA-SEQUENCE:0',
  '#EXT-X-DISCONTINUITY-SEQUENCE:0',
  '#EXTINF:6,',
  '0.ts',
  '#EXTINF:6,',
  '1.ts',
  '#EXT-X-PROGRAM-DATE-TIME:2019-02-14T02:14:00.106Z',
  '#EXTINF:6,',
  '2.ts',
  '#EXT-X-ENDLIST'
].join('\n');

var parser = new m3u8Parser.Parser();

parser.push(manifest);
parser.end();

var parsedManifest = parser.manifest;
```
### Constructor Options

The constructor optinally takes an options object. `uri` and `mainDefinitions` are needed when using `#EXT-X-DEFINE` for variable replacement, and `previousManifest` enables delta playlist updates.

```js
var parser = new m3u8Parser.Parser({
  uri: 'https://exmaple.com/video.m3u8?param_a=34&param_b=abc',
  mainDefinitions: {
    param_c: 'def'
  },
  previousManifest: lastParsedManifest
});
```

* `options.uri` _string_ The URL from which the playlist was fetched. If the request was redirected this should be the final URL. This is required if using `QUERYSTRING` rules with `#EXT-X-DEFINE`.
* `options.mainDefinitions` _object_ An object of definitions from the main playlist. This is required if using `IMPORT` rules with `#EXT-X-DEFINE`.
* `options.previousManifest` _object_ The manifest object returned by the last successful parse of the same rendition (i.e. the previous `parser.manifest`). The parser never makes requests or discovers URLs itself; the caller is responsible for storing the previous result and passing it in. It is only used when the current text is a media playlist that contains an `#EXT-X-SKIP` tag. See [Delta playlist updates](#delta-playlist-updates-ext-x-skip). Without it, or without `#EXT-X-SKIP`, parsing behaves exactly as before and the current text is treated as a complete update.

### Delta playlist updates (EXT-X-SKIP)

Low-latency HLS servers can omit the oldest segments from a media playlist response and describe them with an `#EXT-X-SKIP:SKIPPED-SEGMENTS=K` tag. When `previousManifest` is supplied, those segments are restored so `manifest.segments` is the full playable window instead of only the segments listed in the response.

Given the current `#EXT-X-MEDIA-SEQUENCE` `M` and `SKIPPED-SEGMENTS` `K`:

* the merged `segments` array starts at sequence `M`. Its first `K` entries are the segments numbered `M` through `M+K-1` taken from `previousManifest`, looked up **by sequence number** (never by array position or URI, since different sequences can reference different byte ranges of the same resource). The segments listed in the current text follow them.
* `KEY` and `MAP` inheritance is resumed from the last restored segment and then updated by the declarations actually present in the current text. An explicit `#EXT-X-KEY:METHOD=NONE` clears encryption for everything after it and is not undone by a key carried on a restored segment; a new `#EXT-X-MAP` only affects segments after it.
* `discontinuityStarts` is regenerated against the merged array, with restored discontinuities continuing to count up and discontinuities declared in the current text counting after them. The same rules apply to playlists carrying `#EXT-X-ENDLIST`.
* `preloadSegment` always reflects the current response only: its own `parts` and `preloadHints` are kept, parts and hints from the previous snapshot are never stacked on, and it inherits the `KEY`/`MAP`/timeline state in effect at the end of the current text. The preload hint is never used to fill an omitted full segment. `renditionReports` and other manifest-level fields likewise come from the current response.
* everything copied from `previousManifest` is deep cloned, so the previous snapshot is never mutated and later changes to either object cannot affect the other.

The merge fails with an `error` event instead of returning a partially merged window when:

* the previous snapshot is a master playlist rather than a media playlist,
* `SKIPPED-SEGMENTS` is not a non-negative safe integer, or
* any of the required sequence numbers (`M` through `M+K-1`) is no longer present in the previous snapshot's window.

On failure `manifest.segments` is emptied and no `preloadSegment` is exposed, so an error listener can treat the response as unusable.

### Parsed Output

The parser ouputs a plain javascript object with the following structure:

```js
Manifest {
  allowCache: boolean,
  endList: boolean,
  mediaSequence: number,
  dateRanges: [],
  discontinuitySequence: number,
  playlistType: string,
  custom: {},
  playlists: [
    {
      attributes: {},
      Manifest
    }
  ],
  mediaGroups: {
    AUDIO: {
      'GROUP-ID': {
        NAME: {
          default: boolean,
          autoselect: boolean,
          language: string,
          uri: string,
          instreamId: string,
          characteristics: string,
          forced: boolean
        }
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
  skip: {
    skippedSegments: number,
    recentlyRemovedDateranges: [string]
  },
  partInf: {},
  partTargetDuration: number,
  serverControl: {},
  renditionReports: [ {} ],
  preloadSegment: {
    parts: [ {} ],
    preloadHints: [ {} ],
    uri: string,
    timeline: number,
    key: {},
    map: {}
  },
  segments: [
    {
      title: string,
      byterange: {
        length: number,
        offset: number
      },
      duration: number,
      programDateTime:  number,
      attributes: {},
      discontinuity: number,
      uri: string,
      timeline: number,
      parts: [
        {
          uri: string,
          duration: number,
          independent: boolean,
          byterange: {
            length: number,
            offset: number
          }
        }
      ],
      key: {
        method: string,
        uri: string,
        iv: Uint32Array
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
      'cue-in': string,
      custom: {}
    }
  ]
}
```

* `skip` is present when the playlist contains an `#EXT-X-SKIP` tag. With `previousManifest` supplied, the omitted segments are merged back into `segments`; see [Delta playlist updates](#delta-playlist-updates-ext-x-skip).
* `preloadSegment` holds the partial delivery hints (`#EXT-X-PART` entries in `parts` and `#EXT-X-PRELOAD-HINT` entries in `preloadHints`) that precede the next complete segment. It has no `uri` until that segment is delivered.

## Supported Tags

### Basic Playlist Tags

* [EXTM3U](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.1.1)
* [EXT-X-VERSION](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.1.2)

### Media Segment Tags

* [EXTINF](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.1)
* [EXT-X-BYTERANGE](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.2)
* [EXT-X-DISCONTINUITY](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.3)
* [EXT-X-KEY](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.4)
* [EXT-X-MAP](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.5)
* [EXT-X-PROGRAM-DATE-TIME](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.2.6)
* [EXT-X-DATERANGE](https://datatracker.ietf.org/doc/html/draft-pantos-http-live-streaming-23#section-4.3.2.7)
* [EXT-X-I-FRAMES-ONLY](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.6)

### Media Playlist Tags

* [EXT-X-TARGETDURATION](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.1)
* [EXT-X-MEDIA-SEQUENCE](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.2)
* [EXT-X-DISCONTINUITY-SEQUENCE](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.3)
* [EXT-X-ENDLIST](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.4)
* [EXT-X-PLAYLIST-TYPE](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.3.5)
* [EXT-X-START](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.5.2)
* [EXT-X-INDEPENDENT-SEGMENTS](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.5.1)
* [EXT-X-DEFINE](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis#section-4.4.2.3)

### Main Playlist Tags

* [EXT-X-MEDIA](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.4.1)
* [EXT-X-STREAM-INF](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.4.2)
* [EXT-X-I-FRAME-STREAM-INF](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.4.3)
* [EXT-X-CONTENT-STEERING](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis#section-4.4.6.6)
* [EXT-X-DEFINE](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis#section-4.4.2.3)

### Experimental Tags

m3u8-parser supports 3 additional **Media Segment Tags** not present in the HLS specification.

#### EXT-X-CUE-OUT

The `EXT-X-CUE-OUT` indicates that the following media segment is a break in main content and the start of interstitial content. Its format is:

```
#EXT-X-CUE-OUT:<duration>
```

where `duration` is a decimal-floating-point or decimal-integer number that specifies the total duration of the interstitial in seconds.

#### EXT-X-CUE-OUT-CONT

The `EXT-X-CUE-OUT-CONT` indicates that the following media segment is a part of interstitial content and not the main content. Every media segment following a media segment with an `EXT-X-CUE-OUT` tag *SHOULD* have an `EXT-X-CUE-OUT-CONT` applied to it until there is an `EXT-X-CUE-IN` tag. A media segment between a `EXT-X-CUE-OUT` and `EXT-X-CUE-IN` segment without a `EXT-X-CUE-OUT-CONT` is assumed to be part of the interstitial. Its format is:

```
#EXT-X-CUE-OUT-CONT:<n>/<duration>
```

where `n` is a decimal-floating-point or decimal-integer number that specifies the time in seconds the first sample of the media segment lies within the interstitial content and `duration` is a decimal-floating-point or decimal-integer number that specifies the total duration of the interstitial in seconds. `n` *SHOULD* be the sum of `EXTINF` durations for all preceding media segments up to the `EXT-X-CUE-OUT` tag for the current interstitial. `duration` *SHOULD* match the `duration` specified in the `EXT-X-CUE-OUT` tag for the current interstitial.'

#### EXT-X-CUE-IN

The `EXT-X-CUE-IN` indicates the end of the interstitial and the return of the main content. Its format is:

```
#EXT-X-CUE-IN
```

There *SHOULD* be a closing `EXT-X-CUE-IN` tag for every `EXT-X-CUE-OUT` tag. If a second `EXT-X-CUE-OUT` tag is encountered before an `EXT-X-CUE-IN` tag, the client *MAY* choose to ignore the `EXT-X-CUE-OUT` and treat it as part of the interstitial, or reject the playlist.

Example media playlist using `EXT-X-CUE-` tags.

```
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXTINF:10,
0.ts
#EXTINF:10,
1.ts
#EXT-X-CUE-OUT:30
#EXTINF:10,
2.ts
#EXT-X-CUE-OUT-CONT:10/30
#EXTINF:10,
3.ts
#EXT-X-CUE-OUT-CONT:20/30
#EXTINF:10,
4.ts
#EXT-X-CUE-IN
#EXTINF:10,
5.ts
#EXTINF:10,
6.ts
#EXT-X-ENDLIST
```

### Not Yet Supported

* [EXT-X-SESSION-DATA](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.4.4)
* [EXT-X-SESSION-KEY](http://tools.ietf.org/html/draft-pantos-http-live-streaming#section-4.3.4.5)

### Custom Parsers

To add a parser for a non-standard tag the parser object allows for the specification of custom tags using regular expressions. If a custom parser is specified, a `custom` object is appended to the manifest object.

```js
const manifest = [
  '#EXTM3U',
  '#EXT-X-VERSION:3',
  '#VOD-FRAMERATE:29.97',
  ''
].join('\n');

const parser = new m3u8Parser.Parser();
parser.addParser({
  expression: /^#VOD-FRAMERATE/,
  customType: 'framerate'
});

parser.push(manifest);
parser.end();
parser.manifest.custom.framerate // "#VOD-FRAMERATE:29.97"
```

Custom parsers may additionally be provided a data parsing function that take a line and return a value.

```js
const manifest = [
  '#EXTM3U',
  '#EXT-X-VERSION:3',
  '#VOD-FRAMERATE:29.97',
  ''
].join('\n');

const parser = new m3u8Parser.Parser();
parser.addParser({
  expression: /^#VOD-FRAMERATE/,
  customType: 'framerate',
  dataParser: function(line) {
    return parseFloat(line.split(':')[1]);
  }
});

parser.push(manifest);
parser.end();
parser.manifest.custom.framerate // 29.97
```

Custom parsers may also extract data at a segment level by passing `segment: true` to the options object. Having a segment level custom parser will add a `custom` object to the segment data.

```js
const manifest = [
    '#EXTM3U',
    '#VOD-TIMING:1511816599485',
    '#EXTINF:8.0,',
    'ex1.ts',
    ''
  ].join('\n');

const parser = new m3u8Parser.Parser();
parser.addParser({
  expression: /#VOD-TIMING/,
  customType: 'vodTiming',
  segment: true
});

parser.push(manifest);
parser.end();
parser.manifest.segments[0].custom.vodTiming // #VOD-TIMING:1511816599485
```

Custom parsers may also map a tag to another tag. The old tag will not be replaced and all matching registered mappers and parsers will be executed.
```js
const manifest = [
    '#EXTM3U',
    '#EXAMPLE',
    '#EXTINF:8.0,',
    'ex1.ts',
    ''
  ].join('\n');

const parser = new m3u8Parser.Parser();
parser.addTagMapper({
  expression: /#EXAMPLE/,
  map(line) {
    return `#NEW-TAG:123`;
  }
});
parser.addParser({
  expression: /#NEW-TAG/,
  customType: 'mappingExample',
  segment: true
});

parser.push(manifest);
parser.end();
parser.manifest.segments[0].custom.mappingExample // #NEW-TAG:123
```

## Including the Parser

To include m3u8-parser on your website or web application, use any of the following methods.

### `<script>` Tag

This is the simplest case. Get the script in whatever way you prefer and include it on your page.

```html
<script src="//path/to/m3u8-parser.min.js"></script>
<script>
  var parser = new m3u8Parser.Parser();
</script>
```

### Browserify

When using with Browserify, install m3u8-parser via npm and `require` the parser as you would any other module.

```js
var m3u8Parser = require('m3u8-parser');

var parser = new m3u8Parser.Parser();
```

With ES6:
```js
import { Parser } from 'm3u8-parser';

const parser = new Parser();
```

### RequireJS/AMD

When using with RequireJS (or another AMD library), get the script in whatever way you prefer and `require` the parser as you normally would:

```js
require(['m3u8-parser'], function(m3u8Parser) {
  var parser = new m3u8Parser.Parser();
});
```

## License

Apache-2.0. Copyright (c) Brightcove, Inc
