*This project is not actively maintained.*

Font Stash
==========

Font stash is light-weight online font texture atlas builder written in C. It uses [stb_truetype](http://nothings.org) to render fonts on demand to a texture atlas.

The code is split in two parts, the font atlas and glyph quad generator [fontstash.h](/src/fontstash.h), and an example OpenGL backend ([glstash.h](/glstash.h).

## Screenshot

![screenshot of some text rendered witht the sample program](/screenshots/screen-01.png?raw=true)

## Example
``` C
// Create GL stash for 512x512 texture, our coordinate system has zero at top-left.
struct FONScontext* fs = glfonsCreate(512, 512, FONS_ZERO_TOPLEFT);

// Add font to stash.
int fontNormal = fonsAddFont(fs, "sans", "DroidSerif-Regular.ttf");

// Render some text
float dx = 10, dy = 10;
unsigned int white = glfonsRGBA(255,255,255,255);
unsigned int brown = glfonsRGBA(192,128,0,128);

fonsSetFont(fs, fontNormal);
fonsSetSize(fs, 124.0f);
fonsSetColor(fs, white);
fonsDrawText(fs, dx,dy,"The big ", NULL);

fonsSetSize(fs, 24.0f);
fonsSetColor(fs, brown);
fonsDrawText(fs, dx,dy,"brown fox", NULL);
```

## Using Font Stash in your project

In order to use fontstash in your own project, just copy fontstash.h, stb_truetype.h, and potentially glstash.h to your project.
In one C/C++ define FONTSTASH_IMPLEMENTATION before including the library to expand the font stash implementation in that file.

``` C
#include <stdio.h>					// malloc, free, fopen, fclose, ftell, fseek, fread
#include <string.h>					// memset
#define FONTSTASH_IMPLEMENTATION	// Expands implementation
#include "fontstash.h"
```

``` C
#include <GLFW/glfw3.h>				// Or any other GL header of your choice.
#define GLFONTSTASH_IMPLEMENTATION	// Expands implementation
#include "glfontstash.h"
```

## Creating new rendering backend

The default rendering backend uses OpenGL to render the glyphs. If you want to render the text using some other API, or want tighter integration with your code base you can write your own rendering backend. Take a look at the [glfontstash.h](/src/glfontstash.h) for reference implementation.

The rendering interface FontStash assumes access to is defined in the FONSparams structure. The renderer initialization function is assumed to fill in the FONSparams structure and call fonsCreateInternal to create the FontStash context.

```C
struct FONSparams {
	...
	void* userPtr;
	int (*renderCreate)(void* uptr, int width, int height);
	int (*renderResize)(void* uptr, int width, int height);
	void (*renderUpdate)(void* uptr, int* rect, const unsigned char* data);
	void (*renderDraw)(void* uptr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts);
	void (*renderDelete)(void* uptr);
};
```

- **renderCreate** is called to create renderer for specific API, this is where you should create a texture of given size.
	- return 1 of success, or 0 on failure.
- **renderResize** is called to resize the texture. Called when user explicitly expands or resets the atlas texture.
	- return 1 of success, or 0 on failure.
- **renderUpdate** is called to update texture data
	- _rect_ describes the region of the texture that has changed
	- _data_ pointer to full texture data
- **renderDraw** is called when the font triangles should be drawn
	- _verts_ pointer to vertex position data, 2 floats per vertex
	- _tcoords_ pointer to texture coordinate data, 2 floats per vertex
	- _colors_ pointer to color data, 1 uint per vertex (or 4 bytes)
	- _nverts_ is the number of vertices to draw
- **renderDelete** is called when the renderer should be deleted
- **userPtr** is passed to all calls as first parameter

FontStash uses this API as follows:

```
fonsDrawText() {
	foreach (glyph in input string) {
		if (internal buffer full) {
			updateTexture()
			render()
		}
		add glyph to interal draw buffer
	}
	updateTexture()
	render()
}
```

The size of the internal buffer is defined using `FONS_VERTEX_COUNT` define. The default value is 1024, you can override it when you include fontstash.h and specify the implementation:

``` C
#define FONS_VERTEX_COUNT 2048
#define FONTSTASH_IMPLEMENTATION	// Expands implementation
#include "fontstash.h"
```

## Multi-page glyph atlas

On devices where a single texture is small (for example a hard 64x64 limit) a
single page atlas starts dropping glyphs once it is full, and resizing or
clearing it is not an option because the UVs already handed out would break.
The paged API keeps one stash — the same fonts, sizes and glyph metrics
cache — and grows the atlas by adding fixed-size texture pages on demand.

Create a paged stash with `fonsCreateInternalPaged()` and a
`FONSpagedParams`. The four page callbacks are mandatory and
`fonsCreateInternalPaged` returns `NULL` without calling any of them when the
parameters are invalid: `width`/`height` must be within 8..4096,
`maxPages` within 1..64, and every page callback must be non-NULL.

```C
struct FONSpagedParams {
	int width, height;          // fixed size shared by every page
	int maxPages;               // 1..64
	unsigned char flags;
	void* userPtr;
	int  (*renderCreatePage)(void* uptr, int page, int width, int height);
	void (*renderUpdatePage)(void* uptr, int page, int* rect, const unsigned char* data);
	void (*renderDrawPage)(void* uptr, int page, const float* verts,
	                       const float* tcoords, const unsigned int* colors, int nverts);
	void (*renderDeletePage)(void* uptr, int page);
	void (*renderDelete)(void* uptr);   // optional, shared backend cleanup
};
```

### Page callback lifetime and guarantees

- A successfully created stash always owns page `0`; further pages are
  numbered consecutively (`0, 1, 2, ...`) and all pages have the same size.
- `renderCreatePage` returns 1 on success, 0 on failure. A failed page is
  **not** counted, receives no `renderDeletePage` call and leaves no glyph
  residency behind, so the same glyph can be retried on a later draw. If the
  very first page fails, the library still releases everything it acquired
  and calls the optional `renderDelete` once.
- New glyphs first try every existing page (oldest first) and only create a
  new page when none has room. Existing glyphs are never cleared, resized,
  moved or duplicated: a glyph's page, rectangle and pixels are fixed for its
  lifetime.
- When no page can hold a glyph (page limit reached, glyph larger than an
  empty page, or `renderCreatePage` returned 0), the stash reports
  `FONS_ATLAS_FULL`, skips that glyph's image but keeps advancing the pen by
  its font metrics; already cached glyphs later in the string still draw at
  their correct positions.
- Each triangle batch belongs to exactly one page, and that page's dirty
  pixels are uploaded through `renderUpdatePage` before `renderDrawPage`
  runs. Batches are submitted in input order, not sorted by texture — a
  `page 0, page 1, page 0` sequence produces callbacks in that exact order,
  including when the internal vertex buffer fills mid-line.
- On destruction every successfully created page gets exactly one
  `renderDeletePage` call (in page order); failed pages get none. Only after
  all pages have been released is the optional `renderDelete` called once, so
  a backend can free shared objects there. The `userPtr` itself stays owned by
  the caller when creation is rejected for invalid parameters.

### Measurement, iteration and queries

- `fonsTextBounds` (and the center/right pre-measurement inside draw and
  iteration) only reads font metrics: it never rasterizes, occupies an atlas
  page, or produces page/upload/draw events. The first real draw of a
  measured string still generates the bitmaps.
- `FONStextIter` gained an `int page`. It names the texture page of a
  resident quad, is `0` for a single page stash, and is `-1` with a zeroed
  quad when the glyph is not resident or has an empty outline (the pen still
  advances). Iteration never switches draw batches, so concurrent iterators
  and draws do not disturb each other. Empty outlines keep their advance and
  submit no triangles.
- `fonsGetAtlasPageCount`, `fonsGetTextureDataPage(stash,page,w,h)` and
  `fonsValidateTexturePage(stash,page,dirty)` query per-page state. Dirty
  rectangles are half open `[x0,y0,x1,y1)` and consumed independently per
  page; an invalid page creates nothing, consumes no other page's dirt,
  returns `NULL`/0 and reports zero dimensions.
- On a paged stash the legacy `fonsGetTextureData`/`fonsValidateTexture`
  address page 0, `fonsGetAtlasSize` returns the per-page size, and
  `fonsDrawDebug` draws page 0 only. `fonsExpandAtlas`/`fonsResetAtlas`
  return 0 without invoking callbacks or changing state; the single page
  implementations are unchanged.

### OpenGL backend

`glfonsCreatePaged(width, height, maxPages, flags)` (in
`src/glfontstash.h`) mirrors the interface using the GL2 backend: every page
owns its own GL texture, updates and draws bind the matching texture, and the
shared backend object is freed only after all textures are deleted. The
existing `glfonsCreate`/`glfonsDelete` keep their current behavior. The GL3
core backend retains its original single page interface.

## Compiling

In order to compile the demo project, your will need to install [GLFW](http://www.glfw.org/) to compile.

FontStash example project uses [premake4](http://industriousone.com/premake) to build platform specific projects, now is good time to install it if you don't have it already. To build the example, navigate into the root folder in your favorite terminal, then:

- *OS X*: `premake4 xcode4`
- *Windows*: `premake4 vs2010`
- *Linux*: `premake4 gmake`

See premake4 documentation for full list of supported build file types. The projects will be created in `build` folder. An example of building and running the example on OS X:

```bash
$ premake4 gmake
$ cd build/
$ make
$ ./example
```

# License
The library is licensed under [zlib license](LICENSE.txt)

## Links
Uses [stb_truetype](http://nothings.org) for font rendering.
