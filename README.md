# VideoForge

A developer-first JavaScript (Node.js) library for building video editors, automation tools, and AI-powered editing systems.

VideoForge provides a **Timeline Engine** as its core abstraction. You compose a project by adding tracks and clips, attaching effects, and exporting to any supported format — all through a clean, chainable API. No UI, no FFmpeg wiring required for the library itself.

**License:** AGPL-3.0

---

## Architecture

```
Project
├── Timeline                  ← query engine (duration, fps, resolution)
├── Tracks[]
│   ├── Clips[]               ← VideoClip / AudioClip / ImageClip / TextClip / ShapeClip
│   │   └── Effects[]         ← FadeEffect / Transition / custom Effect
│   └── CaptionClip           ← Motion Typography Engine
│       ├── CaptionSegments[]
│       │   └── CaptionWords[]
│       │       └── CaptionCharacters[]
│       ├── CaptionAnimations[] ← 22 animation types
│       ├── CaptionEffects[]    ← 19 visual effect types
│       └── KeyframeSet         ← per-property interpolation
│
└── Export Pipeline
    └── TimelineConverter     ← Project → IntermediateTimeline (ITR)
        └── IntermediateTimeline
            ├── PremiereXmlExporter  → .xml    (XMEML v5)
            ├── FcpxmlExporter       → .fcpxml (FCPXML 1.10)
            ├── EdlExporter          → .edl    (CMX3600)
            ├── JsonExporter         → .vfp    (VideoForge JSON)
            └── Mp4Exporter          → .mp4    (FFmpeg V1)
```

All exporters consume the **Intermediate Timeline Representation (ITR)** — never the Project directly. This single conversion path ensures consistent output across all formats and preserves unsupported features in a `vf:` namespace for round-trips.

---

## Installation

```bash
npm install videoforge
# or, within this repo:
node examples/basic-usage.js
```

Requires Node.js ≥ 18.

---

## Quick Start

```js
import { Project } from 'videoforge';

const project = new Project({ name: 'My Edit', fps: 30 });

const track = project.addTrack('video');
const clip  = track.addVideo('intro.mp4');

clip.trim(5, 20);   // use source seconds 5–20
clip.fadeIn(1);     // 1-second opacity ramp

await project.export({
  type:   'json',
  output: './project.vfp',
});
```

---

## API Reference

### `Project`

```js
import { Project } from 'videoforge';

const project = new Project({ name: 'My Film', fps: 24, width: 3840, height: 2160 });

// Tracks
const track = project.addTrack('video');        // returns Track
project.getTrack(id);                           // Track | undefined
project.track(id);                              // alias for getTrack
project.removeTrack(id);                        // boolean
project.getTracks();                            // Track[]
project.reorderTracks([id1, id2, id3]);         // Project (chainable)

// Validation
const report = project.validate();                     // { valid, warnings, errors }
const mp4report = project.validate({ exporter: 'mp4' }); // includes MP4-specific checks
if (!report.valid) console.error(report.errors);
console.warn(report.warnings);

// Export
await project.export({ type: 'json',    output: './out.vfp'     });
await project.export({ type: 'premiere', output: './out.xml'    });
await project.export({ type: 'fcpxml',  output: './out.fcpxml'  });
await project.export({ type: 'edl',     output: './out.edl'     });
await project.export({ type: 'mp4',     output: './out.mp4'     }); // requires FFmpeg

// Persistence
await project.save('./autosave.vfp');
const loaded = await Project.load('./autosave.vfp');

// Serialisation
const json = project.toJSON();
const clone = Project.fromJSON(json);
```

---

### `Track`

```js
const videoTrack   = project.addTrack('video');
const audioTrack   = project.addTrack('audio');
const textTrack    = project.addTrack('text');
const shapeTrack   = project.addTrack('shape');
const captionTrack = project.addTrack('caption');

// Clip factories (returns the created clip)
const clip = videoTrack.addVideo('footage.mp4', { outPoint: 60 });
              audioTrack.addAudio('music.mp3');
              textTrack .addText('Hello World', { fontSizeValue: 48 });
              shapeTrack.addShape('rectangle', { width: 200, height: 100 });
              videoTrack.addImage('logo.png');

// Caption clip with preset
const caption = captionTrack.addCaption('Welcome back!', {
  preset:              'hormozi',       // apply a built-in preset
  maxWordsPerSegment:  3,
  wordTimings:         [...],           // optional pre-parsed timing data
});

// Management
track.getClip(id);       // Clip | undefined
track.getClips();        // Clip[] sorted by startTime
track.removeClip(id);    // boolean
track.moveClip(id, t);   // boolean
track.getDuration();     // number (seconds)
```

---

### `Clip` (base — all subclasses inherit these)

