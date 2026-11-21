thekla_atlas
============

This tool performs mesh segmentation, surface parameterization, and chart packing.

This is especially useful when generating light maps for meshes that do not have artist-supplied uv's.

### How to build (Windows)

Open `vc9/thekla.sln` using Visual Studio.

### How to build (OS X)

Use cmake.  For example:

```bash
$ brew install cmake  # Install cmake
$ cmake -H. -Bbuild   # Create makefiles
$ cmake --build build # Invoke the build
```

### Injective atlas generation

A strip can fold back onto itself in UV space: every triangle may still have
positive, consistently oriented area while the chart covers the same texels
more than once. `atlas_generate` does not detect these global overlaps. The
dedicated entry point `atlas_generate_injective` repairs them.

```c
Atlas_Output_Mesh * output;
Atlas_Injective_Report * report;
Atlas_Error error;

output = atlas_generate_injective(input, &options, max_charts, &report, &error);
```

It currently accepts only `Atlas_Charter_Extract` with
`mapper_options.preserve_uvs = true` and `mapper_options.preserve_boundary =
true` on an oriented, non-degenerate manifold triangle mesh (multiple
components, materials and UV seams are allowed).

With this entry point `preserve_uvs` has a stricter meaning than in
`atlas_generate`: the input unwrap is kept exactly as supplied while it is
being *checked*. Charts that already map injectively are never re-parameterized.
Only the descendants of a chart that fails the injective check are
re-parameterized from scratch with the existing LSCM / single-face mappers.
The ordinary `atlas_generate` entry point keeps its original behavior, and the
`Atlas_Charter_Witness` charter is unchanged.

Each round validates, in double precision, non-zero and consistently oriented
triangle area and any positive-area intersection between two triangles of the
same chart. Shared edges, shared vertices and boundary contacts have zero
intersection area and are not considered overlaps; containment, edge crossings
and adjacent faces folding back over each other are detected. Candidates are
filtered with an UV sweep line, so large healthy charts never run a quadratic
all-pairs test. When an overlap is found the lexicographically smallest source
face pair is recorded as the witness, the source-face dual graph is turned into
a BFS tree rooted at the smallest face (neighbors expanded in ascending face
order), and the tree edge on the unique witness path that maximizes the smaller
resulting group is cut. Ties resolve by endpoint face ids. The two strict
subsets are rebuilt as real `Chart` objects and re-checked; invalid descendants
are cut again using the failing witness face and its farthest tree face. Only
charts that pass every check reach the packer, where conservative padding is
always enabled.

`max_charts` must be positive. The function returns an owned
`Atlas_Output_Mesh` (free it with `atlas_free`) and, on success, an owned
`Atlas_Injective_Report` (free it with `atlas_injective_report_free`). The
report and the output mesh hold their own allocations: they stay valid after
the input mesh or any previously returned mesh is released, and the input is
never modified. The report records the final chart of every source face, the
source-face sets of every chart node and final chart, every split with its
parent/child sets, overlap witness and cut edge, and the total number of exact
triangle/triangle narrow-phase tests.

On failure the function returns `NULL`, leaves `*report` untouched (`NULL`),
publishes no partial output and sets one of:

- `Atlas_Error_Invalid_Args` / `Atlas_Error_Invalid_Options` /
  `Atlas_Error_Invalid_Mesh`: bad arguments or unsupported options/mesh.
- `Atlas_Error_Budget_Exceeded`: repairing the overlaps would require more than
  `max_charts` charts (the initial extracted charts already count).
- `Atlas_Error_Uninjectable`: a single face cannot produce a valid map or a
  chart cannot be made injective.

An example that repairs a self-overlapping spiral strip and writes an SVG
colored by chart lives in `examples/injective.cpp`.
