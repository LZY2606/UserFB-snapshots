#pragma once
#include <stdint.h>
#include <stddef.h>
#include <utility>
#include "../StdAllocator.h"
#include "../mpg_config.h"
#include "../common/mpg_frame.h"
#include "mpg_bitreader.h"
#include "mpg_headers.h"
#include "mpg_macroblock.h"
#include "mpg_motion.h"

/*
 * Header-only. Top-level decoder: start-code demux -> sequence_header /
 * picture_header / slice dispatch -> macroblock loop -> reconstructed
 * Frame, delivered to the caller in *display* order. Decodes I-, P-, and
 * B-pictures (Phase A/B/C, see docs/scope.md); a D picture_coding_type,
 * or a P/B-picture arriving before the reference picture(s) it needs have
 * been decoded, is detected and reported as
 * DecodeStatus::kUnsupported/kError rather than mis-decoded, the same
 * "detect and reject, don't guess" policy TinyH264 applies to
 * CABAC/B-slices/etc. A picture buffer allocation failure (out of memory -
 * see StdAllocator.h) is reported the same deliberate way, as
 * DecodeStatus::kAllocationError, rather than crashing.
 *
 * Unlike TinyH264's NalReader, no scratch buffer/emulation-prevention
 * stripping is needed here: MPEG-1's bitstream syntax is specifically
 * designed so the 3-byte prefix 00 00 01 cannot occur anywhere except a
 * genuine start code, so each unit's bytes are read directly out of the
 * caller's own input buffer (see StartCodeReader below).
 *
 * MPEG-1 has no single self-contained "access unit" NAL the way H.264
 * does - a picture is a picture_header unit followed by one or more
 * separate slice_start_code units. Picture *decode* boundaries are
 * detected the way architecture.md describes: whenever a
 * picture-terminating start code (a new picture_header, group_start,
 * sequence_header, or sequence_end) is found while a picture is already
 * in progress, that signals the *previous* picture is complete - see
 * next()'s use of StartCodeReader::peek()/advance() to look at a unit's
 * type before deciding whether to consume it this call or leave it for
 * the next one.
 *
 * Picture *display* order is a separate concern from decode order once
 * B-pictures exist (clause 2.4.3.6 note: a B-picture is transmitted
 * *after* both of the reference pictures it depends on, even though it
 * displays *between* them) - see "Output reordering" below.
 *
 * ## Reference picture management
 *
 * An I- or P-picture ("anchor") is always exactly one of two things a
 * later picture might reference: refPast_ (the most recently *released*
 * anchor - see below) or refFuture_ (the most recently *decoded* anchor,
 * regardless of whether it's been released for display yet). A
 * P-picture's single reference is always refFuture_; a B-picture uses
 * refPast_ as its forward (temporally past) reference and refFuture_ as
 * its backward (temporally future) reference (clause 7.6.1/7.6.5) - both
 * must already be valid, which - given decode order always places a
 * B-picture's two anchors before it - is exactly the state once at least
 * two anchors have been decoded. Demoting refFuture_ to refPast_ when a
 * *third* anchor arrives is an O(1) pointer swap (std::swap), not a
 * ~150KB+ memcpy - same trick TinyH264 uses for its own sliding-window
 * reference list. B-pictures are never held as a reference themselves
 * (clause 7.6.1 - nothing may reference a B-picture), so they never touch
 * refPast_/refFuture_ at all.
 *
 * ## Output reordering
 *
 * A B-picture is always immediately ready for display the moment it
 * finishes decoding (nothing depends on it, so there's nothing to wait
 * for) - next() returns kOk with frame() pointing at curFrame_ directly.
 * An anchor (I/P) picture is different: it must not be shown until every
 * B-picture that displays *before* it (but decodes *after* it) has
 * already been delivered - which, in bitstream order, is exactly when
 * the *next* anchor starts being decoded (everything between the two
 * anchors, in bitstream order, is by construction the B-pictures that
 * belong between them in display order too). So finalizing a newly-
 * decoded anchor doesn't display it - it releases whatever anchor was
 * *previously* sitting in refFuture_ (which, by the same argument, has
 * now had all its dependent B-pictures pass through) for display, and
 * the new anchor takes over refFuture_. This means anchor pictures are
 * always delivered exactly one anchor "late": the very first anchor of a
 * stream produces DecodeStatus::kNeedMoreData (decoded, but nothing
 * releasable yet), and the true final anchor isn't released until
 * next() finds no more start codes at all (see the ending flush in
 * next()) - unavoidable given B-pictures exist at all in the format, and
 * applied uniformly (even to a stream that turns out to have no
 * B-pictures) since the decoder has no way to know in advance whether
 * one is coming. Total frame *count* delivered is unaffected - only the
 * timing of the last one or two next()/write() calls that produce it.
 *
 * ## Streaming/incremental input
 *
 * setInput()'s original contract (still the default: `isLastChunk=true`)
 * is that reaching the end of the buffer it's given always means "this
 * unit is complete" - correct for a self-contained buffer (one whole
 * clip/picture at a time, the way every example/test in this project
 * feeds it), but wrong for a caller forwarding a container-demuxed
 * stream in chunks that don't align to start-code-unit boundaries (found
 * via real-world testing: arduino-audio-tools' DemuxerMPG delivers
 * unbounded video PES data to the video decoder in whatever increments
 * happen to be buffered, not one complete access unit at a time - see
 * that project's ContainerMPG.h::continuePayload()). Feeding such
 * boundary-split data through the `isLastChunk=true` (default) contract
 * doesn't just fail cleanly - it can silently reconstruct a picture with
 * whole rows of stale/wrong macroblocks (the picture's true final slice
 * got dropped mid-decode, but a subsequent picture_header still
 * finalizes and releases it for display as if nothing were missing).
 *
 * Passing `isLastChunk=false` switches to real incremental parsing
 * instead: a unit that can't yet be proven complete (StartUnit::complete
 * is false - simply running out of buffer, not a genuine following start
 * code) is held back in `carry_` (see CarryBuffer) rather than processed
 * or discarded, and completed by whatever setInput() call provides the
 * rest of its bytes - however many calls that takes. pictureInProgress_/
 * skipPicture_/etc. are untouched while waiting, so a picture (or an
 * ignored P-picture's header) paused mid-unit resumes exactly where it
 * left off once its data is complete. The caller decides `isLastChunk`
 * per call - true for a self-contained buffer (unchanged default
 * behavior), false while more of a live/container stream is still to
 * come.
 */

