#pragma once
#ifndef DSY_WAVEFOFOLDER_H
#define DSY_WAVEFOFOLDER_H

#include <stdint.h>
#include "Utility/dcblock.h"
#ifdef __cplusplus

namespace daisysp
{
/** Basic wavefolder module.

Amplitude of input determines level of folding.
Amplitudes of magnitude > 1.0 will start to fold.

Original author(s) : Nick Donaldson
Year : 2022

By default the module applies its triangular transfer curve sample by
sample, exactly as it always has. SetAntialias(true) enables an optional
interval-averaging mode: each output is the exact average of the current
transfer curve (offset, gain, then the original triangular fold) over the
line segment joining the previous valid input to the current one. The
average is evaluated from a closed-form antiderivative of the triangular
wave, so its cost does not grow with the number of fold points crossed.
It allocates no memory and keeps sample-by-sample processing.

@code
Wavefolder folder;
folder.Init();
folder.SetGain(2.0f);
folder.SetAntialias(true); // optional; defaults to off
// ...existing Process()/SetGain()/SetOffset() usage is unchanged...
float out = folder.Process(in);
// Call Reset() on a discontinuous source change; gain, offset and mode
// are kept, only the interval history is cleared.
folder.Reset();
@endcode
*/
class Wavefolder
{
  public:
    Wavefolder() {}
    ~Wavefolder() {}
    /** Initializes the wavefolder module.
    */
    void Init();
    /** applies wavefolding to input.
        Non-finite inputs (NaN/inf) return 0 and do not overwrite the
        stored interval history.
    */
    float Process(float in);
    /**
        \param gain Set input gain.
        Supports negative values for thru-zero
    */
    inline void SetGain(float gain) { gain_ = gain; }
    /**
        \param offset Offset odded to input (pre-gain) for asymmetrical folding.
    */
    inline void SetOffset(float offset) { offset_ = offset; }

    /** Enable or disable interval-averaged (antialiased) processing.
        Disabled by default. Toggling the setting clears the interval
        history; setting the same value again keeps it. The pointwise
        folding curve itself is never modified.
    */
    void SetAntialias(bool antialias);

    /** Clear the interval history only. The current gain, offset and
        antialias mode are left unchanged. The next processed sample is
        output directly from the folding curve, as on the first call
        after Init.
    */
    void Reset();

  private:
    /** The original pointwise transfer, unchanged. */
    float Pointwise(float in) const;

    float  gain_, offset_;
    bool   antialias_;
    bool   hist_valid_;
    double hist_;
};
} // namespace daisysp
#endif
#endif
