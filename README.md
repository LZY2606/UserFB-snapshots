# CPU cubemap seed

Original, dependency-free JavaScript starting point. Node.js >= 20.

`node test/baseline.test.mjs` checks the existing nearest-neighbor sampler.
`node cli.mjs nearest` prints the positive-X face color from fixtures/axis.json.

`cube.mjs` exports faceUV and nearestCube. Coordinates and tie handling are
explicit in the code. The seed has no bilinear sampling, mip LOD blending or tap
trace. The requested extensions should preserve this nearest-neighbor API.