namespace tinympg {

/// Outcome of one Decoder::next() call (one start-code-delimited unit processed, or a picture boundary detected).
enum class DecodeStatus {
  kOk,           ///< a new picture is ready for display via frame()
  kNeedMoreData, ///< this unit wasn't picture-completing, or a picture decoded but isn't display-ready yet - keep going
  kUnsupported,  ///< stream uses a feature this decoder doesn't implement (e.g. a D picture)
  kError,        ///< corrupt/truncated bitstream
  kAllocationError, ///< a picture buffer allocation failed (out of memory) - see StdAllocator.h; not a bitstream problem, but treated as terminal the same way kError is
};

namespace detail {

// ISO/IEC 11172-2 clause 2.4.3.1 start_code values relevant to an elementary video stream.
static const uint8_t kStartCodePicture = 0x00;
static const uint8_t kStartCodeSliceMin = 0x01;
static const uint8_t kStartCodeSliceMax = 0xAF;
static const uint8_t kStartCodeUserData = 0xB2;
static const uint8_t kStartCodeSequenceHeader = 0xB3;
static const uint8_t kStartCodeExtension = 0xB5;
static const uint8_t kStartCodeSequenceEnd = 0xB7;
static const uint8_t kStartCodeGroupStart = 0xB8;

/// One start-code-delimited unit: `data`/`size` point directly into the caller's own input buffer.
struct StartUnit {
  uint8_t type = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t unitEnd = 0;  ///< offset (from the scanned buffer's start) one past this unit's data - see StartCodeReader::advance()
  /**
   * True if `unitEnd` was determined by finding a genuine subsequent start
   * code (this unit is definitely whole); false if it was determined by
   * simply running out of buffer (`unitEnd == size_`) - which could mean
   * this really is the stream's final unit, *or* that the rest of it just
   * hasn't arrived yet in a caller feeding data incrementally (see
   * Decoder::setInput()'s `isLastChunk` parameter, which decides how this
   * flag is interpreted).
   */
  bool complete = false;
};

/**
 * Scans an in-memory buffer for ISO/IEC 11172-2 start codes (00 00 01 XX).
 * Unlike TinyH264's NalReader, peek() does *not* advance the read
 * position by itself - advance() commits to whatever unit peek() most
 * recently returned, so a caller can inspect a unit's type and decide
 * *not* to consume it yet (used by Decoder::next() to detect picture
 * boundaries without losing the unit that signaled one).
 */
class StartCodeReader {
 public:
  void reset(const uint8_t* buf, size_t size) {
    buf_ = buf;
    size_ = size;
    pos_ = 0;
  }

  /**
   * Finds the next start code at or after the current position and fills
   * `unit` with its type and payload span (bounded by the *following*
   * start code, or the end of the buffer - see StartUnit::complete for how
   * to tell those two cases apart). Returns false (leaving `unit`
   * untouched) once not even one complete start code (00 00 01 XX) is
   * available yet. Safe to call repeatedly without an intervening
   * advance() - always returns the same unit until advance() is called.
   */
  bool peek(StartUnit* unit) const {
    size_t codeStart;
    if (!findStartCode(pos_, &codeStart)) return false;
    size_t typePos = codeStart + 3;
    if (typePos >= size_) return false;  // truncated start code, no type byte yet
    size_t dataStart = typePos + 1;
    size_t nextCodeStart;
    bool foundNext = findStartCode(dataStart, &nextCodeStart);
    size_t dataEnd = foundNext ? nextCodeStart : size_;
    unit->type = buf_[typePos];
    unit->data = buf_ + dataStart;
    unit->size = dataEnd - dataStart;
    unit->unitEnd = dataEnd;
    unit->complete = foundNext;
    return true;
  }