```js
clip.trim(5, 20);         // set in/out points (source seconds)
clip.move(30);            // reposition on the timeline
clip.split(60);           // returns { head, tail }
clip.copy();              // deep-clone with new ID
clip.remove();            // removes from owning track

clip.fadeIn(1);           // 1-second fade-in effect
clip.fadeOut(0.5);        // 0.5-second fade-out effect
clip.addCrop({ top: 100, bottom: 100, alignment: 'center' }); // crop edges

clip.addEffect(effect);   // append to effect chain
clip.removeEffect(id);    // boolean
clip.getEffect(id);       // Effect | undefined

clip.toJSON();            // serialise
```

---

### `VideoClip`

```js
const clip = track.addVideo('scene.mp4');

clip.volume(0.8);   // set volume (0–2), returns this
clip.volume();      // read current value → 0.8
clip.mute();        // silence without losing volume setting
clip.unmute();

clip.speed(2);      // 2× playback speed
clip.speed();       // read → 2
clip.reverse();     // toggle reversed playback
clip.reverse(true); // set explicitly
```

---

### `AudioClip`

```js
const music = track.addAudio('bg.mp3');

music.volume(0.5);
music.pan(-0.3);    // stereo pan (-1 left … 0 center … 1 right)
music.speed(1.05);
music.mute();
```

---

### `ImageClip`

```js
const logo = track.addImage('logo.png', { outPoint: 10 });

logo.position(100, 50);    // x, y on canvas
logo.position();           // → { x: 100, y: 50 }
logo.scale(0.5);           // uniform scale
logo.scale(1.5, 1);        // non-uniform
logo.rotation(45);         // degrees clockwise
logo.opacity(0.8);         // 0–1
```

---

### `TextClip`

```js
const title = track.addText('VideoForge', { startTime: 0, outPoint: 5 });

title.font('Helvetica Neue');
title.fontSize(72);
title.color('#FFFFFF');
title.background('#00000080');  // semi-transparent black
title.align('center');          // 'left' | 'center' | 'right' | 'justify'
title.bold(true);
title.italic(false);
title.position(960, 540);
title.opacity(0.9);
```

---

### `ShapeClip`

```js
import { SHAPE_TYPES } from 'videoforge';

const box = track.addShape(SHAPE_TYPES.RECTANGLE);

box.position(0, 900);
box.size(640, 80);
box.fillColor('#1A1A2E');
box.strokeColor('#FFFFFF');
box.strokeWidth(2);
box.opacity(0.85);
box.rotation(0);
box.cornerRadius(8);
```

---

### Effects

```js
import { FadeEffect, CropEffect, Transition, TRANSITION_TYPES, EASING, CROP_ALIGNMENT } from 'videoforge';

// Manual effect construction
const fadeIn  = new FadeEffect('in',  1.5, { easing: EASING.EASE_OUT });
const fadeOut = new FadeEffect('out', 1,   { easing: EASING.EASE_IN });

clip.addEffect(fadeIn);

// Crop effect — remove pixels from any edge, then align content within the frame
clip.addCrop({ top: 100, bottom: 100, alignment: CROP_ALIGNMENT.CENTER });

// Or construct directly for more control
clip.addEffect(new CropEffect({
  left:      200,
  right:     200,
  top:       0,
  bottom:    0,
  alignment: CROP_ALIGNMENT.LEFT,   // content hugs left edge
}));

// Transitions link two adjacent clips
const dissolve = new Transition(TRANSITION_TYPES.CROSS_DISSOLVE, 1);
dissolve.link(clipA, clipB);
clipB.addEffect(dissolve);

// Opacity at a given moment within a fade
const opacity = fadeIn.getOpacityAt(0.75); // seconds into the fade
```

---

### `Timeline`

```js
const tl = project.timeline;

tl.getTotalDuration();        // seconds
tl.getTotalFrames();          // frame count
tl.timeToFrame(12.5);         // → 375  (at 30fps)
tl.frameToTime(375);          // → 12.5

tl.getClipsAtTime(10);        // Clip[] active at t=10s
tl.getClipsInRange(5, 15);    // Clip[] overlapping the window
tl.findOverlaps();            // [[clipA, clipB], ...] conflicts per track
tl.getClipCount();
tl.getTrackCount();
```

---

## Caption & Motion Typography Engine

VideoForge includes a professional motion typography system with four levels of granularity — every level is independently animatable.

```
CaptionClip
└── CaptionSegment[]      ← one line / phrase on screen at a time
    └── CaptionWord[]     ← per-word animations and karaoke fills
        └── CaptionCharacter[] ← per-character scramble / stagger / spin
```

### Basic usage

```js
const track = project.addTrack('caption');

// Plain transcript
const caption = track.addCaption('Welcome to VideoForge', {
  outPoint: 5,
  preset:   'hormozi',
});

// Word-level timing for karaoke / highlight sync
caption.buildKaraoke({ fillColor: '#FFD700' });

// Highlight specific words
caption.highlightKeywords(['VideoForge', 'caption'], '#FF4444');
```

