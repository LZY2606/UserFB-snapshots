[![logo](media/icon/ozz-grey-256.png)](http://guillaumeblanc.github.io/ozz-animation/)

ozz-animation
=============

open source c++ 3d skeletal animation library and toolset
---------------------------------------------------------

ozz-animation provides runtime character animation playback functionalities (loading, sampling, blending...). It proposes a low-level renderer agnostic and game-engine agnostic implementation, focusing on performance and memory constraints with a data-oriented design.

ozz-animation comes with the toolchain to convert from major Digital Content Creation formats (gltf, Fbx, Collada, Obj, 3ds, dxf) to ozz optimized runtime structures. Offline libraries are also provided to implement the conversion from any other animation and skeleton format.

Follow [this link](https://github.com/guillaumeblanc/ozz-animation/issues/175) for a list of projects using ozz-animation.

Documentation
-------------

Documentation and samples are available from [ozz-animation website](http://guillaumeblanc.github.io/ozz-animation/).

Inertialization
---------------

`ozz::animation::InertializationJob` (see `include/ozz/animation/runtime/inertialization_job.h`) inserts an interruptible inertial transition layer between the usual sampling/blending output and `LocalToModelJob`. When a new action starts, only the new target's current local pose and velocity are read: the animation that was left is never sampled again. A switch can be requested while a previous transition is still running; pose and velocity stay continuous and the output catches up with the new target at the end of the requested duration.

Each call needs the target pose **and its time derivative** (`InertializationJob::Velocity`):

- `translation` is the derivative of the local translation, in units per second.
- `angular` is the angular velocity expressed in the parent joint frame, defined such that `q(t + dt) = Exp(angular * dt) * q(t)` (the length of the `Exp` vector argument is the full rotation angle).
- `log_scale` is the derivative of the natural logarithm of the (positive) local scale.

These velocities can be computed analytically from the animation, or by finite-differencing two samples taken at `t` and `t + h` (with `angular = Log(q(t + h) * q(t)^-1) / h`), as demonstrated in `checks/inertialization_demo.cc`.

Timing and switch rules:

- The first call after `Context::Resize`/`Context::Reset` must use `delta_time = 0` and `transition_duration = 0`: the target is adopted directly.
- Steady frames use `transition_duration = 0` and `delta_time >= 0`; the target inputs must already be the ones of the advanced time.
- To switch, first advance with the old target up to the switch time, then call with the new target of that same time, `delta_time = 0` and `transition_duration > 0`. This call still returns the pre-switch pose and velocity; subsequent steady calls with the new target play the transition out.

Supported platforms
-------------------

ozz-animation is tested on WebAssembly, Linux, macOS and Windows, for x86, x86-64 and ARM architectures. The run-time code (ozz_base, ozz_animation, ozz_geometry) depends only on c++17, on the C and the C++ standard libraries, and has no OS specific code. Portability to any other platform shouldn't be an issue.

Samples, tools and tests depend on external libraries (glfw, tinygltf, Fbx SDK, jsoncpp, gtest, ...), which aren't need to ship with ozz-animation runtime.

Build status
------------

|         | Linux  | macOS | Windows | WebAssembly |
| ------- | ------ | ------ | ------- | ------- |
| master  | [![Linux](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml) | [![macOS](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml) | [![Windows](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml) | [![WASM](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml/badge.svg?branch=master)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml) |
| develop | [![Linux](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/linux.yml) | [![macOS](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/macos.yml) | [![Windows](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/windows.yml) | [![WASM](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml/badge.svg?branch=develop)](https://github.com/guillaumeblanc/ozz-animation/actions/workflows/wasm.yml) |

The dashboard for all branches is available [here](http://guillaumeblanc.github.io/ozz-animation/documentation/dashboard/).

Contributing
------------

All contributions are welcome: code reviews, bug reports, bug fixes, samples, features, platforms, testing, documentation, optimizations...

Please read [CONTRIBUTING](CONTRIBUTING.md) file for more details about how to submit bugs or contribute to the code.

License
-------

ozz-animation is hosted on [github](http://github.com/guillaumeblanc/ozz-animation/) and distributed under the **MIT License** (MIT).