  /// Commits to (consumes) the unit most recently returned by peek().
  void advance(const StartUnit& unit) { pos_ = unit.unitEnd; }

  /// Bytes at or after the current (unconsumed) read position - what a
  /// caller feeding data incrementally needs to preserve across calls
  /// whenever peek() couldn't find a complete unit yet (see
  /// Decoder::setInput()'s `isLastChunk` parameter).
  const uint8_t* remainingData() const { return buf_ + pos_; }
  size_t remainingSize() const { return size_ - pos_; }

 private:
  bool findStartCode(size_t from, size_t* codeStart) const {
    for (size_t i = from; i + 2 < size_; i++) {
      if (buf_[i] == 0 && buf_[i + 1] == 0 && buf_[i + 2] == 1) {
        *codeStart = i;
        return true;
      }
    }
    return false;
  }

  const uint8_t* buf_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

/**
 * Owns a copy of whatever tail bytes of an incomplete start-code unit need
 * to survive across separate Decoder::setInput() calls (see that method's
 * `isLastChunk` parameter) - the caller's own buffer for those bytes isn't
 * guaranteed to remain valid once its write()/setInput() call returns, so
 * they can't just be referenced in place the way a *complete* unit's bytes
 * are (see StartCodeReader's own comment on the normal zero-copy case).
 * No-throw allocation-failure handling, same discipline as Buffer (see
 * common/mpg_frame.h's file comment) - `assign()`/`append()` return false,
 * existing contents left untouched, rather than crashing or leaving this
 * object half-updated.
 */
template <typename Allocator>
class CarryBuffer {
 public:
  ~CarryBuffer() { clear(); }

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  /// Replaces this buffer's contents with `src[0..n)`. `src` may alias
  /// this buffer's own current data() (the common case: trimming down to
  /// just the still-unconsumed tail of what this buffer already holds).
  bool assign(const uint8_t* src, size_t n) {
    if (n == 0) {
      clear();
      return true;
    }
    uint8_t* p = tryAllocate(n);
    if (!p) return false;
    memcpy(p, src, n);  // p is always a fresh allocation, never aliases src
    clear();
    data_ = p;
    size_ = n;
    return true;
  }

  /// Appends `src[0..n)` after this buffer's current contents.
  bool append(const uint8_t* src, size_t n) {
    if (n == 0) return true;
    size_t newSize = size_ + n;
    uint8_t* p = tryAllocate(newSize);
    if (!p) return false;
    if (data_) memcpy(p, data_, size_);
    memcpy(p + size_, src, n);
    clear();
    data_ = p;
    size_ = newSize;
    return true;
  }

  void clear() {
    if (data_) {
      alloc_.deallocate(data_, size_);
      data_ = nullptr;
      size_ = 0;
    }
  }

 private:
  uint8_t* tryAllocate(size_t n) {
#if __cpp_exceptions
    try {
      return alloc_.allocate(n);
    } catch (...) {
      return nullptr;
    }
#else
    return alloc_.allocate(n);
#endif
  }

  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  Allocator alloc_;
};

}  // namespace detail

/**
 * Top-level MPEG-1 (Phase A/B/C: I-, P-, and B-picture) decoder: owns
 * start-code demuxing, the active sequence_header state, the picture
 * currently being reconstructed, the two held reference pictures
 * B-pictures predict from, and the decode-to-display reordering this
 * file's top comment describes. This is the engine behind the public
 * TinyMPGDecoder<Allocator> facade (see TinyMPGDecoder.h); most callers
 * should use that instead of this class directly.
 */
template <typename Allocator = StdAllocator<uint8_t>>
class Decoder {
 public:
  /**
   * Overrides the MPG_MAX_WIDTH/MPG_MAX_HEIGHT compile-time defaults
   * (mpg_config.h) for this Decoder instance's picture-buffer allocation
   * ceiling. A stream whose sequence_header declares a coded resolution
   * bigger than this is rejected as kUnsupported. If any picture buffer
   * is already allocated, this releases and reclaims it so the next
   * decode reallocates at the new size.
   */
  void setMaxDimension(int maxWidth, int maxHeight) {
    maxWidth_ = maxWidth;
    maxHeight_ = maxHeight;
    curFrame_.setMaxDimension(maxWidth, maxHeight);
    refPast_.setMaxDimension(maxWidth, maxHeight);
    refFuture_.setMaxDimension(maxWidth, maxHeight);
  }
  int maxWidth() const { return maxWidth_; }
  int maxHeight() const { return maxHeight_; }

  /**
   * When set, P-pictures are discarded as soon as their picture_header is
   * parsed (codingType == 2, clause 6.3.9) - none of their slices are
   * decoded, and next() never returns a status for them at all (not even
   * kUnsupported - unlike a genuinely unsupported feature, this is a
   * caller-requested policy, not a bitstream problem). refFuture_/
   * haveAnchor_ are left exactly as they were, so whatever anchor is
   * already held keeps serving as the reference for whatever picture
   * needs one next - a P-picture chained off the ignored one, or a
   * B-picture that would otherwise have referenced it, ends up predicting
   * from an older anchor instead, which is an inherent, accepted
   * consequence of skipping frames this way, not a bug. I- and B-pictures
   * are unaffected. Off (decode everything) by default.
   */
  void setIgnorePFrames(bool active) { ignorePFrames_ = active; }
  bool ignorePFrames() const { return ignorePFrames_; }

