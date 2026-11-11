<div align="center">
	
	
# rectpack2D

[![Linux & Windows Build](https://github.com/TeamHypersomnia/rectpack2D/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/TeamHypersomnia/rectpack2D/actions/workflows/cmake-multi-platform.yml)

[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/TeamHypersomnia/rectpack2D/badge)](https://scorecard.dev/viewer/?uri=github.com/TeamHypersomnia/rectpack2D)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/14008/badge)](https://www.bestpractices.dev/projects/14008)
	
**Used in [Assassin's Creed: Valhalla](https://www.youtube.com/watch?v=2KnjDL4DnwM&t=2382s)!**

**Used in [Snapchat](https://www.snap.com/terms/ioslicense)**!

**Used by [Skydio](https://pages.skydio.com/rs/784-TUF-591/images/Open%20Source%20Software%20Notice%20v0.2.html), one of the top drone manufacturers!**
	
**[2 scientific references](https://scholar.google.com/scholar?hl=en&as_sdt=0%2C5&q=teamhypersomnia&btnG=)!**
	
	
</div>

A header-only 2D rectangle packing library written in modern C++.  
This is a refactored and **highly optimized** version of the [original library](https://github.com/TeamHypersomnia/rectpack2D/tree/legacy).

It was originally developed for the needs of [Hypersomnia](https://github.com/TeamHypersomnia/Hypersomnia), a free and open-source multiplayer shooter.

![7](https://user-images.githubusercontent.com/3588717/42707552-d8b1c65e-86da-11e8-9412-54c580bd2696.jpg)

## Table of contents

- [Benchmarks](#benchmarks)
- [Usage](#usage)
- [Incremental atlas](#incremental-atlas)
- [Building the example](#building-the-example)
  * [Windows](#windows)
  * [Linux](#linux)
- [Algorithm](#algorithm)
  * [Insertion algorithm](#insertion-algorithm)
  * [Additional heuristics](#additional-heuristics)

## Benchmarks

Tests were conducted on a ``Intel(R) Core(TM) i7-4770K CPU @ 3.50GHz``.  
The binary was built with ``clang 6.0.0``, using an -03 switch.

### Arbitrary game sprites: 582 subjects.  

**Runtime: 0.8 ms**  
**Wasted pixels: 10982 (0.24% - equivalent of a 105 x 105 square)**  

Output (1896 x 2382):

![1](images/atlas_small.png)

In color:  
(black is wasted space)

![2](images/atlas_small_color.png)

### Arbitrary game sprites + Japanese glyphs: 3264 subjects.  

**Runtime: 4 ms**  
**Wasted pixels: 15538 (0.31% - equivalent of a 125 x 125 square)**  

Output (2116 x 2382):

![3](images/atlas_big.png)

In color:  
(black is wasted space)

![4](images/atlas_big_color.png)


### Japanese glyphs + some GUI sprites: 3122 subjects.  

**Runtime: 3.5 - 7 ms**  
**Wasted pixels: 9288 (1.23% - equivalent of a 96 x 96 square)**  

Output (866 x 871):

![5](images/atlas_tiny.png)

In color:  
(black is wasted space)

![6](images/atlas_tiny_color.png)

## Usage

This is a header-only library.
Just include the ``src/finders_interface.h`` and you should be good to go.

For an example use, see ``example/main.cpp``.

## Incremental atlas

The one-shot ``find_best_packing`` family reorders rectangles and searches for
the smallest bin. A renderer that receives UI textures while a frame is already
in flight needs the opposite: a **fixed-size canvas onto which images are
appended over time**, where coordinates handed to a draw batch never move.

``src/rectpack2D/incremental_atlas.h`` provides exactly that. It is built
directly on the insertion/splitting logic of ``empty_spaces`` - it never
reorders, shrinks or grows anything, and does no global optimization. Deletion,
expansion, multiple pages and global-optimum packing are intentionally out of
scope.

```cpp
#include <rectpack2D/incremental_atlas.h>

using namespace rectpack2D;

// Fixed 1024x1024 canvas. Throws std::invalid_argument outside 1..4096.
incremental_atlas<> atlas(1024, 1024);

// A static, stack-backed provider can be chosen instead:
// incremental_atlas<static_empty_spaces<256>> atlas(1024, 1024);

atlas_append_result result = atlas.append({
    atlas_image_request{ "ui/button", 64, 32, /*padding*/ 1, /*allow_rotation*/ true },
    atlas_image_request{ "ui/icon",   16, 16, 0, false }
});

if (!result) {
    // Nothing from this batch was published; the atlas state is untouched.
    // result.status is one of OUT_OF_SPACE, DUPLICATE_ID, INVALID_DIMENSIONS.
}

for (const atlas_entry& entry : result.entries) {
    // entry.layout.outer / entry.layout.content / entry.layout.rotated
}

atlas.find_entry("ui/button");              // nullptr if never published
atlas.get_entries();                        // all entries, insertion order
atlas.get_used_aabb();                      // extent covered so far
```

### Batches are atomic

Every ``append`` call either publishes **all** of its images or **none** of
them. The batch fails (and changes nothing) when:

- an image does not fit in the remaining empty spaces (``OUT_OF_SPACE``),
- an id duplicates an already published entry or another id in the same batch
  (``DUPLICATE_ID``),
- a width/height is outside ``1..4096`` or padding is outside ``0..64``
  (``INVALID_DIMENSIONS``),
- the empty-spaces provider's ``add`` returns ``false`` (``OUT_OF_SPACE``) -
  relevant for ``static_empty_spaces<N>``.

On failure the published entries, the used extent and the empty spaces -
including their traversal order - are byte-for-byte the same as before the
call. Retrying the same valid inputs afterwards produces the same result as a
control instance that never saw the failed call. An empty batch succeeds and
changes nothing.

Canvas dimensions outside ``1..4096`` throw ``std::invalid_argument`` at
construction; per-request range violations are reported as a failed append
instead. A padded image whose outer frame exceeds the canvas is a geometric
``OUT_OF_SPACE`` failure, not an invalid-argument one. All arithmetic stays
well within ``int`` range and cannot overflow.

### Frames, padding and rotation

Coordinates are integer pixels in half-open intervals
``[x, x + w) x [y, y + h)`` with the origin at the top-left corner, growing
right and down. Each entry reports:

- ``outer``: the occupied frame, sized ``(w + 2p) x (h + 2p)`` for an image of
  ``w x h`` with padding ``p``;
- ``content``: the outer frame shrunk by ``p`` on every side, i.e. where the
  original texels live;
- ``rotated``: whether the whole outer frame was rotated clockwise by 90
  degrees. Rotation is chosen per image via ``allow_rotation`` and never
  affects any other image.

When rotated, the width and height are swapped exactly once; the content frame
uses the already-swapped dimensions and is not swapped again. Source texel
``(u, v)`` lands at:

- not rotated: ``(content.x + u, content.y + v)``,
- rotated:     ``(content.x + h - 1 - v, content.y + u)``, where ``h`` is the
  original image height.

### Failure rollback of ``empty_spaces::insert``

The lower-level ``empty_spaces::insert`` now also rolls back on failure. When
a provider such as ``static_empty_spaces<N>`` rejects a split because it is
full, the removed candidate space is restored at its original index and the
used-extent AABB is left untouched, so a failed insertion has no observable
effect. For example, inserting ``3x2`` into an ``8x6`` bin backed by
``static_empty_spaces<1>`` fails (the two splits do not fit in one slot), and a
subsequent ``8x6`` insertion still succeeds. Providers that throw exceptions
are not covered by this guarantee.

### Headless example and tests

``example/grayscale_atlas.cpp`` is a window/GPU/decoder-free demo: it packs
small integer textures into a grayscale P2 PGM atlas, extends padding from the
nearest content-edge texel (corners included), leaves unoccupied pixels at
zero, and demonstrates a failed batch leaving the atlas bytes and the old
coordinates unchanged. The whole feature set is exercised headlessly by:

```bash
sh tests/check_incremental_atlas.sh
```

## Building the example

### Windows

From the repository's folder, run:

```bash
mkdir build
cd build
cmake -G "Visual Studio 15 2017 Win64" ..
````

Then just build the generated ``.sln`` file using the newest Visual Studio Preview.

### Linux

From the repository's folder, run:

```bash
mkdir build
cd build
cmake -DRECTPACK2D_BUILD_EXAMPLE=1 ..
make
````

Then run:

```bash
make run
```

## Algorithm

### Insertion algorithm

The library started as an implementation of this algorithm:

http://blackpawn.com/texts/lightmaps/default.html

The current version somewhat derives from the concept described there -  
however, it uses just a **vector of empty spaces, instead of a tree** - this turned out to be a performance breakthrough.  

Given

```cpp
struct rect_xywh {
	int x;
	int y;
	int w;
	int h;
};
````

Let us create a vector and call it empty_spaces.

```cpp
std::vector<rect_xywh> empty_spaces;
````

Given a user-specified initial bin, which is a square of some size S, we initialize the first empty space.

```cpp
empty_spaces.push_back(rect_xywh(0, 0, S, S));
````

Now, we'd like to insert the first image rectangle.  

To do this, we iterate the vector of empty spaces **backwards** and look for an empty space into which the image can fit.  
For now, we only have the S x S square: let's save the index of this candidate empty space,  
which is ``candidate_space_index = 0;``  

If our image is strictly smaller than the candidate space, we have something like this:

![diag01](images/diag01.png)

The blue is our image rectangle.  
We now calculate the gray rectangles labeled as "bigger split" and "smaller split",  
and save them like this:  

```cpp
// Erase the space that we've just inserted to, by swapping and popping.
empty_spaces[candidate_space_index] = empty_spaces.back();
empty_spaces.pop_back();

// Save the resultant splits
empty_spaces.push_back(bigger_split);
empty_spaces.push_back(smaller_split);
````

Notice that we push the smaller split *after* the bigger one.  
This is fairly important, because later the candidate images will encounter the smaller splits first,  
which will make better use of empty spaces overall.  

#### Corner cases:

- If the image dimensions equal the dimensions of the candidate empty space (image fits exactly),
	- we just delete the space and create no splits.  
- If the image fits into the candidate empty space, but exactly one of the image dimensions equals the respective dimension of the candidate empty space (e.g. image = 20x40, candidate space = 30x40)
	- we delete the space and create a single split. In this case a 10x40 space.

To see the complete, modular procedure for calculating the splits (along with the corner cases),
[see this source](src/rectpack2D/insert_and_split.h).

If the insertion fails, we also try the same procedure for a flipped image.

### Additional heuristics

Now we know how to insert individual images into a bin of a given initial size S.

1. However, what S should be passed to the algorithm so that the rectangles end up wasting the least amount of space?
	- We perform a binary search.
		- We start with the size specified by the library user. Typically, it would be the maximum texture size allowed on a particular GPU.
		- If the packing was successful on the given bin size, decrease the size and try to pack again.
		- If the packing has failed on the given bin size - because some rectangles could not be further inserted - increase the size and try to pack again.
	- The search is aborted if we've successfully inserted into a bin and the dimensions of the next candidate would differ from the previous by less than ``discard_step``.
		- This variable exists so that we may easily trade accuracy for a speedup. ``discard_step = 1`` yields the highest accuracy. ``discard_step = 128`` will yield worse packings, but will be a lot faster, etc.
	- The search is performed first by decreasing the bin size by both width and height, keeping it in square shape.
		- Then we do the same, but only decreasing width.
		- Then we do the same, but only decreasing height.
		- The last two were a breakthrough in packing tightness. It turns out important to consider non-square bins.
2. In what order should the rectangles be inserted so that they pack the tightest?
	- By default, the library tries 5 decreasing orders:
		- By area.
		- By perimeter.
		- By the bigger side.
		- By width.
		- By height.
