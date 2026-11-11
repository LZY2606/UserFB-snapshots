# Subdivision {#subdivision}

Subdivision can be used to refine a mesh and to compute a subdivision surface. PMP provides three different algorithms:

1. Loop subdivision for triangle meshes. See pmp::loop_subdivision().
2. Catmull-Clark subdivision for quad meshes. See pmp::catmull_clark_subdivision().
3. Quad-tri subdivision for a mixed triangle/quad scheme. See pmp::quad_tri_subdivision().

Try for yourself:

\htmlonly
<iframe class="demo" src="/demos/subdivision.html"></iframe>
\endhtmlonly

## Semi-sharp Creases

Both pmp::catmull_clark_subdivision() and pmp::loop_subdivision() support
semi-sharp creases through an optional edge property `e:sharpness` of type
pmp::Scalar:

\code
auto sharpness = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
sharpness[some_edge] = 2.0; // stays sharp for two subdivision steps
catmull_clark_subdivision(mesh);
\endcode

The sharpness value expresses a finite, non-negative crease strength:

- A value of zero (the default) corresponds to a smooth edge.
- A value of one or more corresponds to a fully sharp edge: the edge point
  is the edge midpoint and endpoints with two or more creases use the
  crease rule `(6P+N1+N2)/8`, endpoints with three or more creases stay
  fixed.
- Fractional values linearly blend between the smooth and the sharp limit
  positions.

The strength decreases by one per subdivision step, so a crease gradually
becomes smooth. The two child edges of a split edge inherit the decayed
strength `max(s-1, 0)`, while edges introduced in the interior of faces are
smooth. This also applies to boundary edges, even though their geometry is
governed by the boundary handling mode.

Semi-sharp creases compose with the boolean feature markers: edges flagged
through `e:feature` act as permanent hard edges and vertices flagged through
`v:feature` keep their explicit handling, taking precedence over finite
sharpness values. Semi-sharp creases never set the boolean markers
themselves, so they recover the smooth behavior once their strength is used
up.

Negative, NaN, or infinite sharpness values are invalid and make both
functions throw pmp::InvalidInputException before the mesh is modified. If
the `e:sharpness` property does not exist, both algorithms behave exactly as
before.