  /**
   * Feeds one buffer of MPEG-1 video elementary stream data (start-code
   * delimited, see the file comment) and prepares to decode it one unit
   * at a time via next(). Call setInput() again once inputExhausted() is
   * true; there is no separate reset().
   *
   * `isLastChunk` (default true, matching this method's original
   * single-call contract) tells next() how to treat a unit whose end
   * can't be determined because this buffer simply runs out first (clause
   * 2.4.3.1 doesn't require a trailing marker after a stream's last
   * unit, so this is indistinguishable from "more data is coming" without
   * the caller saying which it is): true treats reaching the buffer's end
   * as that unit being complete (today's original behavior - correct for
   * a self-contained buffer, e.g. one whole clip/picture per call, the
   * way every example/test in this project uses it). Pass false when
   * feeding a live/container-demuxed stream in caller-chosen chunks that
   * may not align to start-code-unit boundaries (e.g. DemuxerMPG's PES
   * packetization, see docs/decoding.md's "Streaming/incremental input"
   * section) - an incomplete trailing unit is then held back internally
   * and completed by the *next* setInput() call's data instead of being
   * processed prematurely (or having its tail silently dropped). Returns
   * false (without changing any decoder state) only if internally
   * carrying over a still-incomplete unit's bytes from a previous
   * isLastChunk=false call required an allocation that failed - see
   * StdAllocator.h; next() also starts reporting kAllocationError once
   * that's happened, so checking this return value is optional but
   * surfaces the failure a call earlier.
   */
  bool setInput(const uint8_t* data, size_t size, bool isLastChunk = true) {
    isLastChunk_ = isLastChunk;
    if (!carry_.empty()) {
      if (!carry_.append(data, size)) {
        pendingAllocationError_ = true;
        return false;
      }
      reader_.reset(carry_.data(), carry_.size());
    } else {
      reader_.reset(data, size);
    }
    inputExhausted_ = false;
    pendingAllocationError_ = false;
    return true;
  }

  /// True once next() has found no further start codes in the current input buffer.
  bool inputExhausted() const { return inputExhausted_; }