### Applying presets

```js
import { createPreset, PRESET_REGISTRY } from 'videoforge';

caption.applyPreset('mrbeast');       // bold yellow words, pop animation
caption.applyPreset('podcast');       // clean lower-third style
caption.applyPreset('karaoke');       // progressive word fill
caption.applyPreset('gaming');        // neon glow, glitch effect

// All 10 built-in presets:
// hormozi | mrbeast | podcast | news | documentary
// karaoke | minimal | gaming | luxury | corporate
console.log([...PRESET_REGISTRY.keys()]);
```

### Animations (22 types)

```js
import {
  FadeAnimation, SlideAnimation, ScaleAnimation, RotateAnimation,
  BounceAnimation, PopAnimation, PulseAnimation, ShakeAnimation,
  WobbleAnimation, WaveAnimation, SwingAnimation, FlipAnimation,
  TypewriterAnimation, KaraokeAnimation, RevealAnimation, ScrambleAnimation,
  ElasticAnimation, GlitchAnimation, HighlightAnimation, ZoomAnimation,
  BlurRevealAnimation, StaggerAnimation,
  ANIMATION_TARGET, ANIMATION_TYPES,
} from 'videoforge';

caption.addAnimation(new SlideAnimation({
  direction: 'up',
  duration:  0.4,
  target:    ANIMATION_TARGET.WORD,   // CLIP | SEGMENT | WORD | CHARACTER
}));

// Stagger: apply any animation with incremental per-element delay
caption.addAnimation(new StaggerAnimation({
  animation: new PopAnimation({ duration: 0.3 }),
  staggerDelay: 0.05,   // seconds between each element
  order: 'forward',     // forward | reverse | random | center | edges
}));
```

### Visual effects (19 types)

```js
import {
  GlowEffect, ShadowEffect, OutlineEffect, GradientEffect, NeonEffect,
  GlassEffect, BlurEffect, MotionBlurEffect, BackgroundBoxEffect,
  RoundedBoxEffect, HighlightEffect, UnderlineEffect, StrikeThroughEffect,
  NoiseEffect, GrainEffect, ChromaticAberrationEffect, BloomEffect,
  DistortionEffect, ReflectionEffect,
} from 'videoforge';

caption.addEffect(new GlowEffect({ color: '#FFD700', radius: 12, strength: 0.8 }));
caption.addEffect(new OutlineEffect({ color: '#000000', width: 3 }));
caption.addEffect(new BackgroundBoxEffect({ color: '#000000CC', padding: 12 }));
```

### Keyframe engine

```js
import { KeyframeSet, KEYFRAMEABLE_PROPERTIES, CAPTION_EASING } from 'videoforge';

caption.addKeyframe('opacity', 0,   0);
caption.addKeyframe('opacity', 0.5, 1);
caption.addKeyframe('opacity', 4.5, 1);
caption.addKeyframe('opacity', 5,   0);

// 24 animatable properties, 14 easing curves
// CAPTION_EASING: linear | easeIn | easeOut | easeInOut | spring | snap | overshoot | ...
```

### `CaptionStyle`

```js
import { CaptionStyle } from 'videoforge';

const style = new CaptionStyle({
  fontFamily:    'Montserrat',
  fontSize:       80,
  fontWeight:     900,
  color:          '#FFFFFF',
  strokeColor:    '#000000',
  strokeWidth:    4,
  letterSpacing:  2,
  lineHeight:     1.3,
  textTransform:  'uppercase',
});

caption.style = style;
```

### `CaptionLayout`

```js
import { CaptionLayout, ANCHOR_POINT, WRAP_MODE, SOCIAL_SAFE_ZONES } from 'videoforge';

caption.layout = new CaptionLayout({
  anchorPoint:  ANCHOR_POINT.BOTTOM_CENTER,
  wrapMode:     WRAP_MODE.WORD,
  maxWidth:     0.85,                        // fraction of canvas width
  safeZone:     SOCIAL_SAFE_ZONES.TIKTOK,    // TikTok | Instagram | YouTube | Shorts
  marginBottom: 0.1,
});
```

### `MotionTypographyEngine`

```js
import { MotionTypographyEngine, SEGMENTATION_STRATEGY } from 'videoforge';

const engine = new MotionTypographyEngine({ maxWordsPerSegment: 3 });

const segments = engine.segmentTranscript(words, {
  strategy: SEGMENTATION_STRATEGY.MAX_WORDS,  // MAX_WORDS | PUNCTUATION | TIME_GAP | SENTENCE | HYBRID
});
```

---

## Professional Interchange Export System

VideoForge uses a **canonical Intermediate Timeline Representation (ITR)** as the single conversion point between the Project model and every export format. This means:

- One conversion path — no format-specific Project traversal
- Unsupported features are preserved in a `vf:` XML namespace for round-trips
- Each exporter receives a fully-validated, format-independent data structure

