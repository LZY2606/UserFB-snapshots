#pragma once
#ifndef DSY_OVERDRIVE_H
#define DSY_OVERDRIVE_H

#include <stdint.h>
#ifdef __cplusplus

/** @file overdrive.h */

namespace daisysp
{
/**
       @brief Distortion / Overdrive Module
       @author Ported by Ben Sergentanis
       @date Jan 2021
       Ported from pichenettes/eurorack/plaits/dsp/fx/overdrive.h \n
       to an independent module. \n
       Original code written by Emilie Gillet in 2014. \n

       By default the module applies its transfer curve sample by sample,
       exactly as it always has. SetAntialias(true) enables an optional
       interval-averaging mode: each output is the exact average of the
       current transfer curve over the line segment joining the previous
       valid input to the current one. This attenuates the aliasing created
       by pointwise nonlinear processing without allocating memory, changing
       the sample rate, or iterating over fold crossings.

       @code
       Overdrive overdrive;
       overdrive.Init();
       overdrive.SetDrive(0.8f);
       overdrive.SetAntialias(true); // optional; defaults to off
       // ...existing Process()/SetDrive() usage is unchanged...
       float out = overdrive.Process(in);
       // Call Reset() on a discontinuous source change; drive and mode
       // are kept, only the interval history is cleared.
       overdrive.Reset();
       @endcode
*/
class Overdrive
{
  public:
    Overdrive() {}
    ~Overdrive() {}

    /** Initializes the module with 0 gain */
    void Init();

    /** Get the next sample
      \param in Input to be overdriven. Non-finite inputs (NaN/inf)
                return 0 and do not overwrite the stored interval history.
    */
    float Process(float in);

    /** Set the amount of drive
          \param drive Works from 0-1
      */
    void SetDrive(float drive);

    /** Enable or disable interval-averaged (antialiased) processing.
        Disabled by default. Toggling the setting clears the interval
        history; setting the same value again keeps it. The pointwise
        transfer curve itself is never modified.
    */
    void SetAntialias(bool antialias);

    /** Clear the interval history only. The current drive and antialias
        mode are left unchanged. The next processed sample is output
        directly from the transfer curve, as on the first call after Init.
    */
    void Reset();

  private:
    /** The original pointwise transfer, unchanged. */
    float Pointwise(float in) const;

    float  drive_;
    float  pre_gain_;
    float  post_gain_;
    bool   antialias_;
    bool   hist_valid_;
    double hist_;
};
} // namespace daisysp
#endif
#endif