  /**
   * Processes exactly one start-code-delimited unit from the current
   * input, or - if a picture-terminating unit is found while a picture is
   * already in progress - finalizes that picture instead, without
   * consuming the terminating unit (it's reprocessed on the *next* call,
   * see the file comment). Returns kOk once a picture is ready for
   * *display* (see the file comment's "Output reordering" section - this
   * is not always the picture that *decoding* just finished); call
   * repeatedly to drain the input set by setInput().
   */
  DecodeStatus next() {
    if (pendingAllocationError_) return DecodeStatus::kAllocationError;
    for (;;) {
      detail::StartUnit u;
      bool found = reader_.peek(&u);

      /*
       * Streaming mode (isLastChunk_ == false, see setInput()): a unit
       * whose end couldn't be determined - either none was found at all,
       * or peek() only found where it *starts* - might simply be waiting
       * on bytes this decoder hasn't been given yet, not actually
       * finished. Hold everything from here on (reader_.remainingData())
       * in carry_ and wait for the next setInput() call to complete it,
       * rather than either processing a truncated unit or discarding its
       * tail (see the file comment's "Streaming/incremental input", added
       * once real-world DemuxerMPG-fed playback showed this could
       * silently corrupt whichever picture straddled a chunk boundary).
       */
      if (!isLastChunk_ && (!found || !u.complete)) {
        if (!carry_.assign(reader_.remainingData(), reader_.remainingSize())) {
          pendingAllocationError_ = true;
          return DecodeStatus::kAllocationError;
        }
        inputExhausted_ = true;
        return DecodeStatus::kNeedMoreData;
      }

      if (!found) {
        inputExhausted_ = true;
        if (pictureInProgress_) {
          DecodeStatus s = finalizePicture();
          if (s != DecodeStatus::kNeedMoreData) return s;
          /*
           * s == kNeedMoreData here means finalizePicture() just decoded
           * the *first* anchor this decoder has ever seen (haveAnchor_
           * was false going in - see that function's own comment) - it
           * has nothing else to release yet, so it deliberately doesn't
           * return kOk. But if that anchor is *also* the last thing in
           * the input (true here - the input is already exhausted), it
           * still needs to be delivered somehow: fall through to the
           * flush check below in this same call, rather than requiring a
           * second next() call that a caller looping only "while status
           * == kOk" (see TinyMPGDecoder::write()) would never make, since
           * this call itself is about to return kNeedMoreData otherwise -
           * a single-I-picture stream (or any stream whose *first*
           * anchor is also its last) would silently never be delivered
           * at all without this. haveAnchor_ is true now (just set by
           * finalizePicture()), so the flush check below applies
           * unconditionally once reached this way.
           */
        }
        // Nothing left to decode - flush a still-undisplayed anchor, if
        // one is pending (see the file comment's "Output reordering"
        // section). refFutureShown_ guards this firing more than once
        // while the caller keeps calling next() with no new input.
        if (haveAnchor_ && !refFutureShown_) {
          displayFrame_ = &refFuture_;
          refFutureShown_ = true;
          return DecodeStatus::kOk;
        }
        return DecodeStatus::kNeedMoreData;
      }

      /*
       * Reached with a unit in hand - either genuinely complete
       * (u.complete), or (isLastChunk_ == true and) simply running out of
       * buffer at this unit, which clause 2.4.3.1 allows for the true
       * last unit of a stream with no trailing marker required - the
       * original, single-call contract this branch preserves unchanged.
       */
      bool terminatesPicture = u.type == detail::kStartCodePicture ||
                                u.type == detail::kStartCodeGroupStart ||
                                u.type == detail::kStartCodeSequenceHeader ||
                                u.type == detail::kStartCodeSequenceEnd;
      if (terminatesPicture && pictureInProgress_) return finalizePicture();

      reader_.advance(u);

      if (u.type == detail::kStartCodeSequenceHeader) {
        BitReader br(u.data, u.size);
        SequenceInfo seq;
        if (!parseSequenceHeader(br, &seq)) return DecodeStatus::kError;
        if (seq.codedWidth > (uint32_t)maxWidth_ || seq.codedHeight > (uint32_t)maxHeight_) {
          return DecodeStatus::kUnsupported;
        }
        seq_ = seq;
        haveSeq_ = true;
        continue;
      }

      if (u.type == detail::kStartCodePicture) {
        if (!haveSeq_) return DecodeStatus::kError;
        BitReader br(u.data, u.size);
        PictureInfo pic;
        if (!parsePictureHeader(br, &pic)) return DecodeStatus::kError;
        if (pic.codingType == 2 && ignorePFrames_) {
          // Deliberately discard this P-picture without decoding it (see
          // setIgnorePFrames()) - reuses skipPicture_, the same mechanism
          // an unsupported picture type uses below to drop its slices,
          // but silently (no status returned, loop just keeps going)
          // since this isn't a bitstream problem.
          skipPicture_ = true;
          pictureInProgress_ = false;
          continue;
        }
        if (pic.codingType < 1 || pic.codingType > 3) {  // not I, P, or B (D not implemented, see docs/scope.md)
          skipPicture_ = true;
          pictureInProgress_ = false;
          return DecodeStatus::kUnsupported;
        }
        if (pic.codingType == 2 && !haveAnchor_) {
          // A P-picture predicts from the most recently decoded picture
          // (clause 7.6.1) - one must already exist.
          return DecodeStatus::kError;
        }
        if (pic.codingType == 3 && !haveTwoAnchors_) {
          // A B-picture predicts from the two anchors surrounding it in
          // display order (clause 7.6.1) - both must already exist.
          return DecodeStatus::kError;
        }
        if (!curFrame_.setSize((int)seq_.width, (int)seq_.height, (int)seq_.codedWidth,
                                (int)seq_.codedHeight)) {
          return DecodeStatus::kAllocationError;
        }
        pic_ = pic;
        skipPicture_ = false;
        pictureInProgress_ = true;
        sliceSeen_ = false;
        if (pic.codingType == 2) {
          curMvP_[0] = curMvP_[1] = 0;
        } else if (pic.codingType == 3) {
          curMvBFwd_[0] = curMvBFwd_[1] = 0;
          curMvBBack_[0] = curMvBBack_[1] = 0;
          curHasForwardB_ = curHasBackwardB_ = false;
          prevWasIntraB_ = false;
        }
        continue;
      }

      if (u.type >= detail::kStartCodeSliceMin && u.type <= detail::kStartCodeSliceMax) {
        if (skipPicture_) continue;  // ignore slices belonging to an unsupported picture
        if (!pictureInProgress_) return DecodeStatus::kError;
        int mbY = u.type - detail::kStartCodeSliceMin;
        if (mbY >= (int)seq_.mbHeight) return DecodeStatus::kError;
        bool ok;
        switch (pic_.codingType) {
          case 1: ok = decodeSliceIntra(u.data, u.size, mbY); break;
          case 2: ok = decodeSliceP(u.data, u.size, mbY); break;
          default: ok = decodeSliceB(u.data, u.size, mbY); break;
        }
        if (!ok) return DecodeStatus::kError;
        sliceSeen_ = true;
        continue;
      }

      // extension_start_code, user_data_start_code, sequence_end_code (when
      // no picture was in progress to terminate), and any reserved/system
      // start code: nothing this decoder needs, skip and keep going.
    }
  }

  /// The most recently display-ready picture (valid after next() returns DecodeStatus::kOk - see the file comment's "Output reordering" section for which picture that actually is).
  const Frame<Allocator>& frame() const { return *displayFrame_; }