### Export pipeline

```
Project
  └─► TimelineConverter.convert(project)
        └─► IntermediateTimeline (ITR)
              ├─► PremiereXmlExporter  → XMEML v5 .xml
              ├─► FcpxmlExporter       → FCPXML 1.10 .fcpxml
              ├─► EdlExporter          → CMX3600 .edl
              └─► JsonExporter         → VideoForge .vfp
```

### Premiere Pro XML (XMEML v5)

```js
import { PremiereXmlExporter } from 'videoforge';

const exporter = new PremiereXmlExporter(project, {
  pretty:            true,   // indent output
  validateInput:     true,   // run ITR validation before generating
  validateOutput:    true,   // run structural XML validation after generating
  includeVfMetadata: true,   // embed vf: namespace block for round-trip
  sequenceName:      'Main Sequence',
});

// Write to disk
await exporter.export('./output/project.xml');

// In-memory string
const xml = exporter.toString();
```

Generated structure:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE xmeml>
<xmeml version="5">
  <sequence id="seq_main">
    <name>My Project</name>
    <duration>900</duration>
    <rate><timebase>30</timebase><ntsc>FALSE</ntsc></rate>
    <media>
      <video>
        <format>...</format>
        <file id="asset_abc123">          <!-- deduplicated asset pool -->
          <pathurl>file:///footage/clip.mp4</pathurl>
          ...
        </file>
        <track>
          <clipitem id="clip_xyz_video">  <!-- per-clip, references file by id -->
            <start>0</start><end>150</end>
            <in>0</in><out>150</out>
            <filter>...</filter>          <!-- opacity / speed / motion filters -->
          </clipitem>
        </track>
        <track>                           <!-- caption track → generator items -->
          <generatoritem id="gen_...">
            <effect><name>Text</name>...</effect>
          </generatoritem>
        </track>
      </video>
      <audio>...</audio>
    </media>
    <vf:metadata xmlns:vf="https://videoforge.dev/ns/1.0">
      <vf:payload>...</vf:payload>        <!-- full caption/animation payload -->
    </vf:metadata>
  </sequence>
</xmeml>
```

### Final Cut Pro XML (FCPXML 1.10)

```js
import { FcpxmlExporter } from 'videoforge';

const exporter = new FcpxmlExporter(project, {
  fcpxmlVersion:     '1.10',
  pretty:            true,
  validateInput:     true,
  validateOutput:    true,
  includeVfMetadata: true,
  libraryName:       'My Library',
  eventName:         'Rough Cut',
});

await exporter.export('./output/project.fcpxml');
const xml = exporter.toString();
```

Generated structure:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE fcpxml>
<fcpxml version="1.10">
  <resources>
    <format id="r_seq_format" frameDuration="1/30s" width="1920" height="1080"/>
    <asset id="asset_abc" src="file:///footage/main.mp4" duration="300/30s" hasVideo="1" hasAudio="1">
      <media-rep kind="original-media" src="file:///footage/main.mp4"/>
    </asset>
    <effect id="r_effect_title" name="Basic Title" type="motion"/>
  </resources>
  <library name="My Library">
    <event name="Rough Cut">
      <project name="My Film" uid="proj_...">
        <sequence format="r_seq_format" duration="900/30s" tcFormat="NDF">
          <spine>
            <clip name="main" ref="asset_abc" offset="0/30s" duration="300/30s">
              <audio-clip .../>              <!-- connected audio -->
              <title ref="r_effect_title" lane="-2" ...>   <!-- connected caption -->
                <param name="Text">...
                <text><text-style font="Helvetica Neue" fontSize="72">Hello</text-style></text>
              </title>
            </clip>
            <gap name="Gap" offset="300/30s" duration="30/30s"/>
          </spine>
        </sequence>
      </project>
    </event>
  </library>
</fcpxml>
```

**Time format:** All times use FCPXML rational notation — `N/Ds` where `D` is the rate denominator. For 29.97fps: `1001/30000s`. For 24fps: `1/24s`.

**Supported frame rates:** 23.976 · 24 · 25 · 29.97 · 30 · 50 · 59.94 · 60

### CMX3600 EDL

```js
import { EdlExporter } from 'videoforge';

const exporter = new EdlExporter(project, {
  title:           'My Cut',
  dropFrame:       null,    // auto-detect from fps (true for 29.97/59.94)
  includeAudio:    true,    // emit AX audio events
  includeComments: true,    // annotate unsupported features as * comments
});

await exporter.export('./output/project.edl');
const edl = exporter.toString();
```

Generated output:

```
TITLE: My Cut
FCM: NON-DROP FRAME

* Generated by VideoForge EDL Exporter
* Project: My Film | FPS: 30 | Resolution: 1920x1080

001  clip_a   V    C        00:00:00:00 00:00:05:00 00:00:00:00 00:00:05:00
* FROM CLIP NAME: clip_a.mp4

002  clip_b   V    D 030    00:00:02:00 00:00:07:00 00:00:05:00 00:00:10:00
* FROM CLIP NAME: clip_b.mp4

003  narrat   A    C        00:00:00:00 00:00:20:00 00:00:00:00 00:00:20:00

* CAPTION: "Welcome to VideoForge" [not representable in EDL]
```

### ITR — direct access

Use the ITR directly when building custom exporters or inspection tools:

```js
import { TimelineConverter, InterchangeValidator } from 'videoforge';

const converter = new TimelineConverter();
const itr       = converter.convert(project);

// Validate before exporting
const validator = new InterchangeValidator();
const result    = validator.validateTimeline(itr);
if (!result.valid) console.error(result.errors);
console.warn(result.warnings);

// Inspect
itr.getVideoTracks();    // TrackRepresentation[]
itr.getAudioTracks();    // TrackRepresentation[]
itr.getCaptionTracks();  // TrackRepresentation[]
itr.getAllClips();        // ClipRepresentation[]
itr.computeDuration();   // seconds
itr.toJSON();            // plain object

// Per-track clips
for (const track of itr.getVideoTracks()) {
  for (const clip of track.getSortedClips()) {
    console.log(clip.timelineStart, '→', clip.timelineEnd, clip.assetId);
  }
}
```

### Time utilities

```js
import { TimeCode, PREMIERE_TICKS_PER_SECOND } from 'videoforge';

const tc = new TimeCode(10.5, 29.97);

tc.toSmpteNdf();          // '00:00:10:15'   NDF timecode
tc.toSmpteDf();           // '00:00:10;15'   drop-frame timecode
tc.toFcpRational();       // '315000/30000s' FCPXML rational
tc.toPremiereTicks_s();   // '2667168000000' Premiere tick string

TimeCode.fcpFrameDuration(23.976);   // '1001/24000s'
TimeCode.premiereRate(29.97);        // { timebase: 30, ntsc: true }
TimeCode.secondsToSmpte(3661, 30);   // '01:01:01:00'
```

### XML utilities

```js
import { XmlBuilder, escapeText, escapeAttr, XmlValidator } from 'videoforge';

// Build XML fluently
const b = new XmlBuilder({ pretty: true });
b.declaration()
 .open('root', { version: '1.0' })
   .leaf('name', {}, 'My Project')
   .comment('generated by VideoForge')
 .close();
console.log(b.toString());

// Validate generated output
const validator = new XmlValidator();
const result = validator.validatePremiereXml(xml);    // { valid, errors[], warnings[] }
const fcpResult = validator.validateFcpXml(fcpxml);
const edlResult = validator.validateEdl(edl);
```

### Custom exporter using ITR

```js
import { Exporter, TimelineConverter, XmlBuilder } from 'videoforge';
import { promises as fs } from 'fs';

class DaVinciXmlExporter extends Exporter {
  constructor(project, options = {}) {
    super(project, options);
    this._converter = new TimelineConverter();
  }

  async export(outputPath) {
    const dest = this.resolveOutputPath(outputPath, '.xml');
    await fs.mkdir(require('path').dirname(dest), { recursive: true });
    await fs.writeFile(dest, this.toString(), 'utf8');
    return dest;
  }

  toString() {
    const itr = this._converter.convert(this.project);
    const b   = new XmlBuilder();
    b.declaration()
     .open('davinci', { version: '1.0' });

    for (const track of itr.getVideoTracks()) {
      b.open('track', { name: track.name });
      for (const clip of track.getSortedClips()) {
        b.leaf('clip', {
          start: String(clip.timelineStart),
          end:   String(clip.timelineEnd),
          src:   itr.getAsset(clip.assetId)?.path ?? '',
        });
      }
      b.close();
    }

    b.close();
    return b.toString();
  }
}
```

---

### Exporters — summary

