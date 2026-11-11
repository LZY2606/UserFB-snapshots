# svg-mesh-3d

[![stable](http://badges.github.io/stability-badges/dist/stable.svg)](http://github.com/badges/stability-badges)

  <center>[<img src="http://i.imgur.com/rY3bRps.png" width="80%" />](http://mattdesl.github.io/svg-mesh-3d/)</center>

[(ThreeJS demo)](http://mattdesl.github.io/svg-mesh-3d/) - [(source)](./demo/index.js)

A high-level module to convert a SVG `<path>` string into a 3D triangulated mesh. Best suited for silhouettes, like font icon SVGs.

Built on top of [cdt2d](https://www.npmjs.com/package/cdt2d) by [@mikolalysenko](https://twitter.com/MikolaLysenko), and various other modules.

## Install

```sh
npm install svg-mesh-3d --save
```

## Example

Here is an example using a simple path:

```js
var svgMesh3d = require('svg-mesh-3d')

var path = 'M305.214,374.779c2.463,0,3.45,0.493...'
var mesh = svgMesh3d(path)
```

The returned mesh is a 3D indexed ["simplicial complex"](https://www.npmjs.com/package/mesh-primitives#generic-mesh-modules) that can be used in ThreeJS, StackGL, etc. It uses arrays for vectors and follows the format:

```js
{
  positions: [ [x1, y1, z1], [x2, y2, z2], ... ],
  cells: [ [a, b, c], [d, e, f] ]
}
```

Or, to load a mesh in the browser from an SVG file:

```js
var loadSvg = require('load-svg')
var parsePath = require('extract-svg-path').parse
var svgMesh3d = require('svg-mesh-3d')

loadSvg('svg/logo.svg', function (err, svg) {
  if (err) throw err

  var svgPath = parsePath(svg)
  var mesh = svgMesh3d(svgPath, {
    delaunay: false,
    scale: 4
  })
})

```

## Demos

- [ThreeJS Demo](http://mattdesl.github.io/svg-mesh-3d/)

Source:

- [demo/index.js](demo/index.js) - animated meshes in ThreeJS
- [demo/2d.js](demo/2d.js) - drawing Canvas2D

To run the demos from source:

```sh
git clone https://github.com/mattdesl/svg-mesh-3d.git
cd svg-mesh-3d

# install dependencies
npm install

# ThreeJS demo
npm run start

# Canvas2D demo
npm run 2d
```

## Usage

#### `mesh = svgMesh3d(svgPath, [opt])`

Triangulates the `svgPath` string into a 3D simplicial complex. The positions in the 3D mesh are normalized to a `-1.0 ... 1.0` range.

Options:

- `delaunay` (default `true`)
  - whether to use Delaunay triangulation
  - Delaunay triangulation is slower, but looks better
- `clean` (default `true`)
  - whether to run the mesh through [clean-pslg](https://www.npmjs.com/package/clean-pslg)
  - slower, but often needed for correct triangulation
- `simplify` (default `0`)
  - a number, the distance threshold for simplication before triangulation (in pixel space)
  - higher number can produce faster triangulation
- `scale` (default `1`)
  - a positive number, the scale at which to [approximate the curves](https://github.com/mattdesl/adaptive-bezier-curve) from the SVG paths
  - higher number leads to smoother corners, but slower triangulation
- `normalize` (default `true`) a boolean, whether to normalize the positions to `-1 .. 1`
- `randomization` (default `0`)
  - a positive number, the amount of extra points to randomly add within the bounding box before triangulation
  - higher number can lead to a nicer aesthetic, but slower triangulation
- `fillRule` (default: unset)
  - either `'nonzero'` or `'evenodd'`, selecting the SVG [fill rule](https://www.w3.org/TR/SVG/painting.html#FillRuleProperty) used to decide which regions are painted
  - `'nonzero'` uses the directed winding number of the discrete contours: two same-direction loops keep the shared region filled, while opposite loops cancel out
  - `'evenodd'` uses the parity of contour crossings, so every second layer is a hole regardless of contour direction
  - the rule is evaluated on the 2D contours after curve discretization (`scale`) and `simplify`, before PSLG cleanup; the retained triangles never cross a cleaned contour edge
  - limited to the combination `clean: true`, `interior: true`, `exterior: false`, `infinity: false` (all defaults). Passing any conflicting value, or an unknown rule, throws a `TypeError`
  - when `fillRule` is omitted the behaviour is unchanged and the other options are forwarded to [cdt2d](https://www.npmjs.com/package/cdt2d) exactly as before

Other options are passed to [cdt2d](https://www.npmjs.com/package/cdt2d), such as `exterior` (default false) and `interior` (default true).

## Checks

The pinned check runtime lives in `checks/runtime`. Install it once, then run:

```sh
npm install --prefix checks/runtime

# legacy behaviour baseline (also exercises fillRule areas)
sh checks/baseline.sh

# fill rule test suite: areas, interior sample points, triangle overlap,
# invalid options and unchanged default output
NODE_PATH=checks/runtime/node_modules node --test checks/fill_rule.test.cjs
```

A diagnostic script renders the two fill rules of one path side by side as
SVG. The picture is only for orientation; the tests decide correctness from
numbers:

```sh
NODE_PATH=checks/runtime/node_modules node checks/fill_rule_svg.cjs out.svg "M0 0H10V10H0Z M2 2H8V8H2Z"
```

## See Also

- [extract-svg-path](https://www.npmjs.com/package/extract-svg-path) - grab `<path>` data from SVG
- [load-svg](https://www.npmjs.com/package/load-svg) - load SVG in browser
- [three-simplicial-complex](https://www.npmjs.com/package/three-simplicial-complex) - render meshes in ThreeJS
- [cdt2d](https://www.npmjs.com/package/cdt2d) - constrained delaunay triangulation in 2D

## License

MIT, see [LICENSE.md](http://github.com/mattdesl/svg-mesh-3d/blob/master/LICENSE.md) for details.