  /**
   * Reserves this decoder's picture buffers up front instead of the
   * default allocate-on-first-decode behavior. Entirely optional - next()
   * still allocates lazily on its own if this was never called. Returns
   * false if any of the three allocations failed (out of memory) - see
   * StdAllocator.h; whichever of the three succeeded before the first
   * failure are left allocated (harmless - end() releases them, or a
   * retried begin() call skips them via ensureAllocated()'s "allocate
   * once" contract and just attempts the rest again).
   */
  bool begin() {
    return curFrame_.ensureAllocated() && refPast_.ensureAllocated() &&
           refFuture_.ensureAllocated();
  }

  /**
   * Releases all picture buffers and resets this decoder's stream state
   * (cached sequence_header, in-progress-picture bookkeeping, reference-
   * picture availability, input-exhaustion flag) back to how a freshly
   * constructed Decoder starts. Not required before destruction - only
   * useful for freeing memory before that, while this object is still
   * alive.
   */
  void end() {
    curFrame_.release();
    refPast_.release();
    refFuture_.release();
    displayFrame_ = &curFrame_;
    haveSeq_ = false;
    haveAnchor_ = false;
    haveTwoAnchors_ = false;
    refFutureShown_ = false;
    pictureInProgress_ = false;
    skipPicture_ = false;
    sliceSeen_ = false;
    inputExhausted_ = false;
    carry_.clear();
    isLastChunk_ = true;
    pendingAllocationError_ = false;
  }

 private:
  /**
   * Finishes the in-progress picture. A B-picture is immediately display-
   * ready (nothing ever references it). An I/P picture ("anchor") instead
   * releases whichever anchor was previously held in refFuture_ (now
   * safe to display - see the file comment) and takes its place there
   * itself, undisplayed for now; the very first anchor of a stream has
   * nothing to release yet, so it just gets stored (kNeedMoreData) -
   * and so does an anchor arriving after refFuture_'s content has
   * *already* been released (refFutureShown_ true - see next()'s
   * end-of-input flush): there's equally nothing new to release in that
   * case, and treating it as if there were would re-deliver a picture
   * the caller already saw. This matters for exactly the use case
   * setInput()'s own comment documents - feeding more data after
   * inputExhausted() was already true is normal, not just a benchmark
   * replaying one clip repeatedly (which is how this was actually
   * found: the same stale-redelivery bug, just made obvious by calling
   * write() on the same short clip many times in a row). refPast_ and
   * haveTwoAnchors_ are deliberately left untouched either way - they
   * stay valid B-picture reference material for a stream that's
   * genuinely continuing, which is the common case this exception
   * shouldn't disturb.
   */
  DecodeStatus finalizePicture() {
    pictureInProgress_ = false;
    if (!sliceSeen_) return DecodeStatus::kError;  // picture_header with no slice data at all

    if (pic_.codingType == 3) {  // B-picture: always immediately displayable
      displayFrame_ = &curFrame_;
      return DecodeStatus::kOk;
    }

    // I or P: an anchor picture.
    if (haveAnchor_ && !refFutureShown_) {
      std::swap(refPast_, refFuture_);  // old refFuture_ -> refPast_: now displayable, and a valid forward ref
      haveTwoAnchors_ = true;
      std::swap(refFuture_, curFrame_);  // the picture just decoded becomes the new refFuture_
      refFutureShown_ = false;
      displayFrame_ = &refPast_;
      return DecodeStatus::kOk;
    }
    std::swap(refFuture_, curFrame_);
    haveAnchor_ = true;
    refFutureShown_ = false;
    return DecodeStatus::kNeedMoreData;
  }

  /**
   * Decodes one slice's worth of macroblocks (clause 2.4.3.7's slice()
   * syntax, I-picture case) into curFrame_, starting at macroblock row
   * mbY (from the slice_start_code's own low byte). DC predictors reset
   * to 128 here (once per slice, clause 2.4.3.7) rather than once per
   * picture. Trailing zero-padding bits between the last macroblock and
   * the next (byte-aligned) start code are handled by
   * decodeMbAddrIncrement() itself matching the "end of slice" guard code
   * (see mpg_vlc_tables.h) - not treated as an error.
   */
  bool decodeSliceIntra(const uint8_t* data, size_t size, int mbY) {
    BitReader br(data, size);
    int qscale = (int)br.u(5);
    if (qscale == 0) return false;
    while (br.flag()) br.skipBits(8);  // extra_bit_slice / extra_information_slice, clause 2.4.3.7
    if (br.error()) return false;

    int32_t lastDc[3] = {128, 128, 128};
    int mbWidth = (int)seq_.mbWidth;
    int mbHeight = (int)seq_.mbHeight;

    MbAddrIncrement first = decodeMbAddrIncrement(br);
    if (first.error || first.endOfSlice) return false;  // an empty slice is malformed
    int mbX = first.value;
    if (mbX >= mbWidth) return false;

    for (bool isFirst = true;; isFirst = false) {
      if (!isFirst) {
        MbAddrIncrement r = decodeMbAddrIncrement(br);
        if (r.error || r.endOfSlice) break;  // trailing padding, or a genuine bitstream error either way ends this slice
        if (r.value != 0) return false;      // any skip is illegal in an I-picture (clause 7.2.1)
        mbX++;
        if (mbX >= mbWidth) {
          mbX = 0;
          mbY++;
          if (mbY >= mbHeight) return false;
        }
      }

      MbTypeIntra mt = decodeMbTypeIntra(br);
      if (mt.error) return false;
      if (mt.quant) {
        qscale = (int)br.u(5);
        if (qscale == 0) return false;
      }
      if (!decodeIntraMacroblock(br, curFrame_, mbX, mbY, lastDc, seq_.intraMatrix, qscale)) {
        return false;
      }
    }
    return true;
  }

