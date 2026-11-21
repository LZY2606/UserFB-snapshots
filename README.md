# Crystal Cavern Ray Tracer

A CPU ray tracer in C++17, written for CS 440 (Computer Graphics) at Habib University, Spring 2026. The showcase scene is "Crystal Cavern": a procedurally generated cave of 1.29 million triangles, rendered at 1920x1080 with Whitted recursive tracing, reflective floor, Phong crystals, and multiple shadow-casting lights.

![Final render](final.png)

`preview_480x360.png` is a low resolution render of the same scene, produced on this machine to verify the build instructions below.

## Team and attribution

This was a three-person course project (team 07885 / 07879 / 09212). Salman Adnan (07885) wrote the shading and materials layer: the Lambertian, GlossySpecular, and PerfectSpecular BRDFs, the Matte, Phong, and Reflective materials, the Whitted tracer with its reflection math, and shadow ray integration. The core engine grew out of his individual Assignment 5 implementation. Teammates contributed the BVH and sampling work, and the scene generator, lights, and website. The full breakdown is in `website/index.html`.

The class hierarchy (World, ViewPlane, Sampler, Camera, Geometry, Material) started from the CS 440 course scaffold, which is derived from the sample code accompanying Kevin Suffern's *Ray Tracing from the Ground Up* (several headers still carry his "Courtesy Kevin Suffern" credit). Suffern's code is distributed under the GNU GPL, so this repository keeps the GPLv2 license rather than relicensing.

## Features

- Whitted recursive ray tracing (configurable depth, default 5)
- BRDFs: Lambertian, glossy specular (Phong lobe), perfect specular
- Materials: Matte, Phong, Reflective, plus the scaffold's Cosine debug material
- Geometry: spheres, planes, triangles (Moller-Trumbore intersection)
- BVH acceleration: top-down median split on the longest axis, leaf threshold 4
- Lights: point (with distance attenuation), directional, optional shadows per light
- Samplers: one ray per pixel (Simple) or n x n jittered antialiasing
- Cameras: perspective and parallel
- OpenMP parallel rendering, PNG output via bundled stb_image_write

Roughly 3,000 lines of C++ with no external dependencies beyond a C++17 compiler.

## Architecture

- `raytracer/world/` owns the scene: geometry list, lights, camera, sampler, tracer, and the render loop. `World::build()` is supplied by exactly one file from `raytracer/build/`, chosen at compile time.
- `raytracer/tracers/Whitted.cpp` recurses through `Material::shade()`, which sums light contributions weighted by BRDFs and spawns reflection rays.
- `raytracer/acceleration/BVH.cpp` replaces the brute force loop in `World::hit_objects()` when a build file calls `world.build_accel()`.
- `raytracer/build/crystalgen.hpp` procedurally places the crystal clusters for the cavern scenes.

## Build and run

Requires a C++17 compiler; OpenMP is optional but strongly recommended. No libraries to install.

```bash
cd raytracer
make            # builds the 480x360 preview scene
make run        # renders it, writes scene.png (a few seconds on 8 threads)
```

Pick a different scene with `SCENE=`:

```bash
make run SCENE=build/buildCrystalCavern.cpp    # 1920x1080, 4 spp, ~9 min
```

Or compile by hand, swapping in any one build file:

```bash
g++ -O3 -fopenmp -std=c++17 raytracer.cpp \
    world/*.cpp utilities/*.cpp geometry/*.cpp cameras/*.cpp \
    image/*.cpp samplers/*.cpp materials/*.cpp brdf/*.cpp \
    lights/*.cpp tracers/*.cpp acceleration/*.cpp \
    build/buildCrystalCavernLow.cpp \
    -o scene.exe && ./scene.exe
```

Verified on this machine with GCC 14.3: the low scene compiles clean and renders 480x360 in about 5.4 s on 8 threads, matching `preview_480x360.png`.

### Scene files

| File | Resolution | BVH | Sampler | Notes |
|---|---|---|---|---|
| buildCrystalCavern.cpp | 1920x1080 | on | Jittered 2x2 | final render (`final.png`) |
| buildCrystalCavernMid.cpp | 1280x720 | on | Simple | ~1 min |
| buildCrystalCavernLow.cpp | 480x360 | on | Simple | fast preview |
| buildCrystalCavernNoAccel.cpp | 20x15 | off | Simple | brute force, timing only |
| buildHelloWorld.cpp, buildMvp.cpp, buildChapter14.cpp | small | off | Simple | regression scenes, match `scene-1/2/3.png` |

## Performance

Measured numbers are in `timings.txt` (8-thread CPU, -O3, OpenMP). Headline: brute force costs 166.6 ms per pixel on the cavern scene; with the BVH it drops to 0.040 ms per pixel, roughly a 4000x per-pixel speedup on 1.29 M triangles. The 480x360 figure there (6.9 s) is consistent with the 5.4 s reproduced during this cleanup.

## Repository layout

