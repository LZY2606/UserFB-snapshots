# VideoForge API Reference

Complete API documentation for VideoForge v0.9.0-alpha.1.

---

## Table of Contents

1. [Installation & Quick Start](#1-installation--quick-start)
2. [Project](#2-project)
3. [Timeline](#3-timeline)
4. [Track](#4-track)
5. [Clips](#5-clips)
   - [VideoClip](#51-videoclip)
   - [AudioClip](#52-audioclip)
   - [ImageClip](#53-imageclip)
   - [TextClip](#54-textclip)
   - [ShapeClip](#55-shapeclip)
6. [Effects](#6-effects)
   - [FadeEffect](#61-fadeeffect)
   - [Transition](#62-transition)
   - [CropEffect](#63-cropeffect)
   - [ChromaKeyEffect](#64-chromakeyeffect)
7. [Exporters](#7-exporters)
   - [Mp4Exporter](#71-mp4exporter)
   - [PremiereXmlExporter](#72-premierexmlexporter)
   - [FcpxmlExporter](#73-fcpxmlexporter)
   - [EdlExporter](#74-edlexporter)
   - [JsonExporter](#75-jsonexporter)
8. [Captions & Motion Typography](#8-captions--motion-typography)
   - [CaptionClip](#81-captionclip)
   - [CaptionStyle](#82-captionstyle)
   - [CaptionLayout](#83-captionlayout)
   - [CaptionAnimation](#84-captionanimation)
   - [CaptionEffect](#85-captioneffect)
   - [CaptionKeyframe / KeyframeSet](#86-captionkeyframe--keyframeset)
   - [CaptionPreset](#87-captionpreset)
   - [MotionTypographyEngine](#88-motiontypographyengine)
9. [Preview](#9-preview)
   - [PreviewPlayer](#91-previewplayer)
   - [PreviewRenderer](#92-previewrenderer)
10. [Constants](#10-constants)
11. [Utilities](#11-utilities)
12. [Complete Examples](#12-complete-examples)

---

## 1. Installation & Quick Start

```bash
npm install videoforge
```

```js
import {
  Project,
  Mp4Exporter,
  PremiereXmlExporter,
  FcpxmlExporter,
  EdlExporter,
  EXPORT_TYPES,
  TRACK_TYPES,
} from 'videoforge';
```

### Minimal example — export a clip to MP4

```js
import { Project } from 'videoforge';

const project = new Project({ name: 'My Film', fps: 30, width: 1920, height: 1080 });

const track = project.addTrack('video');
track.addVideo('/footage/interview.mp4', { inPoint: 0, outPoint: 60 });

await project.export({ type: 'mp4', output: 'output.mp4' });
```

---

## 2. Project

**`import { Project } from 'videoforge'`**

The root object. Holds a `Timeline`, an ordered list of `Track`s, and orchestrates export.

### Constructor

```js
new Project(options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `name` | `string` | `'Untitled Project'` | Human-readable project name |
| `fps` | `number` | `30` | Frames per second |
| `width` | `number` | `1920` | Canvas width (px) |
| `height` | `number` | `1080` | Canvas height (px) |
| `sampleRate` | `number` | `48000` | Audio sample rate (Hz) |
| `channels` | `number` | `2` | Audio channels (1 = mono, 2 = stereo) |

```js
const project = new Project({
  name: 'Documentary',
  fps: 24,
  width: 3840,
  height: 2160,
  sampleRate: 48000,
  channels: 2,
});
```

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `id` | `string` | Auto-generated unique ID |
| `name` | `string` | Project name (read/write) |
| `timeline` | `Timeline` | Temporal configuration |
| `metadata` | `object` | Free-form metadata (author, description, tags…) |
| `version` | `string` | Schema version (`'1.0.0'`) |
| `createdAt` | `Date` | Creation timestamp |
| `updatedAt` | `Date` | Last-modified timestamp |

### Methods

#### `addTrack(type?, options?): Track`

Create and append a new track. Returns the new `Track`.

```js
const videoTrack = project.addTrack('video');
const audioTrack = project.addTrack('audio', { name: 'Music' });
const textTrack  = project.addTrack('text');
```

Valid `type` values: `'video'` `'audio'` `'image'` `'text'` `'shape'`

---

#### `getTrack(id): Track | undefined`

Retrieve a track by its `id`.

```js
const track = project.getTrack('track_abc123');
```

`track(id)` is an alias for `getTrack(id)`.

---

#### `removeTrack(id): boolean`

Remove a track and all its clips. Returns `true` on success.

```js
project.removeTrack(track.id);
```

---

#### `getTracks(): Track[]`

Return all tracks in render-stack order (bottom → top).

```js
for (const track of project.getTracks()) {
  console.log(track.type, track.getClips().length);
}
```

---

#### `reorderTracks(orderedIds): Project`

Reorder tracks by specifying the new ID order. Chainable.

```js
project.reorderTracks([audioTrack.id, videoTrack.id, textTrack.id]);
```

---

#### `validate(options?): { valid, warnings, errors }`

Validate the project and return a report. Does **not** throw.

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `exporter` | `string` | — | Pass `'mp4'` to add MP4-specific checks |

```js
const report = project.validate({ exporter: 'mp4' });

if (!report.valid) {
  for (const err of report.errors) {
    console.error(`[${err.type}] ${err.message}`);
  }
}

for (const warn of report.warnings) {
  console.warn(`[${warn.type}] ${warn.message}`);
}
```

**Error types:** `MISSING_ASSET`, `INVALID_TRIM`, `NEGATIVE_DURATION`
**Warning types:** `EMPTY_TRACK`, `UNSUPPORTED_CLIP_TYPE`, `UNSUPPORTED_EFFECT`

---

#### `async export(options): Promise<string>`

Export the project. Resolves to the output file path.

| Option | Type | Required | Description |
|--------|------|----------|-------------|
| `type` | `string` | Yes | One of `EXPORT_TYPES.*` |
| `output` | `string` | Yes | Output file path |
| …exporter options | — | — | Passed through to the exporter |

```js
// MP4
await project.export({ type: 'mp4', output: 'out.mp4' });

// Premiere Pro XML
await project.export({ type: 'premiere', output: 'sequence.xml' });

// Final Cut Pro X
await project.export({ type: 'fcpxml', output: 'project.fcpxml' });

// CMX3600 EDL
await project.export({ type: 'edl', output: 'cut.edl' });

// JSON snapshot
await project.export({ type: 'json', output: 'project.vfp' });
```

---

#### `async save(filePath): Promise<string>`

Serialize the project to a `.vfp` JSON file.

```js
await project.save('./my-project.vfp');
```

---

#### `static async load(filePath): Promise<Project>`

Restore a project from a `.vfp` file.

```js
const project = await Project.load('./my-project.vfp');
```

---

#### `toJSON() / static fromJSON(data)`

Low-level serialization without disk I/O.

```js
const snapshot = project.toJSON();
const restored = Project.fromJSON(snapshot);
```

---

## 3. Timeline

`project.timeline` — read-only configuration for the sequence.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `fps` | `number` | Frames per second |
| `width` | `number` | Canvas width (px) |
| `height` | `number` | Canvas height (px) |
| `sampleRate` | `number` | Audio sample rate (Hz) |
| `channels` | `number` | Audio channel count |

### Methods

#### `getTotalDuration(): number`

Total playback duration in seconds (end time of the last clip across all tracks).

```js
console.log('Duration:', project.timeline.getTotalDuration(), 's');
```

---

#### `getTotalFrames(): number`

Duration expressed as a frame count.

```js
const frames = project.timeline.getTotalFrames();
```

---

#### `timeToFrame(time) / frameToTime(frame)`

Convert between seconds and frame numbers.

```js
const frame = project.timeline.timeToFrame(10.5);   // → 315 at 30fps
const time  = project.timeline.frameToTime(315);    // → 10.5
```

---

#### `getClipsAtTime(time): Clip[]`

All clips (across all tracks) that are active at the given second.

```js
const activeClips = project.timeline.getClipsAtTime(5.0);
```

---

#### `getClipsInRange(startTime, endTime): Clip[]`

Clips active within the half-open interval `[startTime, endTime)`.

---

#### `findOverlaps(): Array<[Clip, Clip]>`

Detect clips on the same track that overlap in time.

```js
const overlaps = project.timeline.findOverlaps();
if (overlaps.length) console.warn('Overlapping clips detected');
```

---

## 4. Track

Returned by `project.addTrack()`. Groups clips of a single type.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `id` | `string` | Unique identifier |
| `type` | `string` | Track type (`'video'`, `'audio'`, etc.) |
| `name` | `string` | Human-readable label |
| `volume` | `number` | Master volume for this track (0–2) |
| `muted` | `boolean` | Track-level mute |
| `solo` | `boolean` | Solo mode |
| `locked` | `boolean` | Prevent edits |
| `visible` | `boolean` | Visibility in preview |

### Clip Factory Methods

#### `addVideo(path, options?): VideoClip`

```js
const clip = track.addVideo('/footage/clip.mp4', {
  startTime: 0,      // position on timeline (seconds)
  inPoint:   5,      // source in-point (seconds)
  outPoint:  15,     // source out-point (seconds)
});
```

---

#### `addAudio(path, options?): AudioClip`

```js
const music = track.addAudio('/audio/theme.mp3', {
  startTime: 0,
  inPoint:   0,
  outPoint:  120,
});
```

---

#### `addImage(path, options?): ImageClip`

```js
const logo = track.addImage('/assets/logo.png', {
  startTime: 10,
  outPoint:  15,    // how long to hold the image (seconds)
});
```

---

#### `addText(text, options?): TextClip`

```js
const title = track.addText('Chapter One', {
  startTime: 0,
  outPoint:  5,
});
```

---

#### `addShape(shapeType, options?): ShapeClip`

```js
const bar = track.addShape('rectangle', {
  startTime: 0,
  outPoint:  10,
});
```

Valid `shapeType` values: `'rectangle'` `'ellipse'` `'triangle'` `'line'` `'polygon'` `'arrow'`

---

#### `addCaption(text?, options?): CaptionClip`

Creates a full Motion Typography Engine caption clip.

```js
const caption = track.addCaption('Welcome to VideoForge', {
  startTime: 0,
  outPoint:  5,
  preset: 'hormozi',   // optional built-in preset name
});
```

---

### Clip Management Methods

#### `getClips(): Clip[]`

Returns all clips sorted by `startTime`.

```js
for (const clip of track.getClips()) {
  console.log(clip.id, clip.startTime, '→', clip.endTime);
}
```

---

#### `getClip(id): Clip | undefined`

```js
const clip = track.getClip('clip_xyz');
```

---

#### `removeClip(id): boolean`

```js
track.removeClip(clip.id);
```

---

#### `moveClip(id, startTime): boolean`

Reposition a clip on the timeline.

```js
track.moveClip(clip.id, 30.0);  // move to 30 s
```

---

#### `getDuration(): number`

End time of the last clip on this track.

---

## 5. Clips

All clip types inherit from `Clip`. Common properties and methods are listed first, then type-specific ones.

### Common Clip Properties

| Property | Type | Description |
|----------|------|-------------|
| `id` | `string` | Unique identifier |
| `type` | `string` | Clip type string |
| `name` | `string` | Human-readable label |
| `asset` | `Asset` | Source media asset |
| `startTime` | `number` | Position on timeline (seconds) |
| `inPoint` | `number` | Source in-point (seconds) |
| `outPoint` | `number` | Source out-point (seconds) |
| `duration` | `number` | *(computed)* `outPoint - inPoint` |
| `endTime` | `number` | *(computed)* `startTime + duration` |
| `effects` | `Effect[]` | Ordered effect chain |
| `muted` | `boolean` | Whether audio is muted |
| `locked` | `boolean` | Prevent edits |
| `visible` | `boolean` | Visibility in preview |

### Common Clip Methods

#### `trim(inPoint, outPoint): Clip`

Restrict the source portion used. Chainable.

```js
clip.trim(2, 8);   // use seconds 2–8 of the source media
```

---

#### `move(startTime): Clip`

Reposition on the timeline. Chainable.

```js
clip.move(30);   // place at 30 s on the timeline
```

---

#### `split(time): { head, tail }`

Split into two clips at an absolute timeline time. The original clip becomes `head`; a new clip is returned as `tail`.

```js
const { head, tail } = clip.split(10);
// head: startTime → 10 s
// tail: 10 s → original end
```

---

#### `copy(): Clip`

Deep copy with a new ID (effects are also cloned).

```js
const clone = clip.copy();
track.addClip(clone);
```

---

#### `remove(): boolean`

Remove this clip from its parent track.

```js
clip.remove();
```

---

#### `fadeIn(duration?, options?): Clip`

Append a fade-in effect. Chainable.

```js
clip.fadeIn(0.5);                         // 0.5-second fade in
clip.fadeIn(1, { easing: 'easeIn' });
```

---

#### `fadeOut(duration?, options?): Clip`

Append a fade-out effect. Chainable.

```js
clip.fadeOut(1);
```

---

#### `addCrop(options?): Clip`

Append a `CropEffect` that removes pixels from one or more edges and places the remaining content within the original canvas at the given alignment. Chainable.

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `top` | `number` | `0` | Pixels to remove from the top edge |
| `bottom` | `number` | `0` | Pixels to remove from the bottom edge |
| `left` | `number` | `0` | Pixels to remove from the left edge |
| `right` | `number` | `0` | Pixels to remove from the right edge |
| `alignment` | `string` | `'center'` | Where to place the cropped content — one of `CROP_ALIGNMENT.*` |

```js
import { CROP_ALIGNMENT } from 'videoforge';

// Crop 100 px from top and bottom; center the result
clip.addCrop({ top: 100, bottom: 100 });

// Crop sides and pin result to the left edge
clip.addCrop({ left: 300, right: 300, alignment: CROP_ALIGNMENT.LEFT });

// Chain with other effects
clip.addCrop({ top: 80, alignment: CROP_ALIGNMENT.TOP }).fadeIn(0.5);
```

---

#### `addChromaKey(options?): Clip`

Append a `ChromaKeyEffect` that removes a solid background colour (green screen, blue screen, etc.), making matching pixels transparent. Chainable.

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `color` | `string` | `'#00FF00'` | Background colour to remove |
| `similarity` | `number` | `0.10` | Colour match tolerance, `0`–`1` |
| `blend` | `number` | `0.10` | Softness of the key edge, `0`–`1` |
| `yuv` | `boolean` | `false` | Match colour similarity in YUV space instead of RGB |

```js
// Remove a standard green screen background
clip.addChromaKey();

// Remove a blue screen with a wider tolerance
clip.addChromaKey({ color: '#0000FF', similarity: 0.25, blend: 0.05 });
```

---

#### `addEffect(effect): Clip` / `removeEffect(id): boolean` / `getEffect(id): Effect`

Manually manage the effect chain.

```js
import { FadeEffect } from 'videoforge';

const fade = new FadeEffect('in', 0.5);
clip.addEffect(fade);
clip.removeEffect(fade.id);
```

---

### 5.1 VideoClip

Returned by `track.addVideo()`.

#### Constructor options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `startTime` | `number` | `0` | Timeline position (s) |
| `inPoint` | `number` | `0` | Source in-point (s) |
| `outPoint` | `number` | `null` | Source out-point (s, defaults to asset duration) |

#### Extra properties

| Property | Type | Description |
|----------|------|-------------|
| `isReversed` | `boolean` | Whether playback is reversed |
| `isMuted` | `boolean` | Whether audio is muted |

#### Methods

##### `volume(value?): number | VideoClip`

Get or set volume (0–2). Chainable when setting.

```js
clip.volume();        // → 1.0
clip.volume(0.8);     // → VideoClip (chainable)
```

---

##### `mute() / unmute(): VideoClip`

Silence or restore audio. Chainable.

```js
clip.mute();
clip.unmute();
```

---

##### `speed(rate?): number | VideoClip`

Get or set playback speed. Chainable when setting. Speed `2` plays at double speed; `0.5` at half speed.

```js
clip.speed();      // → 1
clip.speed(2);     // double speed
clip.speed(0.5);   // slow motion
```

---

##### `reverse(value?): VideoClip`

Toggle or set reversed playback. Chainable.

```js
clip.reverse(true);   // play backwards
clip.reverse(false);  // restore forward playback
clip.reverse();       // toggle
```

---

#### Full VideoClip example

```js
const vt = project.addTrack('video');

const clip = vt.addVideo('/footage/interview.mp4', { inPoint: 10, outPoint: 40 })
  .volume(0.9)
  .speed(1)
  .fadeIn(0.5)
  .fadeOut(1);

// Slow-motion reverse segment
const slowReverse = vt.addVideo('/footage/action.mp4', { startTime: 40, inPoint: 0, outPoint: 5 })
  .speed(0.25)
  .reverse(true)
  .fadeIn(0.3);
```

---

### 5.2 AudioClip

Returned by `track.addAudio()`.

#### Extra properties

| Property | Type | Description |
|----------|------|-------------|
| `isMuted` | `boolean` | Whether this clip is muted |

#### Methods

##### `volume(value?): number | AudioClip`

Get or set volume (0–2). Chainable when setting.

```js
music.volume(0.6);   // 60% volume
```

---

##### `pan(value?): number | AudioClip`

Get or set stereo pan. `-1` = full left, `0` = centre, `+1` = full right. Chainable when setting.

```js
narration.pan(-0.3);   // slightly left
```

---

##### `speed(rate?): number | AudioClip`

Get or set playback speed. Chainable when setting.

```js
music.speed(1.05);   // slightly sped up
```

---

##### `mute() / unmute(): AudioClip`

Chainable.

```js
sfx.mute();
```

---

#### Full AudioClip example

```js
const at = project.addTrack('audio', { name: 'Music' });

const music = at.addAudio('/audio/theme.mp3', { inPoint: 0, outPoint: 180 })
  .volume(0.5)
  .pan(0)
  .fadeIn(2)
  .fadeOut(3);

const narr = project.addTrack('audio', { name: 'VO' })
  .addAudio('/audio/narration.wav', { inPoint: 0, outPoint: 120 })
  .volume(1.0)
  .pan(-0.1);
```

---

### 5.3 ImageClip

Returned by `track.addImage()`.

Images hold for 5 seconds by default; set `outPoint` to change.

#### Methods

##### `position(x?, y?): { x, y } | ImageClip`

Get or set canvas position (pixels). Chainable when setting.

```js
logo.position(50, 50);          // top-left corner
logo.position();                 // → { x: 50, y: 50 }
```

---

##### `scale(x?, y?): { x, y } | ImageClip`

Get or set scale. Omitting `y` scales uniformly. Chainable when setting.

```js
logo.scale(0.5);          // 50% uniform
logo.scale(2, 1.5);       // 200% wide, 150% tall
```

---

##### `rotation(degrees?): number | ImageClip`

Get or set rotation in degrees (clockwise). Chainable when setting.

```js
logo.rotation(45);
```

---

##### `opacity(value?): number | ImageClip`

Get or set opacity (0–1). Chainable when setting.

```js
watermark.opacity(0.3);
```

---

#### Full ImageClip example

```js
const it = project.addTrack('image', { name: 'Overlays' });

const logo = it.addImage('/assets/logo.png', { startTime: 0, outPoint: 300 })
  .position(1800, 30)
  .scale(0.4)
  .opacity(0.85)
  .fadeIn(0.5)
  .fadeOut(0.5);
```

---

### 5.4 TextClip

Returned by `track.addText()`.

#### Extra properties

| Property | Type | Description |
|----------|------|-------------|
| `text` | `string` | Text content (read/write) |

#### Methods

##### `font(name?): string | TextClip`

Get or set the font family. Chainable when setting.

```js
title.font('Helvetica Neue');
```

---

##### `fontSize(size?): number | TextClip`

Get or set font size in points. Chainable when setting.

```js
title.fontSize(96);
```

---

##### `color(value?): string | TextClip`

Get or set text colour (CSS hex or colour name). Chainable when setting.

```js
title.color('#FFFFFF');
title.color('rgba(255, 255, 0, 0.9)');
```

---

##### `background(value?): string | TextClip`

Get or set background colour. Chainable when setting.

```js
title.background('rgba(0, 0, 0, 0.6)');
```

---

##### `align(value?): string | TextClip`

Get or set text alignment. Values: `'left'` `'center'` `'right'` `'justify'`. Chainable when setting.

```js
title.align('center');
```

---

##### `bold(value?): boolean | TextClip`

Get or set bold. Chainable when setting.

```js
title.bold(true);
```

---

##### `italic(value?): boolean | TextClip`

Get or set italic. Chainable when setting.

```js
credit.italic(true);
```

---

##### `position(x?, y?): { x, y } | TextClip`

Get or set canvas position. Chainable when setting.

```js
title.position(960, 900);   // centred near bottom (at 1920×1080)
```

---

##### `opacity(value?): number | TextClip`

Get or set opacity (0–1). Chainable when setting.

```js
subtitle.opacity(0.9);
```

---

#### Full TextClip example

```js
const tt = project.addTrack('text', { name: 'Titles' });

const mainTitle = tt.addText('Chapter One', { startTime: 2, outPoint: 7 })
  .font('Montserrat')
  .fontSize(120)
  .color('#FFFFFF')
  .bold(true)
  .align('center')
  .position(960, 540)
  .fadeIn(0.5)
  .fadeOut(0.5);

const subtitle = tt.addText('The Beginning', { startTime: 3, outPoint: 7 })
  .font('Montserrat')
  .fontSize(48)
  .color('#CCCCCC')
  .italic(true)
  .position(960, 620)
  .fadeIn(0.8);
```

---

### 5.5 ShapeClip

Returned by `track.addShape()`.

Valid `shapeType` values: `'rectangle'` `'ellipse'` `'triangle'` `'line'` `'polygon'` `'arrow'`

#### Extra properties

| Property | Type | Description |
|----------|------|-------------|
| `shapeType` | `string` | Shape type identifier |

#### Methods

##### `position(x?, y?): { x, y } | ShapeClip`

Get or set canvas position. Chainable.

---

##### `size(width?, height?): { width, height } | ShapeClip`

Get or set bounding-box dimensions. Chainable.

```js
bar.size(1920, 8);   // full-width horizontal bar, 8 px tall
```

---

##### `fillColor(value?): string | ShapeClip`

Get or set fill colour. Chainable.

```js
dot.fillColor('#FF0000');
```

---

##### `strokeColor(value?): string | ShapeClip`

Get or set stroke colour. Chainable.

```js
frame.strokeColor('#FFFFFF');
```

---

##### `strokeWidth(value?): number | ShapeClip`

Get or set stroke width in pixels. Chainable.

```js
frame.strokeWidth(4);
```

---

##### `opacity(value?): number | ShapeClip`

Get or set opacity (0–1). Chainable.

```js
overlay.opacity(0.5);
```

---

##### `rotation(degrees?): number | ShapeClip`

Get or set rotation in degrees (clockwise). Chainable.

```js
diamond.rotation(45);
```

---

##### `cornerRadius(value?): number | ShapeClip`

Get or set corner radius (rectangles only). Chainable.

```js
card.cornerRadius(16);
```

---

#### Full ShapeClip example

```js
const st = project.addTrack('shape', { name: 'Graphics' });

// Rounded card background
const card = st.addShape('rectangle', { startTime: 0, outPoint: 10 })
  .position(160, 800)
  .size(600, 120)
  .fillColor('rgba(0, 0, 0, 0.75)')
  .cornerRadius(12)
  .opacity(0.9)
  .fadeIn(0.3);

// Accent circle
const dot = st.addShape('ellipse', { startTime: 0, outPoint: 10 })
  .position(200, 840)
  .size(40, 40)
  .fillColor('#FF3B30');
```

---

## 6. Effects

### 6.1 FadeEffect

**`import { FadeEffect } from 'videoforge'`**

```js
new FadeEffect(direction, duration?, options?)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `direction` | `'in' \| 'out'` | — | Fade direction |
| `duration` | `number` | `1` | Duration in seconds |
| `options.easing` | `string` | `'linear'` | Easing curve |
| `options.fromOpacity` | `number` | `null` | Start opacity (null = automatic) |
| `options.toOpacity` | `number` | `null` | End opacity (null = automatic) |

```js
import { FadeEffect, EASING } from 'videoforge';

const fadeIn  = new FadeEffect('in',  1,   { easing: EASING.EASE_IN });
const fadeOut = new FadeEffect('out', 0.5, { easing: EASING.EASE_OUT });

clip.addEffect(fadeIn).addEffect(fadeOut);

// Easier shorthand — identical result:
clip.fadeIn(1, { easing: 'easeIn' }).fadeOut(0.5, { easing: 'easeOut' });
```

#### Methods

| Method | Description |
|--------|-------------|
| `enable() / disable()` | Toggle effect; chainable |
| `setParams(updates)` | Merge parameter updates; chainable |
| `getOpacityAt(t)` | Compute opacity at offset `t` within the fade (0 ≤ t ≤ duration) |

---

### 6.2 Transition

**`import { Transition } from 'videoforge'`**

```js
new Transition(transitionType?, duration?, options?)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `transitionType` | `string` | `'crossDissolve'` | Transition style |
| `duration` | `number` | `1` | Duration in seconds |
| `options.easing` | `string` | `'easeInOut'` | Easing curve |
| `options.fromClipId` | `string` | `null` | Outgoing clip ID |
| `options.toClipId` | `string` | `null` | Incoming clip ID |

Valid `transitionType` values: `'crossDissolve'` `'wipeLeft'` `'wipeRight'` `'wipeUp'` `'wipeDown'` `'slide'` `'zoom'` `'dipToBlack'` `'dipToWhite'`

```js
import { Transition, TRANSITION_TYPES } from 'videoforge';

const dissolve = new Transition(TRANSITION_TYPES.CROSS_DISSOLVE, 1.5);
dissolve.link(clipA, clipB);

clipA.addEffect(dissolve);
```

> **Note:** Transitions are exported in Premiere XML and FCPXML but are not rendered in MP4 export (a warning is issued).

---

### 6.3 CropEffect

**`import { CropEffect, CROP_ALIGNMENT } from 'videoforge'`**

Removes pixels from any combination of edges and positions the remaining content within the original canvas using a 9-point alignment grid.

```js
new CropEffect(params?)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `params.top` | `number` | `0` | Pixels to remove from the top |
| `params.bottom` | `number` | `0` | Pixels to remove from the bottom |
| `params.left` | `number` | `0` | Pixels to remove from the left |
| `params.right` | `number` | `0` | Pixels to remove from the right |
| `params.alignment` | `string` | `'center'` | Content placement after crop — one of `CROP_ALIGNMENT.*` |

#### Alignment values (`CROP_ALIGNMENT`)

| Constant | Value | Description |
|----------|-------|-------------|
| `CROP_ALIGNMENT.CENTER` | `'center'` | Content centred in the original frame (default) |
| `CROP_ALIGNMENT.TOP` | `'top'` | Content pinned to the top edge |
| `CROP_ALIGNMENT.BOTTOM` | `'bottom'` | Content pinned to the bottom edge |
| `CROP_ALIGNMENT.LEFT` | `'left'` | Content pinned to the left edge |
| `CROP_ALIGNMENT.RIGHT` | `'right'` | Content pinned to the right edge |
| `CROP_ALIGNMENT.TOP_LEFT` | `'topLeft'` | Content pinned to the top-left corner |
| `CROP_ALIGNMENT.TOP_RIGHT` | `'topRight'` | Content pinned to the top-right corner |
| `CROP_ALIGNMENT.BOTTOM_LEFT` | `'bottomLeft'` | Content pinned to the bottom-left corner |
| `CROP_ALIGNMENT.BOTTOM_RIGHT` | `'bottomRight'` | Content pinned to the bottom-right corner |

#### Accessors

All crop values and alignment are readable and writable via getters/setters:

```js
const fx = new CropEffect({ top: 100, bottom: 100 });
fx.top;           // → 100
fx.bottom = 150;  // update at any time
fx.alignment = CROP_ALIGNMENT.TOP;
```

#### Methods

| Method | Description |
|--------|-------------|
| `enable() / disable()` | Toggle effect; chainable |
| `setParams(updates)` | Merge parameter updates; chainable |
| `toJSON()` | Serialize to plain object |
| `static fromJSON(data)` | Restore a `CropEffect` from serialised data |

#### Export behaviour per format

| Format | How crop is encoded |
|--------|---------------------|
| **MP4 (FFmpeg)** | `crop=iw-L-R:ih-T-B:L:T` then `pad=…:black` with offset per alignment |
| **Premiere XML** | Built-in `Crop` video effect with `left/right/top/bottom` as canvas-percentage values; alignment shifts the `Basic Motion` centre point |
| **FCPXML** | `<adjust-crop mode="trim">` with normalised (0–1) values; alignment adds `<adjust-transform position>` |
| **EDL** | `* CROP: left=Xpx right=Xpx top=Xpx bottom=Xpx alignment=Y [not representable in EDL]` comment |
| **JSON** | Fully serialised in the clip's `effects` array |

#### Full example

```js
import { Project, CropEffect, CROP_ALIGNMENT } from 'videoforge';

const project = new Project({ name: 'Crop Demo', fps: 30, width: 1920, height: 1080 });
const vt = project.addTrack('video');

// Crop a 16:9 clip to a 9:16 vertical frame, centred
const clip = vt.addVideo('/footage/wide.mp4', { inPoint: 0, outPoint: 30 });
const verticalCrop = (1920 - 607) / 2;   // keep 607 px wide → 9:16 at 1080p
clip.addCrop({
  left:      verticalCrop,
  right:     verticalCrop,
  alignment: CROP_ALIGNMENT.CENTER,
});

// Export — crop applied in all formats
await project.export({ type: 'mp4',     output: './out.mp4' });
await project.export({ type: 'premiere', output: './out.xml' });
await project.export({ type: 'fcpxml',  output: './out.fcpxml' });
```

> **Shorthand:** `clip.addCrop({ ... })` is equivalent to `clip.addEffect(new CropEffect({ ... }))`.

---

### 6.4 ChromaKeyEffect

**`import { ChromaKeyEffect } from 'videoforge'`**

Removes a solid background colour (green screen, blue screen, or any other flat backdrop) from a video clip, making matching pixels transparent.

```js
new ChromaKeyEffect(params?)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `params.color` | `string` | `'#00FF00'` | Background colour to remove (hex or CSS/FFmpeg colour name) |
| `params.similarity` | `number` | `0.10` | Colour match tolerance, `0`–`1`. Higher values key out a wider range of shades |
| `params.blend` | `number` | `0.10` | Softness of the key edge, `0`–`1`. Higher values give a smoother transition between opaque and transparent pixels |
| `params.yuv` | `boolean` | `false` | Match colour similarity in YUV space instead of RGB — often cleaner on compressed source footage |

#### Accessors

All chroma key values are readable and writable via getters/setters; `similarity` and `blend` throw a `RangeError` outside `[0, 1]`:

```js
const fx = new ChromaKeyEffect({ color: '#00FF00' });
fx.similarity;          // → 0.10
fx.similarity = 0.25;   // update at any time
fx.color = '#0000FF';   // switch to a blue screen
```

#### Methods

| Method | Description |
|--------|-------------|
| `enable() / disable()` | Toggle effect; chainable |
| `setParams(updates)` | Merge parameter updates; chainable |
| `toJSON()` | Serialize to plain object |
| `static fromJSON(data)` | Restore a `ChromaKeyEffect` from serialised data |

#### Export behaviour per format

| Format | How chroma key is encoded |
|--------|----------------------------|
| **MP4 (FFmpeg)** | `chromakey=color=0xRRGGBB:similarity=S:blend=B:yuv=0\|1` filter applied per clip |
| **Premiere XML / FCPXML / EDL** | Passed through as a generic effect/filter entry in the clip's effect list (see each exporter's generic effect handling) |
| **JSON** | Fully serialised in the clip's `effects` array |

> Keyed-out pixels become transparent (an alpha channel is added). For the transparency to survive into the exported MP4, pair the effect with an alpha-capable codec/pixel format via `project.export({ type: 'mp4', pixelFormat: 'yuva420p', videoCodec: 'qtrle' })` (or similar) — the default `libx264`/`yuv420p` combination has no alpha channel and will flatten keyed-out areas to black.

#### Full example

```js
import { Project, ChromaKeyEffect } from 'videoforge';

const project = new Project({ name: 'Green Screen Demo', fps: 30, width: 1920, height: 1080 });
const vt = project.addTrack('video');

const clip = vt.addVideo('/footage/greenscreen.mp4', { inPoint: 0, outPoint: 30 });
clip.addChromaKey({
  color:      '#00FF00',
  similarity: 0.15,
  blend:      0.05,
});

await project.export({ type: 'mp4', output: './out.mp4' });
```

> **Shorthand:** `clip.addChromaKey({ ... })` is equivalent to `clip.addEffect(new ChromaKeyEffect({ ... }))`.

---

## 7. Exporters

Exporters can be used directly or via `project.export({ type })`.

### 7.1 Mp4Exporter

**`import { Mp4Exporter } from 'videoforge'`**

Requires **FFmpeg** to be installed and available in `PATH`.

```js
new Mp4Exporter(project, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `preset` | `string` | `'medium'` | FFmpeg preset (`ultrafast` → `veryslow`) |
| `crf` | `number` | `23` | Constant Rate Factor (0 = lossless, 51 = worst) |
| `videoBitrate` | `string` | — | Target video bitrate (e.g. `'8000k'`) |
| `audioBitrate` | `string` | `'192k'` | Target audio bitrate |
| `onProgress` | `(pct: number) => void` | — | Progress callback (0–100) |

```js
import { Project, Mp4Exporter } from 'videoforge';

const project = new Project({ name: 'Export', fps: 30, width: 1920, height: 1080 });
const vt = project.addTrack('video');
vt.addVideo('/footage/clip.mp4', { inPoint: 0, outPoint: 30 });

const exporter = new Mp4Exporter(project, {
  preset: 'fast',
  crf: 22,
  onProgress: (pct) => process.stdout.write(`\rEncoding: ${pct.toFixed(1)}%`),
});

const result = await exporter.export('/output/final.mp4');
// result: { success: true, outputPath: '/output/final.mp4', fileSize: 12345678, duration: 30 }
console.log('\nDone! Size:', result.fileSize, 'bytes');
```

#### Supported features

| Feature | Supported |
|---------|-----------|
| VideoClip (trim, speed, reverse, fade) | Yes |
| AudioClip (volume, fade) | Yes |
| Embedded audio in video clips | Yes (auto-detected via ffprobe) |
| Multiple video clips (concatenated) | Yes |
| ImageClip / TextClip / ShapeClip | Skipped with warning |
| Transitions | Ignored with warning |

#### Embedded audio detection

If you know a video file has audio, set `asset.audioChannels` to skip the ffprobe probe:

```js
const clip = vt.addVideo('/footage/interview.mp4', { inPoint: 0, outPoint: 60 });
clip.asset.audioChannels = 2;   // skip auto-detection
```

If `audioChannels` is `0` (default), the exporter probes the file automatically.

---

### 7.2 PremiereXmlExporter

**`import { PremiereXmlExporter } from 'videoforge'`**

Exports an Adobe Premiere Pro FCP7-compatible XML sequence.

```js
new PremiereXmlExporter(project, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `pretty` | `boolean` | `true` | Pretty-print XML |
| `validateInput` | `boolean` | `true` | Validate project before export |
| `validateOutput` | `boolean` | `true` | Validate generated XML |
| `includeVfMetadata` | `boolean` | `true` | Embed VideoForge metadata in `vf:` namespace |
| `sequenceName` | `string` | `null` | Override sequence name |

```js
import { PremiereXmlExporter } from 'videoforge';

const exporter = new PremiereXmlExporter(project, { sequenceName: 'Act 1' });
await exporter.export('/output/sequence.xml');

// Or generate the XML string without writing to disk:
const xml = exporter.toString();
```

---

### 7.3 FcpxmlExporter

**`import { FcpxmlExporter } from 'videoforge'`**

Exports an Apple Final Cut Pro X `.fcpxml` file.

```js
new FcpxmlExporter(project, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `fcpxmlVersion` | `string` | `'1.10'` | FCPXML schema version |
| `pretty` | `boolean` | `true` | Pretty-print XML |
| `validateInput` | `boolean` | `true` | Validate project before export |
| `validateOutput` | `boolean` | `true` | Validate generated XML |
| `includeVfMetadata` | `boolean` | `true` | Embed VideoForge metadata |
| `libraryName` | `string` | `null` | FCP library name |
| `eventName` | `string` | `null` | FCP event name |

```js
import { FcpxmlExporter } from 'videoforge';

const exporter = new FcpxmlExporter(project, {
  libraryName: 'Documentary Library',
  eventName:   '2024-06-04 Shoot',
});
await exporter.export('/output/project.fcpxml');

const xml = exporter.toString();  // no disk write
```

---

### 7.4 EdlExporter

**`import { EdlExporter } from 'videoforge'`**

Exports a CMX3600-compatible Edit Decision List.

```js
new EdlExporter(project, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `title` | `string` | `null` | EDL title (defaults to project name) |
| `dropFrame` | `boolean` | `null` | `null` = auto-detect from fps |
| `includeAudio` | `boolean` | `true` | Include audio events |
| `includeComments` | `boolean` | `true` | Include `* FROM CLIP NAME` comments |
| `validateInput` | `boolean` | `true` | Validate project before export |

```js
import { EdlExporter } from 'videoforge';

const edl = new EdlExporter(project, { title: 'Final Cut v3' });
await edl.export('/output/cut.edl');

// Or get the EDL string directly:
const edlString = edl.toString();
console.log(edlString);
```

---

### 7.5 JsonExporter

**`import { JsonExporter } from 'videoforge'`**

Serializes the full project to a `.vfp` JSON file. Useful for saving/restoring work between sessions, and for debugging.

```js
new JsonExporter(project, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `pretty` | `boolean` | `true` | Pretty-print JSON |
| `indent` | `number` | `2` | Indentation level |

```js
import { JsonExporter } from 'videoforge';

const exporter = new JsonExporter(project);
await exporter.export('/output/project.vfp');

// In-memory only:
const obj = exporter.toObject();   // plain JS object
const str = exporter.toString();   // JSON string
```

---

## 8. Captions & Motion Typography

VideoForge ships a full Motion Typography Engine. Captions support per-word timing, karaoke-style animations, effects, keyframes, and built-in presets for social platforms.

### 8.1 CaptionClip

**`import { CaptionClip } from 'videoforge'`**

Usually created via `track.addCaption()`.

```js
new CaptionClip(asset?, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `startTime` | `number` | `0` | Timeline position (s) |
| `outPoint` | `number` | `30` | End time (s) |
| `style` | `CaptionStyle \| object` | — | Override master style |
| `layout` | `CaptionLayout \| object` | — | Override layout |
| `name` | `string` | `'Caption'` | Clip label |

#### Properties

| Property | Type | Description |
|----------|------|-------------|
| `transcript` | `string` | Raw transcript text |
| `segments` | `CaptionSegment[]` | Display segments |
| `style` | `CaptionStyle` | Master style (inherited by children) |
| `layout` | `CaptionLayout` | Position and wrap settings |
| `captionAnimations` | `object[]` | Clip-level animation chain |
| `presetName` | `string \| null` | Applied preset name |

#### `setTranscript(text, options?): CaptionClip`

Set the transcript and auto-build segments. Chainable.

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `wordTimings` | `Array<{word,start,end}>` | `null` | Per-word timing |
| `maxWordsPerSegment` | `number` | `5` | Words per display segment |
| `autoSegment` | `boolean` | `true` | Auto-split into segments |

```js
const caption = track.addCaption();
caption.setTranscript("This is a test of VideoForge captions", {
  maxWordsPerSegment: 4,
});

// With word-level timing (e.g. from Whisper)
caption.setTranscript("Hello world", {
  wordTimings: [
    { word: 'Hello', start: 0.0, end: 0.4 },
    { word: 'world', start: 0.5, end: 0.9 },
  ],
});
```

---

#### `applyPreset(preset): CaptionClip`

Apply a built-in preset by name or by `CaptionPreset` instance. Chainable.

Built-in preset names: `'hormozi'` `'mrbeast'` `'podcast'` `'news'` `'documentary'` `'karaoke'` `'minimal'` `'gaming'` `'luxury'` `'corporate'`

```js
caption.applyPreset('hormozi');   // bold ALL-CAPS with yellow highlights
caption.applyPreset('podcast');   // minimal centered style
```

---

#### Animation helpers

These methods add animations to all elements at their respective granularity.

```js
caption
  .animateCaption('fade',       { duration: 0.3 })     // whole clip fades in/out
  .animateLines('slide',        { duration: 0.4 })     // each segment slides
  .animateWords('pop',          { stagger: 0.05 })     // words pop in with stagger
  .animateCharacters('rotate',  { stagger: 0.02 });    // chars rotate in with stagger
```

---

#### `addAnimation(animation): CaptionClip`

Add a `CaptionAnimation` instance to the clip-level chain. Chainable.

```js
import { FadeAnimation } from 'videoforge';

caption.addAnimation(new FadeAnimation({ duration: 0.5, delay: 0 }));
```

---

#### Full CaptionClip example

```js
import { Project, CaptionStyle } from 'videoforge';

const project = new Project({ name: 'Reel', fps: 30, width: 1080, height: 1920 });
const vt = project.addTrack('video');
vt.addVideo('/footage/clip.mp4', { inPoint: 0, outPoint: 15 });

const ct = project.addTrack('video', { name: 'Captions' });

const caption = ct.addCaption('This changed everything', {
  startTime: 1,
  outPoint: 5,
})
  .applyPreset('hormozi')
  .setTranscript('This changed everything', {
    maxWordsPerSegment: 3,
    wordTimings: [
      { word: 'This',     start: 0.0, end: 0.3 },
      { word: 'changed',  start: 0.4, end: 0.8 },
      { word: 'everything', start: 0.9, end: 1.5 },
    ],
  });
```

---

### 8.2 CaptionStyle

Describes the visual appearance of caption text. Inherits down the hierarchy (CaptionClip → CaptionSegment → CaptionWord → CaptionCharacter).

**`import { CaptionStyle } from 'videoforge'`**

```js
const style = new CaptionStyle({
  fontFamily:    'Montserrat',
  fontWeight:    900,
  fontSize:      64,
  fill:          '#FFFFFF',
  textAlign:     'center',
});
```

#### Key properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `fontFamily` | `string` | `'Arial'` | Font family |
| `fontWeight` | `number\|string` | `700` | Font weight |
| `fontSize` | `number` | `48` | Font size (px) |
| `fontStyle` | `string` | `'normal'` | `'normal'` or `'italic'` |
| `fill` | `string` | `'#FFFFFF'` | Text colour |
| `stroke` | `object\|null` | `null` | `{ color, width, join }` |
| `shadow` | `object\|null` | `null` | `{ color, offsetX, offsetY, blur }` |
| `glow` | `object\|null` | `null` | `{ color, blur, strength }` |
| `background` | `object\|null` | `null` | `{ color, padding, borderRadius, opacity }` |
| `letterSpacing` | `number` | `0` | Letter spacing (px) |
| `lineHeight` | `number` | `1.2` | Line height multiplier |
| `textAlign` | `string` | `'center'` | Text alignment |
| `underline` | `boolean` | `false` | Underline |
| `strikethrough` | `boolean` | `false` | Strikethrough |

#### Methods

```js
const style = new CaptionStyle({ fontSize: 64 })
  .setStroke('#000000', 3, 'round')
  .setShadow('rgba(0,0,0,0.8)', 2, 2, 8)
  .setGlow('#FFD700', 12, 3);

// Merge two styles (returns a new CaptionStyle):
const merged = baseStyle.merge(overrideStyle);
const copy   = style.clone();
```

---

### 8.3 CaptionLayout

Controls where and how captions are positioned on the canvas.

**`import { CaptionLayout, ANCHOR_POINT, WRAP_MODE } from 'videoforge'`**

```js
const layout = new CaptionLayout({
  x:       0.5,                        // normalised horizontal (0–1)
  y:       0.85,                       // normalised vertical (0–1)
  anchor:  'bottomCenter',
  wrapMode: 'word',
  maxWordsPerLine: 5,
});
```

#### Key properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `x` | `number` | `0.5` | Horizontal position (0–1) |
| `y` | `number` | `0.85` | Vertical position (0–1) |
| `anchor` | `string` | `'bottomCenter'` | `ANCHOR_POINT.*` |
| `wrapMode` | `string` | `'word'` | `WRAP_MODE.*` |
| `maxWordsPerLine` | `number` | `6` | Line-wrap word limit |
| `maxCharsPerLine` | `number` | `40` | Line-wrap char limit |
| `textAlign` | `string` | `'center'` | Text alignment within box |
| `safeZone` | `object\|null` | `null` | `{ top, right, bottom, left }` (0–1) |
| `zIndex` | `number` | `10` | Z-order |

#### `applySafeZone(platform): CaptionLayout`

Apply a platform-specific safe zone. Chainable.

Supported platforms: `'tiktok'` `'instagram'` `'youtube'` `'shorts'` `'reels'` `'twitter'` `'broadcast'` `'action'` `'title'`

```js
caption.layout.applySafeZone('tiktok');
caption.layout.applySafeZone('broadcast');
```

---

### 8.4 CaptionAnimation

22 animation types for captions, segments, words, and characters.

**`import { ANIMATION_TYPES, FadeAnimation, SlideAnimation } from 'videoforge'`**

#### Common constructor options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `duration` | `number` | `0.5` | Animation duration (s) |
| `delay` | `number` | `0` | Start delay (s) |
| `easing` | `string` | `'easeOut'` | Easing function |
| `stagger` | `number` | `0` | Per-element stagger delay (s) |
| `loop` | `boolean` | `false` | Loop indefinitely |
| `repeat` | `number` | `0` | Extra repetitions |
| `reverse` | `boolean` | `false` | Play in reverse |
| `target` | `string` | — | `ANIMATION_TARGET.*` |

#### Available animation types

| Type | Class | Description |
|------|-------|-------------|
| `fade` | `FadeAnimation` | Opacity transition |
| `slide` | `SlideAnimation` | Position slide |
| `scale` | `ScaleAnimation` | Scale in/out |
| `rotate` | `RotateAnimation` | Rotation |
| `bounce` | `BounceAnimation` | Bouncy entrance |
| `pop` | `PopAnimation` | Quick scale pop |
| `pulse` | `PulseAnimation` | Repeating scale pulse |
| `shake` | `ShakeAnimation` | Horizontal shake |
| `wobble` | `WobbleAnimation` | Rotation wobble |
| `wave` | `WaveAnimation` | Sinusoidal wave |
| `swing` | `SwingAnimation` | Pendulum swing |
| `flip` | `FlipAnimation` | 3D flip |
| `typewriter` | `TypewriterAnimation` | Character-by-character reveal |
| `karaoke` | `KaraokeAnimation` | Word highlight sync |
| `reveal` | `RevealAnimation` | Masked reveal |
| `scramble` | `ScrambleAnimation` | Character scramble |
| `elastic` | `ElasticAnimation` | Elastic spring |
| `glitch` | `GlitchAnimation` | Digital glitch |
| `highlight` | `HighlightAnimation` | Colour highlight sweep |
| `zoom` | `ZoomAnimation` | Zoom in/out |
| `blurReveal` | `BlurRevealAnimation` | Blur-to-sharp reveal |
| `stagger` | `StaggerAnimation` | Staggered entrance |

```js
import { FadeAnimation, PopAnimation, TypewriterAnimation } from 'videoforge';

// Word-level pop entrance with stagger
caption.animateWords('pop', { duration: 0.2, stagger: 0.06 });

// Character typewriter effect
caption.animateCharacters('typewriter', { duration: 0.05, stagger: 0.04 });

// Clip-level karaoke sync (requires word timings)
caption.animateWords('karaoke', { duration: 0.3 });
```

---

### 8.5 CaptionEffect

19 visual effects applied on top of rendered text.

**`import { CAPTION_EFFECT_TYPES, GlowEffect, ShadowEffect } from 'videoforge'`**

| Type | Class | Key params |
|------|-------|------------|
| `glow` | `GlowEffect` | `color, blur, strength, layers` |
| `shadow` | `ShadowEffect` | `color, offsetX, offsetY, blur, spread` |
| `outline` | `OutlineEffect` | `color, width, join` |
| `gradient` | `GradientEffect` | `type, stops, angle` |
| `neon` | `NeonEffect` | `color, blur, strength` |
| `glass` | `GlassEffect` | `blur, opacity, tint` |
| `blur` | `BlurEffect` | `blur` |
| `motionBlur` | `MotionBlurEffect` | `angle, distance` |
| `backgroundBox` | `BackgroundBoxEffect` | `color, padding, opacity` |
| `roundedBox` | `RoundedBoxEffect` | `color, padding, radius, opacity` |
| `highlight` | `HighlightEffect` | `color, padding, opacity` |
| `underline` | `UnderlineEffect` | `color, width, offset` |
| `strikethrough` | `StrikeThroughEffect` | `color, width` |
| `noise` | `NoiseEffect` | `intensity, animated` |
| `grain` | `GrainEffect` | `intensity` |
| `chromaticAberration` | `ChromaticAberrationEffect` | `offset, angle` |
| `bloom` | `BloomEffect` | `threshold, blur, strength` |
| `distortion` | `DistortionEffect` | `type, strength` |
| `reflection` | `ReflectionEffect` | `opacity, scale, offset` |

```js
import { GlowEffect, NeonEffect } from 'videoforge';

const glow = new GlowEffect({ color: '#FFD700', blur: 20, strength: 0.8 });
const neon = new NeonEffect({ color: '#00FFFF', blur: 30 });

// Apply via CaptionStyle:
caption.style.glow = { color: '#FFD700', blur: 20, strength: 0.8 };
```

---

### 8.6 CaptionKeyframe / KeyframeSet

Animate individual style or transform properties over time within a caption clip.

**`import { CaptionKeyframe, KeyframeSet, KeyframeTrack, CAPTION_EASING } from 'videoforge'`**

```js
// KeyframeSet — container for all animated properties on one element
const kfs = new KeyframeSet();

// Animate opacity: 0 at t=0, full at t=0.5, full at t=4.5, 0 at t=5
kfs
  .set('opacity',  0.0, 0)
  .set('opacity',  0.5, 1,  CAPTION_EASING.EASE_OUT)
  .set('opacity',  4.5, 1)
  .set('opacity',  5.0, 0,  CAPTION_EASING.EASE_IN);

// Animate font size for an impact zoom
kfs
  .set('fontSize', 0.0, 30)
  .set('fontSize', 0.2, 80, CAPTION_EASING.EASE_OUT_BACK)
  .set('fontSize', 4.8, 80);

// Assign to a caption segment
const segment = caption.segments[0];
segment.keyframeSet = kfs;

// Interpolate at any time
const opacity  = kfs.interpolate('opacity',  1.0);   // → 1.0
const fontSize = kfs.interpolate('fontSize', 0.1);   // → interpolated value
```

#### Keyframeable properties

`x` `y` `scaleX` `scaleY` `rotation` `opacity` `blur` `color` `fill` `stroke` `strokeWidth` `shadowBlur` `shadowOffsetX` `shadowOffsetY` `letterSpacing` `tracking` `lineHeight` `backgroundOpacity` `glowBlur` `glowStrength` `fontSize` `skewX` `skewY`

#### CAPTION_EASING values

`linear` `easeIn` `easeOut` `easeInOut` `easeInBack` `easeOutBack` `easeInOutBack` `easeInBounce` `easeOutBounce` `easeInElastic` `easeOutElastic` `spring` `snap` `overshoot`

---

### 8.7 CaptionPreset

Reusable style + layout + animation bundles. Apply by name or by building a custom preset.

**`import { CaptionPreset, PRESET_REGISTRY, createPreset } from 'videoforge'`**

#### Built-in presets

| Name | Class | Description |
|------|-------|-------------|
| `'hormozi'` | `HormoziPreset` | Bold ALL-CAPS, yellow highlights, outline, lower-third |
| `'mrbeast'` | `MrBeastPreset` | Large energetic text, per-character pop entrance |
| `'podcast'` | `PodcastPreset` | Minimal, centered, clean |
| `'news'` | `NewsPreset` | Professional lower-third bar |
| `'documentary'` | `DocumentaryPreset` | Elegant serif, subtle shadow |
| `'karaoke'` | `KaraokePreset` | Line-by-line, word-highlight animation |
| `'minimal'` | `MinimalPreset` | Plain white text, no effects |
| `'gaming'` | `GamingPreset` | Colorful, bouncy, staggered entrance |
| `'luxury'` | `LuxuryPreset` | Gradient fill, glow, premium feel |
| `'corporate'` | `CorporatePreset` | Professional sans-serif with outline |

```js
// Apply by name (simplest)
caption.applyPreset('hormozi');

// Instantiate directly
import { HormoziPreset, createPreset } from 'videoforge';

const preset   = new HormoziPreset();
const { style, layout, animations } = preset.build();
caption.style  = style;
caption.layout = layout;

// Factory function
const preset2 = createPreset('gaming');
caption.applyPreset(preset2);
```

---

### 8.8 MotionTypographyEngine

**`import { MotionTypographyEngine } from 'videoforge'`**

Low-level engine for parsing transcripts and segmenting them.

```js
const engine = new MotionTypographyEngine({
  defaultFps:          30,
  maxWordsPerSegment:  5,
  timeGapThreshold:    0.5,
});
```

#### `segmentTranscript(words, options?): CaptionSegment[]`

Group an array of timed words into display segments.

```js
const words = [
  { word: 'Hello',   start: 0.0, end: 0.3 },
  { word: 'world',   start: 0.4, end: 0.8 },
  { word: 'this',    start: 0.9, end: 1.0 },
  { word: 'is',      start: 1.1, end: 1.2 },
  { word: 'VideoForge', start: 1.3, end: 1.8 },
];

const segments = engine.segmentTranscript(words, {
  strategy:         'maxWords',
  maxWords:         3,
  timeGapThreshold: 0.5,
});
```

---

#### `applyPreset(captionClip, preset): CaptionClip`

Apply a preset to a `CaptionClip` via the engine. Chainable.

```js
engine.applyPreset(caption, 'hormozi');
```

---

## 9. Preview

The preview API is a skeleton — attach your own backend (Canvas, WebGL, etc.).

### 9.1 PreviewPlayer

**`import { PreviewPlayer } from 'videoforge'`**

```js
new PreviewPlayer(renderer, timeline, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `startTime` | `number` | `0` | Initial playhead position (s) |
| `loop` | `boolean` | `false` | Loop when reaching end |
| `onFrame` | `(time, frame) => void` | — | Called every rendered frame |
| `onStateChange` | `(newState, prevState) => void` | — | Called on state transitions |
| `onEnded` | `() => void` | — | Called when playback ends |

#### Properties

| Property | Type | Description |
|----------|------|-------------|
| `currentTime` | `number` | Live playhead position (s) |
| `state` | `string` | `PLAYER_STATE.*` |
| `isPlaying` | `boolean` | — |
| `isPaused` | `boolean` | — |

#### Methods

```js
player.play();          // start/resume
player.pause();         // pause
player.stop();          // stop and seek to 0
player.seek(30);        // jump to 30 s
```

All methods are chainable.

```js
import { PreviewPlayer, PreviewRenderer } from 'videoforge';

const renderer = new PreviewRenderer(project.timeline, { backend: myCanvas });
const player   = new PreviewPlayer(renderer, project.timeline, {
  loop:     true,
  onFrame:  (time) => console.log('Frame at', time.toFixed(3), 's'),
  onEnded:  () => console.log('Playback ended'),
});

player.play();
setTimeout(() => player.pause(), 5000);
```

---

### 9.2 PreviewRenderer

**`import { PreviewRenderer } from 'videoforge'`**

```js
new PreviewRenderer(timeline, options?)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `backend` | `object\|null` | `null` | Rendering backend (Canvas, WebGL…) |
| `dpr` | `number` | `1` | Device pixel ratio |

#### Methods

```js
// Attach a backend
renderer.setBackend(myCanvasBackend);

// Render one frame (async, requires backend)
const frame = await renderer.render(10.5);   // time in seconds

// Render a range and process each frame
await renderer.renderRange(0, 60, async (frame, frameNum) => {
  await saveFrame(frame, frameNum);
});

// Thumbnail extraction
const thumbnail = await renderer.getFrame(30);
```

---

## 10. Constants

**`import { TRACK_TYPES, CLIP_TYPES, EXPORT_TYPES, ... } from 'videoforge'`**

### TRACK_TYPES

```js
TRACK_TYPES.VIDEO   // 'video'
TRACK_TYPES.AUDIO   // 'audio'
TRACK_TYPES.IMAGE   // 'image'
TRACK_TYPES.TEXT    // 'text'
TRACK_TYPES.SHAPE   // 'shape'
```

### CLIP_TYPES

```js
CLIP_TYPES.VIDEO   // 'video'
CLIP_TYPES.AUDIO   // 'audio'
CLIP_TYPES.IMAGE   // 'image'
CLIP_TYPES.TEXT    // 'text'
CLIP_TYPES.SHAPE   // 'shape'
```

### ASSET_TYPES

```js
ASSET_TYPES.VIDEO      // 'video'
ASSET_TYPES.AUDIO      // 'audio'
ASSET_TYPES.IMAGE      // 'image'
ASSET_TYPES.FONT       // 'font'
ASSET_TYPES.SYNTHETIC  // 'synthetic'
```

### EFFECT_TYPES

```js
EFFECT_TYPES.FADE_IN          // 'fadeIn'
EFFECT_TYPES.FADE_OUT         // 'fadeOut'
EFFECT_TYPES.TRANSITION       // 'transition'
EFFECT_TYPES.COLOR_CORRECTION // 'colorCorrection'
EFFECT_TYPES.BLUR             // 'blur'
EFFECT_TYPES.CROP             // 'crop'
EFFECT_TYPES.CHROMA_KEY       // 'chromaKey'
EFFECT_TYPES.CUSTOM           // 'custom'
```

### CROP_ALIGNMENT

Positioning constant used with [`CropEffect`](#63-cropeffect) to control where the remaining
content is placed within the original canvas after pixels are removed.

```js
import { CROP_ALIGNMENT } from 'videoforge';

CROP_ALIGNMENT.CENTER        // 'center'       — default, content centred
CROP_ALIGNMENT.TOP           // 'top'          — pinned to top edge
CROP_ALIGNMENT.BOTTOM        // 'bottom'       — pinned to bottom edge
CROP_ALIGNMENT.LEFT          // 'left'         — pinned to left edge
CROP_ALIGNMENT.RIGHT         // 'right'        — pinned to right edge
CROP_ALIGNMENT.TOP_LEFT      // 'topLeft'      — pinned to top-left corner
CROP_ALIGNMENT.TOP_RIGHT     // 'topRight'     — pinned to top-right corner
CROP_ALIGNMENT.BOTTOM_LEFT   // 'bottomLeft'   — pinned to bottom-left corner
CROP_ALIGNMENT.BOTTOM_RIGHT  // 'bottomRight'  — pinned to bottom-right corner
```

### TRANSITION_TYPES

```js
TRANSITION_TYPES.CROSS_DISSOLVE  // 'crossDissolve'
TRANSITION_TYPES.WIPE_LEFT       // 'wipeLeft'
TRANSITION_TYPES.WIPE_RIGHT      // 'wipeRight'
TRANSITION_TYPES.WIPE_UP         // 'wipeUp'
TRANSITION_TYPES.WIPE_DOWN       // 'wipeDown'
TRANSITION_TYPES.SLIDE           // 'slide'
TRANSITION_TYPES.ZOOM            // 'zoom'
TRANSITION_TYPES.DIP_TO_BLACK    // 'dipToBlack'
TRANSITION_TYPES.DIP_TO_WHITE    // 'dipToWhite'
```

### EXPORT_TYPES

```js
EXPORT_TYPES.JSON      // 'json'
EXPORT_TYPES.PREMIERE  // 'premiere'
EXPORT_TYPES.FCPXML    // 'fcpxml'
EXPORT_TYPES.EDL       // 'edl'
EXPORT_TYPES.MP4       // 'mp4'
```

### EASING

```js
EASING.LINEAR       // 'linear'
EASING.EASE_IN      // 'easeIn'
EASING.EASE_OUT     // 'easeOut'
EASING.EASE_IN_OUT  // 'easeInOut'
```

### TEXT_ALIGN

```js
TEXT_ALIGN.LEFT     // 'left'
TEXT_ALIGN.CENTER   // 'center'
TEXT_ALIGN.RIGHT    // 'right'
TEXT_ALIGN.JUSTIFY  // 'justify'
```

### SHAPE_TYPES

```js
SHAPE_TYPES.RECTANGLE  // 'rectangle'
SHAPE_TYPES.ELLIPSE    // 'ellipse'
SHAPE_TYPES.TRIANGLE   // 'triangle'
SHAPE_TYPES.LINE       // 'line'
SHAPE_TYPES.POLYGON    // 'polygon'
SHAPE_TYPES.ARROW      // 'arrow'
```

### PLAYER_STATE

```js
PLAYER_STATE.IDLE       // 'idle'
PLAYER_STATE.PLAYING    // 'playing'
PLAYER_STATE.PAUSED     // 'paused'
PLAYER_STATE.BUFFERING  // 'buffering'
PLAYER_STATE.ENDED      // 'ended'
```

### DEFAULTS

```js
DEFAULTS.FPS           // 30
DEFAULTS.WIDTH         // 1920
DEFAULTS.HEIGHT        // 1080
DEFAULTS.SAMPLE_RATE   // 48000
DEFAULTS.CHANNELS      // 2
DEFAULTS.VIDEO_BITRATE // '8000k'
DEFAULTS.AUDIO_BITRATE // '192k'
```

---

## 11. Utilities

### IdGenerator

**`import { IdGenerator } from 'videoforge'`**

```js
IdGenerator.generate('clip');   // 'clip_1717488932_a3f_0'
IdGenerator.uuid();             // 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'
IdGenerator.reset();            // reset counter (useful in tests)
```

---

## 12. Complete Examples

### Example 1 — Short interview edit with music and lower-third

```js
import { Project } from 'videoforge';

const project = new Project({ name: 'Interview Edit', fps: 30, width: 1920, height: 1080 });

// ── Tracks ────────────────────────────────────────────────────────────────────
const videoTrack = project.addTrack('video', { name: 'A-Roll' });
const musicTrack = project.addTrack('audio', { name: 'Music' });
const textTrack  = project.addTrack('text',  { name: 'Lower-thirds' });

// ── Video ─────────────────────────────────────────────────────────────────────
videoTrack
  .addVideo('/footage/interview-01.mp4', { inPoint: 5,  outPoint: 25 })
  .volume(1)
  .fadeIn(0.5);

videoTrack
  .addVideo('/footage/interview-02.mp4', { startTime: 20, inPoint: 0, outPoint: 18 })
  .fadeIn(0.3)
  .fadeOut(1);

// ── Music ─────────────────────────────────────────────────────────────────────
musicTrack
  .addAudio('/audio/ambient.mp3', { inPoint: 0, outPoint: 38 })
  .volume(0.25)
  .fadeIn(2)
  .fadeOut(3);

// ── Lower-third title ─────────────────────────────────────────────────────────
textTrack
  .addText('Jane Smith — CEO', { startTime: 2, outPoint: 6 })
  .font('Inter')
  .fontSize(48)
  .color('#FFFFFF')
  .bold(true)
  .position(120, 940)
  .fadeIn(0.4)
  .fadeOut(0.4);

// ── Validate ──────────────────────────────────────────────────────────────────
const { valid, warnings, errors } = project.validate({ exporter: 'mp4' });
if (!valid) throw new Error(errors.map(e => e.message).join('\n'));
warnings.forEach(w => console.warn(w.message));

// ── Export ────────────────────────────────────────────────────────────────────
await project.export({ type: 'mp4', output: '/output/interview.mp4' });
```

---

### Example 2 — Social media reel with Hormozi-style captions

```js
import { Project } from 'videoforge';

const project = new Project({ name: 'Reel', fps: 30, width: 1080, height: 1920 });

const vt = project.addTrack('video');
const ct = project.addTrack('video', { name: 'Captions' });

// 15-second vertical reel
vt.addVideo('/footage/reel.mp4', { inPoint: 0, outPoint: 15 }).fadeOut(0.5);

// Caption with auto-detected word-level timing
ct.addCaption('Five habits that changed my life forever', {
  startTime: 0.5,
  outPoint:  15,
})
  .applyPreset('hormozi')
  .setTranscript('Five habits that changed my life forever', {
    maxWordsPerSegment: 4,
    wordTimings: [
      { word: 'Five',     start: 0.5,  end: 0.8  },
      { word: 'habits',   start: 0.9,  end: 1.3  },
      { word: 'that',     start: 1.4,  end: 1.6  },
      { word: 'changed',  start: 1.7,  end: 2.1  },
      { word: 'my',       start: 2.2,  end: 2.4  },
      { word: 'life',     start: 2.5,  end: 2.8  },
      { word: 'forever',  start: 2.9,  end: 3.5  },
    ],
  });

await project.export({ type: 'mp4', output: '/output/reel.mp4' });
```

---

### Example 3 — Podcast edit exported to Premiere Pro

```js
import { Project, PremiereXmlExporter } from 'videoforge';

const project = new Project({ name: 'Podcast Ep. 42', fps: 30, width: 1920, height: 1080 });

const vt = project.addTrack('video', { name: 'B-Roll' });
const at = project.addTrack('audio', { name: 'Interview' });
const mt = project.addTrack('audio', { name: 'Music' });

// B-roll
vt.addVideo('/broll/office.mp4',     { startTime: 0,   inPoint: 0,  outPoint: 30 });
vt.addVideo('/broll/coffee.mp4',     { startTime: 30,  inPoint: 5,  outPoint: 20 });
vt.addVideo('/broll/whiteboard.mp4', { startTime: 45,  inPoint: 0,  outPoint: 25 });

// Interview audio
at.addAudio('/audio/interview.wav', { inPoint: 0, outPoint: 3600 }).volume(1.0);

// Music bed
mt.addAudio('/audio/theme.mp3', { inPoint: 0, outPoint: 3600 })
  .volume(0.15)
  .fadeIn(3)
  .fadeOut(5);

// Export to Premiere
const exporter = new PremiereXmlExporter(project, { sequenceName: 'Podcast Ep. 42 v1' });
await exporter.export('/output/podcast-ep42.xml');
console.log('Premiere XML ready — import via File > Import');
```

---

### Example 4 — Save, validate, and restore a project

```js
import { Project } from 'videoforge';

// Build
const project = new Project({ name: 'My Doco', fps: 24, width: 3840, height: 2160 });
project.metadata = { author: 'Jane', tags: ['documentary', '4K'] };

const vt = project.addTrack('video');
const clip = vt.addVideo('/footage/scene.mp4', { inPoint: 0, outPoint: 120 });
clip.name = 'Opening Scene';
clip.speed(1).volume(0.95).fadeIn(1).fadeOut(2);

// Validate
const report = project.validate();
console.log('Valid:', report.valid);

// Save
await project.save('/projects/my-doco.vfp');

// Later — restore
const restored = await Project.load('/projects/my-doco.vfp');
console.log('Project:', restored.name);                              // 'My Doco'
console.log('FPS:',     restored.timeline.fps);                     // 24
console.log('Clips:',   restored.getTracks()[0].getClips().length); // 1

// Continue editing
const restoredClip = restored.getTracks()[0].getClips()[0];
console.log('Clip name:', restoredClip.name);    // 'Opening Scene'
restoredClip.speed(2);                           // speed up
await restored.save('/projects/my-doco-v2.vfp');
```

---

### Example 5 — Export to multiple formats from one project

```js
import {
  Project,
  Mp4Exporter,
  PremiereXmlExporter,
  FcpxmlExporter,
  EdlExporter,
} from 'videoforge';

const project = new Project({ name: 'Multi-Format', fps: 30, width: 1920, height: 1080 });
const vt = project.addTrack('video');
vt.addVideo('/footage/master.mp4', { inPoint: 0, outPoint: 60 });

const validate = project.validate({ exporter: 'mp4' });
if (!validate.valid) throw new Error('Invalid project');

// Run all exports in parallel
await Promise.all([
  new Mp4Exporter(project, { preset: 'fast', crf: 22 })
    .export('/output/master.mp4'),

  new PremiereXmlExporter(project)
    .export('/output/master-premiere.xml'),

  new FcpxmlExporter(project)
    .export('/output/master.fcpxml'),

  new EdlExporter(project)
    .export('/output/master.edl'),
]);

console.log('All exports complete.');
```

---

*VideoForge v0.9.0-alpha.1 — AGPL-3.0*