  /**
   * Decodes one slice's worth of macroblocks, P-picture case (clause
   * 2.4.3.7's slice(), Phase B - see docs/scope.md). Structurally the
   * same skeleton as decodeSliceIntra() (same quantizer_scale/
   * extra_bit_slice preamble, same decodeMbAddrIncrement() walk for
   * finding the first macroblock's column), but a nonzero
   * macroblock_address_increment is now a run of *skipped* macroblocks
   * to motion-compensate (clause 7.6.6, "reconstruction of skipped
   * macroblocks") rather than an error - each one predicted directly from
   * curMvP_ (the most recently decoded real macroblock's motion vector in
   * this picture, or (0,0) if nothing has set it yet) with no residual
   * and no bitstream data of its own. curMvP_ is decoder-level (not
   * slice-local) state, reset once per *picture* rather than once per
   * slice - a skip run at the very start of a slice (other than the
   * picture's first) correctly inherits the last real motion vector from
   * the *previous* slice, matching FFmpeg's positional (not per-slice)
   * skip semantics. lastMv (the differential motion vector predictor)
   * *is* reset every slice (clause 2.4.3.7) - it and curMvP_ are
   * deliberately different state; see decodePMacroblock()'s comment
   * (mpg_macroblock.h) for why they can diverge.
   */
  bool decodeSliceP(const uint8_t* data, size_t size, int mbY) {
    BitReader br(data, size);
    int qscale = (int)br.u(5);
    if (qscale == 0) return false;
    while (br.flag()) br.skipBits(8);
    if (br.error()) return false;

    int32_t lastDc[3] = {128, 128, 128};
    int32_t lastMv[2] = {0, 0};
    int mbWidth = (int)seq_.mbWidth;
    int mbHeight = (int)seq_.mbHeight;

    MbAddrIncrement first = decodeMbAddrIncrement(br);
    if (first.error || first.endOfSlice) return false;
    int mbX = first.value;
    if (mbX >= mbWidth) return false;

    for (bool isFirst = true;; isFirst = false) {
      if (!isFirst) {
        // Advancing to (and, for a skip run, predicting) each of the next
        // r.value + 1 macroblock positions happens *here*, while preparing
        // for the next decode - not as unconditional cleanup after the
        // previous one - so that reaching the true last macroblock of the
        // picture (which decodeMbAddrIncrement()'s next call correctly
        // reports as endOfSlice, breaking the loop below before any of
        // this runs again) never spuriously looks like a mbY overflow.
        MbAddrIncrement r = decodeMbAddrIncrement(br);
        if (r.error || r.endOfSlice) break;
        for (int s = 0; s < r.value; s++) {
          mbX++;
          if (mbX >= mbWidth) {
            mbX = 0;
            mbY++;
            if (mbY >= mbHeight) return false;
          }
          predictMacroblock(refFuture_, curFrame_, mbX, mbY, curMvP_[0], curMvP_[1]);
        }
        mbX++;
        if (mbX >= mbWidth) {
          mbX = 0;
          mbY++;
          if (mbY >= mbHeight) return false;
        }
      }

      if (!decodePMacroblock(br, curFrame_, refFuture_, mbX, mbY, lastDc, lastMv, curMvP_,
                              seq_.intraMatrix, seq_.nonIntraMatrix, &qscale,
                              pic_.forwardFCode, pic_.fullPelForwardVector)) {
        return false;
      }
    }
    return true;
  }