| Exporter | Format | Status | Notes |
|---|---|---|---|
| `JsonExporter` | VideoForge `.vfp` | Full | Round-trip serialisation |
| `PremiereXmlExporter` | XMEML v5 `.xml` | Full | File dedup, filters, transitions, captions |
| `FcpxmlExporter` | FCPXML 1.10 `.fcpxml` | Full | Rational time, connected clips, titles |
| `EdlExporter` | CMX3600 `.edl` | Full | NDF/DF timecodes, dissolves, audio AX events |
| `Mp4Exporter` | MP4 `.mp4` | V1 — Full | Requires FFmpeg in PATH. See [MP4 Export](#mp4-export) |

---

## MP4 Export

`Mp4Exporter` renders a VideoForge project to an `.mp4` file by generating an FFmpeg `-filter_complex` command and executing it in a child process.  FFmpeg must be installed and available in `PATH`.

### Quick start

```js
import { Mp4Exporter } from 'videoforge';

const exporter = new Mp4Exporter(project, {
  preset:       'medium',    // FFmpeg libx264 preset (ultrafast → veryslow)
  crf:          18,          // quality (0 = lossless, 51 = worst)
  videoCodec:   'libx264',
  audioCodec:   'aac',
  pixelFormat:  'yuv420p',
  audioBitrate: '192k',
  onProgress:   (pct) => console.log(`${pct.toFixed(0)}%`),
});

const result = await exporter.export('./output/final.mp4');
// result: { success, output, duration, fileSize }
```

Or via `project.export()`:

```js
const result = await project.export({
  type:   'mp4',
  output: './output/final.mp4',
  preset: 'fast',
  crf:    22,
});
```

### What's supported (V1)

| Feature | Support |
|---|---|
| `VideoClip` — trim (`inPoint`, `outPoint`) | ✅ |
| `VideoClip` — speed (any positive multiplier) | ✅ |
| `VideoClip` — reverse playback | ✅ |
| `VideoClip` — `fadeIn` / `fadeOut` (video) | ✅ |
| `VideoClip` — `volume`, `mute` (embedded audio) | ✅ |
| Embedded audio in video files (auto-detected via ffprobe) | ✅ |
| `AudioClip` — trim, speed, volume, mute, fadeIn, fadeOut | ✅ |
| Multiple clips on a video track — concatenated in order | ✅ |
| Multiple audio tracks — mixed via `amix` | ✅ |
| Progress callback (`onProgress`) | ✅ |
| `project.validate({ exporter: 'mp4' })` pre-export checks | ✅ |

### What's not supported (V1 limitations)

| Feature | Status |
|---|---|
| `ImageClip` rendering | ⚠️ Skipped — clips are ignored by the MP4 exporter |
| `TextClip` / `CaptionClip` rendering | ⚠️ Skipped — use Premiere/FCP XML export instead |
| `ShapeClip` rendering | ⚠️ Skipped |
| Transitions (cross-dissolve, wipe, etc.) | ⚠️ Ignored |
| Color correction / LUT effects | ⚠️ Ignored |
| Blur / custom effects | ⚠️ Ignored |
| Keyframe animation | ⚠️ Not implemented |
| Gaps between clips (silence / black) | ⚠️ Clips are placed back-to-back |
| GPU encoding (NVENC, VideoToolbox) | Configurable via `videoCodec` option |

Use `project.validate({ exporter: 'mp4' })` to get a full warning list before exporting:

```js
const report = project.validate({ exporter: 'mp4' });
// report.warnings includes UNSUPPORTED_CLIP_TYPE and UNSUPPORTED_EFFECT entries
// report.errors includes MISSING_ASSET, INVALID_TRIM, NEGATIVE_DURATION entries
// report.valid is false if any errors are present
```

### Inspect the generated FFmpeg command

```js
const exporter = new Mp4Exporter(project, { preset: 'ultrafast' });
const args = exporter.buildCommand('./output/preview.mp4');
console.log('ffmpeg', args.join(' '));
```

---

### Preview (skeleton)

```js
import { PreviewRenderer, PreviewPlayer } from 'videoforge';

const renderer = new PreviewRenderer(project.timeline);
renderer.setBackend(myCanvasBackend);  // bring your own backend

const player = new PreviewPlayer(renderer, project.timeline, {
  loop: true,
  onFrame: (time, frame) => { /* draw frame */ },
  onStateChange: (state) => console.log('State:', state),
});

player.play();
player.seek(30);
player.pause();
player.stop();

console.log(player.currentTime);  // seconds
console.log(player.state);        // 'playing' | 'paused' | 'idle' | 'ended'
```

### CPU preview in Node.js (in-memory frames)

`CpuBackend` composites the timeline into plain RGBA buffers — no DOM, no
canvas, no native dependencies.  You supply already-decoded video frames
through `readFrame(clip, sourceTime)` (sync or async); each frame is
`{ width, height, data }` with `data` a non-premultiplied RGBA
`Uint8ClampedArray`, row by row.

```js
import { Project, PreviewRenderer, CpuBackend } from 'videoforge';

const project = new Project({ fps: 30, width: 640, height: 360 });
const track = project.addTrack('video');
const clip = track.addVideo('intro.mp4', { inPoint: 0, outPoint: 5, startTime: 0 });
clip.speed(2);  // plays source 0–5 s over 2.5 s of timeline

// Stand-in decoder: map (clip, sourceTime) to decoded RGBA frames.
const readFrame = async (clip, sourceTime) => decodeFrame(clip.asset.path, sourceTime);

const renderer = new PreviewRenderer(project.timeline, {
  backend: new CpuBackend({ width: 640, height: 360, readFrame }),
});

const frame = await renderer.render(1.25);
// → { width: 640, height: 360, data: Uint8ClampedArray } — source time 2.5 s

// Scrub a whole range frame by frame:
await renderer.renderRange(0, 1, async (frame, frameNumber) => {
  inspect(frame.data);
});
```

Every render starts from a transparent canvas; tracks composite bottom →
top (source-over), and clips joined by an enabled `Transition`
(`crossDissolve`, `wipeLeft`) render as a single blended layer across the
overlap.  Returned frames own fresh storage on every call.

---

## File Structure

```
src/
├── core/
│   ├── Project.js        Root project — owns tracks and timeline
│   ├── Track.js          Clip container for one timeline lane
│   ├── Clip.js           Abstract base for all clip types
│   ├── Asset.js          Source media file reference
│   └── Timeline.js       Temporal query engine
│
├── clips/
│   ├── VideoClip.js      Video with volume/speed/reverse controls
│   ├── AudioClip.js      Audio with volume/pan/speed controls
│   ├── ImageClip.js      Still image with 2-D transforms
│   ├── TextClip.js       Synthetic text renderer
│   └── ShapeClip.js      Synthetic geometry renderer
│
├── effects/
│   ├── Effect.js         Abstract base for all effects
│   ├── FadeEffect.js     Opacity ramp (in / out)
│   ├── CropEffect.js     Edge crop with alignment (top/bottom/left/right + 9-position align)
│   └── Transition.js     Between-clip transition (dissolve, wipe, …)
│
├── captions/
│   ├── CaptionClip.js           Root caption clip — extends Clip
│   ├── CaptionSegment.js        One phrase visible at a time
│   ├── CaptionWord.js           Per-word animations and karaoke
│   ├── CaptionCharacter.js      Per-character granularity
│   ├── CaptionStyle.js          Typography properties
│   ├── CaptionLayout.js         Anchor, wrap mode, safe zones
│   ├── CaptionKeyframe.js       Keyframe engine + 14 easing curves
│   ├── CaptionAnimation.js      Base + 22 animation types
│   ├── CaptionEffect.js         Base + 19 visual effect types
│   ├── CaptionPreset.js         Base + 10 built-in presets
│   ├── CaptionRenderer.js       Backend-agnostic render interface
│   └── MotionTypographyEngine.js  Segmentation + orchestration
│
├── interchange/
│   ├── IntermediateTimeline.js  Canonical ITR root
│   ├── TimelineConverter.js     Project → ITR conversion
│   ├── AssetReference.js        Media file descriptor
│   ├── TrackRepresentation.js   Track in ITR
│   ├── ClipRepresentation.js    Clip in ITR (all types)
│   ├── EffectRepresentation.js  Effect in ITR
│   ├── TransitionRepresentation.js
│   ├── CaptionRepresentation.js  + WebVTT / title helpers
│   ├── utils/
│   │   ├── TimeCode.js           seconds ↔ SMPTE ↔ FCP rational ↔ ticks
│   │   ├── XmlBuilder.js         Fluent stack-based XML builder
│   │   ├── XmlEscaper.js         escapeText / escapeAttr / escapeUrl
│   │   ├── XmlValidator.js       Structural validation (Premiere / FCP / EDL)
│   │   └── XmlNamespaceManager.js  vf: namespace for round-trip metadata
│   └── validation/
│       └── InterchangeValidator.js  Full ITR validation pipeline
│
├── exporters/
│   ├── Exporter.js              Abstract base exporter
│   ├── JsonExporter.js          VideoForge JSON (.vfp) — full
│   ├── PremiereXmlExporter.js   XMEML v5 (.xml) — full
│   ├── FcpxmlExporter.js        FCPXML 1.10 (.fcpxml) — full
│   ├── EdlExporter.js           CMX3600 EDL (.edl) — full
│   ├── Mp4Exporter.js           Re-export from mp4/ subdirectory
│   └── mp4/
│       ├── Mp4Exporter.js       Main exporter — export(), buildCommand()
│       ├── FFmpegCommandBuilder.js  Assembles FFmpeg args array
│       ├── FilterGraphBuilder.js    Builds -filter_complex string
│       └── ProgressParser.js        Parses FFmpeg stderr for progress
│
├── preview/
│   ├── PreviewPlayer.js    Playback driver (rAF / setInterval)
│   ├── PreviewRenderer.js  Frame compositor (backend-agnostic)
│   └── CpuBackend.js       Pure-JS RGBA compositor for Node.js
│
├── utils/
│   ├── IdGenerator.js     Prefixed unique IDs + UUID v4
│   ├── Constants.js       All enumerations and defaults
│   └── FrameOps.js        Pure RGBA frame ops (source-over, dissolve, wipe)
│
└── index.js               Public surface — re-exports everything

tests/
├── core/
│   └── project-validate.test.js  Project.validate() API tests
├── helpers/
│   └── fixtures.js               Shared FFmpeg fixture generator
├── interchange/
│   ├── premiere.test.js          XMEML v5 output tests
│   ├── fcpxml.test.js            FCPXML 1.10 output tests
│   ├── roundtrip.test.js         ITR round-trip fidelity tests
│   └── captions.test.js          Caption export tests
├── exporters/
│   ├── mp4-exporter.test.js      Mp4Exporter unit tests (no FFmpeg needed)
│   └── mp4-integration.test.js   Mp4Exporter integration tests (requires FFmpeg)
└── integration/
    └── mp4-export.test.js        End-to-end export + validate workflow tests

examples/
├── basic-usage.js        Core API overview (JSON export)
├── basic-edit.js         VideoClip trim/speed/fade → MP4
├── podcast-edit.js       AudioClip volume/pan/speed → MP4
├── premiere-export.js    PremiereXmlExporter workflow
├── fcpxml-export.js      FcpxmlExporter workflow
└── caption-demo.js       Caption Engine — presets, animations, effects
```

---

## Constants

```js
import {
  TRACK_TYPES,        // video | audio | image | text | shape
  CLIP_TYPES,         // video | audio | image | text | shape
  ASSET_TYPES,        // video | audio | image | font | synthetic
  EFFECT_TYPES,       // fadeIn | fadeOut | transition | colorCorrection | blur | crop | custom
  CROP_ALIGNMENT,     // center | top | bottom | left | right | topLeft | topRight | bottomLeft | bottomRight
  TRANSITION_TYPES,   // crossDissolve | wipeLeft | wipeRight | wipeUp | wipeDown | slide | zoom | dipToBlack | dipToWhite
  EXPORT_TYPES,       // json | premiere | fcpxml | edl | mp4
  SHAPE_TYPES,        // rectangle | ellipse | triangle | line | polygon | arrow
  TEXT_ALIGN,         // left | center | right | justify
  PLAYER_STATE,       // idle | playing | paused | buffering | ended
  EASING,             // linear | easeIn | easeOut | easeInOut
  DEFAULTS,           // { FPS, WIDTH, HEIGHT, SAMPLE_RATE, … }

  // Caption
  ANIMATION_TYPES,    // fade | slide | scale | rotate | bounce | pop | … (22 types)
  ANIMATION_TARGET,   // CLIP | SEGMENT | WORD | CHARACTER
  STAGGER_ORDER,      // FORWARD | REVERSE | RANDOM | CENTER | EDGES
  CAPTION_EASING,     // linear | easeIn | … | spring | snap | overshoot (14 curves)
  KEYFRAMEABLE_PROPERTIES,  // opacity | x | y | scaleX | scaleY | rotation | … (24)
  WRAP_MODE,          // WORD | CHARACTER | NONE
  ANCHOR_POINT,       // TOP_LEFT | TOP_CENTER | … | BOTTOM_RIGHT (9 positions)
  SOCIAL_SAFE_ZONES,  // TIKTOK | INSTAGRAM | YOUTUBE | SHORTS | REELS | BROADCAST

  // Interchange
  ITR_VERSION,        // '1.0'
  TRANSITION_ALIGNMENT, // center | startBlack | endBlack | custom
} from 'videoforge';
```

---

## Extending VideoForge

### Custom Effect

```js
import { Effect, EFFECT_TYPES } from 'videoforge';

class ChromaKeyEffect extends Effect {
  constructor(keyColor = '#00FF00', tolerance = 0.1) {
    super(EFFECT_TYPES.CUSTOM, { keyColor, tolerance });
  }

  apply(context) {
    if (!this.enabled) return context;
    // GPU shader or pixel-level chroma key
    return context;
  }
}
```

### Custom Exporter using ITR

See [Custom exporter using ITR](#custom-exporter-using-itr) above for a full example.

---

## Roadmap

- [x] Core Timeline Engine (Project / Track / Clip / Asset)
- [x] Caption & Motion Typography Engine (22 animations, 19 effects, 10 presets)
- [x] Intermediate Timeline Representation (ITR)
- [x] Premiere Pro XMEML v5 exporter (full)
- [x] FCPXML 1.10 exporter (full)
- [x] CMX3600 EDL exporter (full)
- [x] MP4 render pipeline V1 (FFmpeg filter-complex, VideoClip + AudioClip, embedded audio)
- [x] `project.validate()` — pre-export validation API
- [x] `CropEffect` — edge crop with alignment, exported to MP4/Premiere/FCPXML/EDL/JSON
- [ ] Clip-type `fromJSON` registry (full round-trip deserialisation)
- [ ] MP4 V2 — gap padding, caption/text/shape rendering, transitions
- [ ] `node-canvas` / WebGL `CaptionRenderer` backend
- [ ] Color correction effect (LUT / curves)
- [ ] MCP server layer (separate package: `videoforge-mcp`)

---

## License

Copyright (c) 2026 Atul Taware

VideoForge is licensed under AGPL-3.0.

See LICENSE for details.
