//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#ifndef OZZ_OZZ_ANIMATION_RUNTIME_SPARSE_SAMPLING_JOB_H_
#define OZZ_OZZ_ANIMATION_RUNTIME_SPARSE_SAMPLING_JOB_H_

#include <cstdint>

#include "ozz/animation/runtime/export.h"
#include "ozz/base/platform.h"
#include "ozz/base/span.h"

namespace ozz {

// Forward declaration of math structures.
namespace math {
struct SoaTransform;
}

namespace animation {

// Forward declares the animation type to sample.
class Animation;

// SparseSamplingJob samples an animation for a subset of its joints only.
//
// Whereas SamplingJob walks the whole interleaved key stream and decompresses
// every outdated SoA group of the animation, SparseSamplingJob builds, during
// Context::Prepare, per-track indexes over the untouched compressed data, so
// that Run only touches the SoA groups (groups of 4 joints) that contain a
// requested joint. Groups that are not selected never participate in the key
// time search nor in decompression. The animation file format is unchanged:
// indexes are reconstructed from the compressed animation itself, keeping the
// RawAnimation around is not required.
//
// The job does not own its buffers. Only the selected SoA groups are written
// to output; untouched groups, unselected lanes (including NaN sentinels and
// negative zeros) and output SoA entries beyond the animation are left
// bit-for-bit unchanged.
//
// Preparation cost (a full scan of the compressed keys, with linear memory
// allocation) is paid once per animation, in Prepare(). Run() does not
// allocate nor free. SparseSamplingJob targets crowds and partial-body
// sampling where a small fraction of a large skeleton is needed; for full
// posture sampling, SamplingJob remains the better option.
struct OZZ_ANIMATION_DLL SparseSamplingJob {
  // Validates job parameters. Returns true for a valid job, or false
  // otherwise:
  // -if animation or context is nullptr, or the context is not prepared for
  //  this animation.
  // -if output range is empty or too small for the animation.
  // -if ratio is not finite.
  // -if any joint index is out of range, not strictly ascending or
  //  duplicated.
  bool Validate() const;

  // Runs the sparse sampling task.
  // The job is validated before any operation is performed, see Validate()
  // for more details. On failure, the output, the context state and the stats
  // are left untouched.
  // Returns false if *this job is not valid.
  bool Run() const;

  // Time ratio in the unit interval [0,1], clamped before job execution.
  float ratio = 0.f;

  // The animation to sample. Owned by the caller: it must outlive the
  // prepared context. Call Context::Invalidate() (or Prepare() another
  // animation) before destroying or moving it.
  const Animation* animation = nullptr;

  // Forward declares the context object used by the SparseSamplingJob.
  class Context;

  // A context prepared for *this animation.
  Context* context = nullptr;

  // Joints to sample, as joint indices in the animation track order. The
  // range must be strictly ascending and free of duplicates. An empty range
  // is valid: the job then succeeds without writing anything.
  // The range can change between Run() calls, including from empty to
  // non-empty, without re-preparing the context.
  span<const int> joint_indices;

  // Job output, in SoA form. Must be at least as big as the animation's
  // number of SoA tracks. Only the SoA groups referenced by joint_indices are
  // written.
  span<ozz::math::SoaTransform> output;

  // Optional per-run workload counters. When non-null, the pointed object is
  // reset at the start of a successful Run() and incremented where the
  // measured operation actually happens. Counters are not updated on
  // validation failure, nor during Prepare().
  struct Stats {
    // Number of key time comparisons performed while locating the
    // interpolating keys of every selected track/channel.
    uint32_t time_comparisons = 0;

    // Number of 4-lane Float3 (half precision) block decompressions, summed
    // over translations and scales.
    uint32_t float3_decompress_blocks = 0;

    // Number of 4-lane compressed quaternion block decompressions.
    uint32_t quaternion_decompress_blocks = 0;

    // Number of SoA groups that went through interpolation.
    uint32_t interpolated_groups = 0;
  };
  Stats* stats = nullptr;
};

// Holds the per-track indexes rebuilt from a compressed Animation.
// The context cannot be copied (it owns its allocations); moves transfer the
// prepared state without leaving dangling pointers behind.
class OZZ_ANIMATION_DLL SparseSamplingJob::Context {
 public:
  // Constructs an empty, unprepared context.
  Context();

  // Disables copy.
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // Moves are supported: the moved-from context is left unprepared.
  Context(Context&&) noexcept;
  Context& operator=(Context&&) noexcept;

  // Deallocates the context.
  ~Context();

  // Prepares the context for _animation by scanning its compressed keys.
  // This is the only stage that scans every key of the animation; it may
  // allocate memory linearly with the number of compressed keys.
  // _animation is not modified and must remain valid (not destroyed nor
  // moved) until Invalidate() is called or another animation is prepared.
  // Preparing another animation cleanly detaches from the previous one:
  // indexes never reference the old compressed data anymore.
  // Returns false if _animation is nullptr. Any previously prepared state is
  // replaced on success, and left untouched on failure.
  bool Prepare(const Animation* _animation);

  // Detaches the context from its animation. After this call, Run() fails
  // validation until Prepare() succeeds again. This must be called before
  // the prepared animation is destroyed or moved.
  void Invalidate();

  // The animation this context is prepared for, nullptr when unprepared.
  const Animation* animation() const { return animation_; }

  // Number of SoA tracks of the prepared animation (0 when unprepared). This
  // is the minimum output span size required to run a job with this context.
  int num_soa_tracks() const { return num_soa_tracks_; }

 private:
  friend struct SparseSamplingJob;

  // Per component (translation, rotation, scale), the ordered list of global
  // compressed-key indices of every track (tracks are concatenated), and the
  // offset of each track's first entry in that list (with a trailing
  // sentinel holding the total key count).
  struct Component {
    span<uint32_t> key_index;
    span<uint32_t> track_starts;
  };

  void Deallocate();

  const Animation* animation_ = nullptr;
  int num_soa_tracks_ = 0;

  // Single allocation backing the key indexes, track starts and preparation
  // scratch of the three components.
  void* allocation_ = nullptr;
  size_t allocation_size_ = 0;
  Component translations_;
  Component rotations_;
  Component scales_;
};
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_RUNTIME_SPARSE_SAMPLING_JOB_H_