  /**
   * Decodes one slice's worth of macroblocks, B-picture case (clause
   * 2.4.3.7's slice(), Phase C - see docs/scope.md). Same skeleton as
   * decodeSliceP(), generalized to two independent directions: a skip
   * run motion-compensates from curMvBFwd_/curMvBBack_ using whichever
   * direction(s) curHasForwardB_/curHasBackwardB_ (also decoder-level,
   * picture-persistent state, same rationale as decodeSliceP()'s curMvP_)
   * say the *previous* real macroblock used (clause 7.6.6 - unlike a
   * P-picture's skip, always unconditionally forward-only, a B-picture's
   * skip reuses its predecessor's entire prediction mode, not just a
   * motion vector). A skip immediately after an intra macroblock is
   * rejected as a bitstream error (prevWasIntraB_) - there is no
   * meaningful motion vector for such a skip to inherit, matching
   * FFmpeg's "skip with previntra" check.
   */
  bool decodeSliceB(const uint8_t* data, size_t size, int mbY) {
    BitReader br(data, size);
    int qscale = (int)br.u(5);
    if (qscale == 0) return false;
    while (br.flag()) br.skipBits(8);
    if (br.error()) return false;

    int32_t lastDc[3] = {128, 128, 128};
    int32_t lastMvFwd[2] = {0, 0};
    int32_t lastMvBack[2] = {0, 0};
    int mbWidth = (int)seq_.mbWidth;
    int mbHeight = (int)seq_.mbHeight;

    MbAddrIncrement first = decodeMbAddrIncrement(br);
    if (first.error || first.endOfSlice) return false;
    int mbX = first.value;
    if (mbX >= mbWidth) return false;

    for (bool isFirst = true;; isFirst = false) {
      if (!isFirst) {
        MbAddrIncrement r = decodeMbAddrIncrement(br);
        if (r.error || r.endOfSlice) break;
        for (int s = 0; s < r.value; s++) {
          if (prevWasIntraB_) return false;  // "skip with previntra" - nothing valid to inherit
          if (!curHasForwardB_ && !curHasBackwardB_) return false;  // nothing decoded yet to inherit from
          mbX++;
          if (mbX >= mbWidth) {
            mbX = 0;
            mbY++;
            if (mbY >= mbHeight) return false;
          }
          predictMacroblockBi(curHasForwardB_ ? &refPast_ : nullptr,
                               curHasBackwardB_ ? &refFuture_ : nullptr, curFrame_, mbX, mbY,
                               curMvBFwd_[0], curMvBFwd_[1], curMvBBack_[0], curMvBBack_[1]);
        }
        mbX++;
        if (mbX >= mbWidth) {
          mbX = 0;
          mbY++;
          if (mbY >= mbHeight) return false;
        }
      }

      bool wasIntra = false;
      if (!decodeBMacroblock(br, curFrame_, refPast_, refFuture_, mbX, mbY, lastDc, lastMvFwd,
                              lastMvBack, curMvBFwd_, curMvBBack_, &curHasForwardB_,
                              &curHasBackwardB_, &wasIntra, seq_.intraMatrix, seq_.nonIntraMatrix,
                              &qscale, pic_.forwardFCode, pic_.backwardFCode,
                              pic_.fullPelForwardVector, pic_.fullPelBackwardVector)) {
        return false;
      }
      prevWasIntraB_ = wasIntra;
    }
    return true;
  }

  detail::StartCodeReader reader_;
  bool inputExhausted_ = false;
  detail::CarryBuffer<Allocator> carry_;  ///< see setInput()'s `isLastChunk` parameter
  bool isLastChunk_ = true;
  bool pendingAllocationError_ = false;  ///< a carry_ grow/trim failed - see setInput()/next()

  SequenceInfo seq_;
  bool haveSeq_ = false;
  PictureInfo pic_;  ///< the in-progress picture's header fields (coding type, f_codes, ...)
  bool pictureInProgress_ = false;
  bool skipPicture_ = false;  ///< true while draining slices that belong to an already-reported-unsupported picture
  bool sliceSeen_ = false;    ///< true once at least one slice has been decoded for the in-progress picture

  Frame<Allocator> curFrame_;   ///< the picture currently being reconstructed
  Frame<Allocator> refPast_;    ///< the held reference anchor temporally *before* a B-picture (also the older of the two most recent anchors generally)
  Frame<Allocator> refFuture_;  ///< the most recently *decoded* anchor - a P-picture's one reference, and a B-picture's temporally-future reference
  bool haveAnchor_ = false;      ///< true once refFuture_ holds a real decoded anchor
  bool haveTwoAnchors_ = false;  ///< true once refPast_ *also* holds a real decoded anchor (required before any B-picture)
  bool refFutureShown_ = false;  ///< true once refFuture_'s current content has already been released for display (see finalizePicture()/the end-of-input flush in next())
  Frame<Allocator>* displayFrame_ = &curFrame_;  ///< which buffer frame() currently exposes

  int32_t curMvP_[2] = {0, 0};  ///< P-picture skip-inheritance state (decoder-level, reset per picture) - see decodeSliceP()
  int32_t curMvBFwd_[2] = {0, 0};   ///< B-picture forward skip-inheritance motion vector - see decodeSliceB()
  int32_t curMvBBack_[2] = {0, 0};  ///< B-picture backward skip-inheritance motion vector - see decodeSliceB()
  bool curHasForwardB_ = false;     ///< whether the most recent real B-picture macroblock used forward prediction
  bool curHasBackwardB_ = false;    ///< whether the most recent real B-picture macroblock used backward prediction
  bool prevWasIntraB_ = false;      ///< whether the most recently decoded B-picture macroblock (real or skip) was intra

  int maxWidth_ = MPG_MAX_WIDTH;
  int maxHeight_ = MPG_MAX_HEIGHT;
  bool ignorePFrames_ = false;  ///< see setIgnorePFrames() - a caller policy, not stream state, so end() doesn't reset it (matches maxWidth_/maxHeight_)
};

}  // namespace tinympg
