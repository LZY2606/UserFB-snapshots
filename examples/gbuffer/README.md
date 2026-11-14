# Software renderer multiple render targets (MTR)

The software backend can render up to **four offscreen color outputs from a
single fragment execution**, which is what a deferred-lighting G-buffer needs:
one fragment writes base color, an encoded normal and material parameters at
the same time, so the scene is never redrawn per target. Depth is a single
shared attachment for all color slots.

The change is confined to the software backend under `src/Render/Software`.
No floating-point color formats, window system or GPU backend are involved.

## 1. Framebuffer attachments

`FrameBuffer` keeps its old single-color interface and adds explicit slots.
Slots are numbered `0..3` (`FrameBuffer::COLOR_ATTACHMENT_COUNT == 4`).

```cpp
// bind / rebind a 2D RGBA8 texture to a slot (pass an empty shared_ptr to unbind)
virtual void setColorAttachment(unsigned slot, std::shared_ptr<Texture>& color, int level);
const FrameBufferAttachment& getColorAttachment(unsigned slot) const;
bool isColorReady(unsigned slot) const;
```

- The legacy `setColorAttachment(tex, level)` / `getColorAttachment()` /
  `isColorReady()` overloads now delegate to **slot 0**.
- The cubemap overload `setColorAttachment(tex, CubeMapFace, level)` is
  retained and still binds slot 0.
- Slots may be bound **sparsely**: binding only slot 2, leaving gaps, or
  unbinding a slot are all legal. An unbound slot simply receives no writes.
- Slots 1..3 currently accept 2D `RGBA8` textures only (the original formats
  and the cubemap path remain supported on slot 0).
- A slot out of range (`>= 4`) throws `std::invalid_argument`.

## 2. Shader outputs

`ShaderBuiltin` exposes four color outputs:

```cpp
glm::vec4& colorOutput(unsigned slot);   // slot 0..3
```

- `colorOutput(0)` returns the **same object** as the legacy `FragColor`, so
  existing shaders are unchanged. Slots 1..3 are independent vectors.
- Before every fragment runs, all four outputs are cleared to zero and the
  `discard` flag is reset.
- Setting `gl->discard = true` makes the fragment commit **no color on any
  slot and no depth**.

## 3. Per-attachment render state

`RenderStates` holds a `ColorAttachmentState colorAttachments[4]` array. Each
entry can override blending and restrict the RGBA write mask:

```cpp
// use this slot's own blend switch/parameters instead of the global ones
renderStates.setAttachmentBlend(slot, /*enable=*/true, blendParams);

// restrict which channels the slot may write (ColorWrite_R/G/B/A, OR-ed)
renderStates.setAttachmentWriteMask(slot, ColorWrite_R | ColorWrite_A);
```

- By default `blendOverride == false`: the slot inherits the global
  `RenderStates::blend` and `RenderStates::blendParams`, and
  `colorWriteMask == ColorWrite_RGBA` (all channels writable).
- Blending reads the **destination color of that same attachment** and keeps
  the original float-to-byte conversion rules (`glm::clamp` then `*255`).
- The write mask only affects color. Disabling some channels, or all four
  channels (mask `0`), for one or every attachment does **not** stop the
  sample from taking part in the shared depth test and depth write.

## 4. Pipeline semantics

- The fragment shader runs **once per fragment**, regardless of how many color
  slots are bound; its outputs are then fanned out to the bound attachments.
  The shader is never re-executed per attachment, and the depth test that can
  modify depth is performed only once.
- Depth is one shared attachment. A failed depth test skips every color write.
- Points, lines and triangles (including both early-Z settings and the
  point/line polygon modes) all write every bound slot.
- Single-sample and the existing four-sample targets are both supported. At
  the end of a draw every multisample color attachment is resolved
  independently into its own single-sample buffer (per-sample average), not
  just slot 0.

## 5. Validation and clearing

`FrameBufferSoft::validate()` runs at the start of `beginRenderPass` and again
before each `draw()`, throwing `std::invalid_argument` when:

- an attachment image is not allocated;
- the bound attachments do not all share the same dimensions, where the
  effective size is computed from the bound **mip level**
  (`getLevelWidth/Height(level)`), or do not share the same sample count;
- the same image (texture + layer + level) is attached more than once, or used
  as both color and depth;
- no attachment is present at all.

Invalid framebuffers are therefore rejected before a clear or a draw.
Clearing keeps using `ClearStates`: the one `clearColor` is applied to every
bound color slot (empty slots are untouched) and `clearDepth` to the depth
attachment. Switching framebuffers or rebinding an image takes effect on the
next draw — the renderer re-reads the attachment buffers every pass/draw and
never keeps a stale color-buffer pointer.

## 6. Windowless G-buffer example

`GBufferExample.cpp` renders one full-screen triangle into three sparse color
targets (albedo, encoded normal, material) plus shared depth, then writes each
layer to a PNG with stb. It needs no window or graphics SDK.

Build against the objects produced by `task_support/build.sh` (run
`bash task_support/run.sh baseline` once to create them), from the repository
root:

```sh
c++ -std=c++11 -O0 -pthread -DGLM_FORCE_PURE -D_MSC_EXTENSIONS \
    -include cfloat -include limits -include atomic \
    -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 \
    examples/gbuffer/GBufferExample.cpp \
    .task-build/Geometry.o .task-build/Logger.o .task-build/ImageUtils.o \
    .task-build/RendererSoft.o .task-build/md5.o -o gbuffer_example
./gbuffer_example
```

It writes `gbuffer_albedo.png`, `gbuffer_normal.png` and
`gbuffer_material.png` in the working directory.

## 7. Tests

`checks/target.cpp` registers `routing`, `composition` and `resolve` groups
with `task_cases` / `task_case_count` and is driven by
`task_support/driver.c`. Run:

```sh
bash task_support/run.sh target   # new MTR cases, then the basic draw
bash task_support/run.sh baseline # original single-attachment path only
```

The cases cover sparse binding, the output-0 alias, fragment call counts,
multiple-attachment writes under LESS, per-attachment blending and channel
masks, discard-based occlusion, both early-Z modes, points/lines, the cubemap
slot-0 overload, mip-level effective sizes, framebuffer validation, and
four-sample per-attachment resolve with a single-target numeric control.