- `raytracer/` - engine source and scene files
- `final.png` - the 1920x1080 submission render
- `preview_480x360.png` - small render produced while verifying the build
- `timings.txt` - benchmark log for the acceleration comparison
- `website/index.html` - self-contained project page (scene writeup, feature list, per-member contributions, references)
- `docs/presentation.pdf` - the project presentation slides

## Challenges

- Brute force intersection was unusable at cavern scale. 1.29 M triangles meant every primary ray tested every triangle, and the no-accel timing (166.6 ms per pixel on a 20x15 image) made even a preview impractical. The fix was a top-down BVH in `acceleration/BVH.cpp`: pick the longest box axis, sort primitives by centroid on that axis, split at the median, and stop at 4 primitives or depth 30 per leaf. That dropped per-pixel cost to 0.040 ms, so the brute force loop in `World::hit_objects()` is only kept for the timing comparison.

- Keeping the reflection recursion from running away. `tracers/Whitted.cpp` calls back into `Material::shade()`, which spawns a new ray for reflective surfaces, so without a bound a mirror facing a mirror recurses forever. The guard is a plain depth counter: `trace_ray()` returns black once `depth > max_depth` (default 5), and each material passes `si.depth` down when it casts a secondary ray. Simple, but it has to be threaded through every material that reflects.

- Shadow acne on the crystals. Casting a shadow ray straight from the hit point re-intersects the same surface because of float rounding, which speckles lit faces with fake shadow. We offset the shadow ray origin along the normal (`si.hit_point + si.normal * 0.001f` in `materials/Matte.cpp`, mirrored in Phong) and cull lights on the back side with the `ndot <= 0` test before doing any shadow work. The offset is a single global epsilon, which is scene-scale dependent and noted as a limitation below.

- No system C++ compiler on the machine doing this pass. `g++` is not installed, only a bare `gcc`, so the usual `make` fails immediately. The working toolchain lives inside the flatpak `org.kde.Sdk//6.9` sandbox (g++ 14.3). Builds and renders during this verification ran through `flatpak run --command=sh --filesystem=host org.kde.Sdk//6.9 -c '...'` with the project on the host filesystem. The `--filesystem=host` flag matters: without it the sandbox can't see the source tree.

- Splitting the engine cleanly across three people. Because scene selection happens at compile time (one `build/*.cpp` per binary) and everything shares raw-pointer ownership through `World`, module boundaries had to be agreed up front or the merges would fight. Salman owned the shading and materials layer (the three BRDFs, the Matte/Phong/Reflective materials, the Whitted tracer, and shadow ray integration), which grew out of his Assignment 5 core; teammates took the BVH and samplers, and the scene generator, lights, and website. The `ShadeInfo` struct is the seam that let those pieces develop in parallel.
- **Writing the exhaustive project documentation honestly.** Writing an accurate deep dive meant checking the README's own claims against the actual shading code, not just restating them: the README describes point lights with distance attenuation, but PointLight::L() actually returns a constant color with no falloff. That's a real discrepancy, reported as one rather than silently reconciled in either direction.

## What I learned

- The acceleration structure is the whole ballgame for a scene this size. A ~4000x per-pixel speedup came entirely from not touching most of the geometry, not from any micro-optimization in the intersection code. Median split is not the best partition (a surface area heuristic would traverse fewer nodes), but even the naive version turned an impossible render into a few seconds.
- Recursive tracing needs its termination baked in from the start. Threading `depth` through `trace_ray` and every reflective `shade()` is easy to forget in exactly one material, and that one omission is an infinite loop rather than a wrong pixel.
- Float epsilon bugs look like rendering bugs. Shadow acne reads as a shading or lighting problem, but the cause is ray-surface self-intersection. Learning to reach for a normal offset first saved a lot of time chasing the wrong layer.
- OpenMP over the outer pixel loop with `schedule(dynamic, 4)` is nearly free parallelism here because pixels are independent, but shared RNG state is not. The jittered sampler needed `thread_local` seeding to stay deterministic across thread counts.
- Licensing follows lineage, not preference. Because the class scaffold derives from Suffern's GPL sample code, the whole project stays GPLv2 whether or not we would have picked it. Provenance of the starter code decides the license.

## What I'd do differently

- Scenes are selected by compiling one `build/*.cpp` into the binary, and the Makefile rebuilds all ~40 translation units every time. Separate object files plus a runtime scene format (even a tiny JSON loader) would cut the edit-render loop from minutes to seconds.
- Ownership is raw pointers throughout. `World`'s destructor deletes geometry, lights, camera, sampler, and tracer, but nothing enforces that build files heap-allocate exactly once per object. `unique_ptr` would make the contract explicit.
- The BVH uses a median split and allocates one node per `new`. A surface area heuristic and a flattened array layout would improve both traversal speed and cache behavior.
- Shadow acne is handled with a single global `kEpsilon = 0.0001`, which is scene-scale dependent. It happens to work here; it would not survive a much larger or smaller scene.
- There is no gamma correction or tone mapping stage, so light values were tuned by eye against clamped output. Fine for a stylized scene, wrong as a default.

## License

GPLv2, inherited from the Kevin Suffern derived scaffold this engine is built on. See `LICENSE`.
