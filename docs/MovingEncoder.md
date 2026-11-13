# Moving Source Encoding

`AmbisonicMovingEncoder` encodes a mono signal emitted by a **moving** point
source into Ambisonics while keeping the full acoustic propagation chain of
every emitted sample. It is an offline component: it does not open any device
and works entirely on caller-provided buffers.

## Why a separate encoder

`AmbisonicEncoderDist` first delays the mono stream and then encodes the
delayed samples with the source's *current* direction. Repeatedly calling
`SetPosition()` and `Refresh()` therefore cannot express propagation: a sound
that has already left the source would be re-aimed by later positions, and the
distance-dependent arrival times that produce Doppler shift are not modelled.

`AmbisonicMovingEncoder` instead handles each mono sample independently:

1. The sample is encoded with the azimuth and elevation measured at its own
   emission instant.
2. The W-panning gain of its own emission distance is applied (the same
   interior/exterior rule as `AmbisonicEncoderDist`).
3. The resulting encoded vector is placed on the listener time line at its
   arrival time. A listener frame falling between two arrival times is a linear
   interpolation of the two **signed encoded vectors**.

Interpolating the encoded vectors (the product of the signed audio sample and
the directional coefficients), rather than interpolating angles first or
re-aiming old sound, is what preserves the emission direction of in-flight
sound and yields a physically correct pitch change.

## The two clocks

Two independent sample clocks are maintained. Both start at zero and are
continuous across `Process()` calls.

- The **emission clock** counts the mono samples handed to `Process()`. The
  emission sample with index *n* is associated with the *n*-th entry of the
  position array.
- The **listener clock** counts the B-Format frames produced. One listener
  frame is produced per accepted emission sample, so the two clocks advance at
  the same nominal rate but are offset by propagation.

The arrival time, in listener frames, of emission sample *n* is

```math
a(n) = n + \frac{\mathrm{distance}(n)\,\mathrm{sampleRate}}{344}
```

with the speed of sound fixed at 344 m/s and distances in metres. The first
listener frames can therefore be silence, emitted before any sound has reached
the listener. A virtual silent emission frame (index -1, using the first
frame's distance) precedes the first real sample, so listener frames earlier
than the first arrival are exactly zero. If an arrival time lands exactly on an
integer listener frame, that frame reproduces the encoded vector exactly.

For an integer listener frame *r*, the encoder finds the interval
`a(i) <= r <= a(i+1)` and outputs

```math
\mathbf{b}(r) = (1-f)\,\mathbf{v}(i) + f\,\mathbf{v}(i+1),
\qquad
f = \frac{r - a(i)}{a(i+1) - a(i)}
```

where `v(i)` is the encoded vector of emission *i*. Arrival times are stored in
double precision so that long runs do not lose the fractional position.

### Arrival time limits

The difference between consecutive arrival times must stay in
`[0.25, 4]` listener samples, including the pair crossing a `Process()`
boundary. Outside that range the trajectory is too fast to represent on this
time grid and the whole block is rejected.

## Distance gains (W-panning)

With `R` the configured room radius and `d` the emission distance, all default
order weights are one and:

- if `d < R`: the W channel gain is `1 - d/(2R)` and every other channel gain
  is `d/(2R)`;
- if `d >= R`: every channel gain is `R/(2d)`.

The gain is applied per emission sample, using that sample's own distance.

## Configuration and streaming

`Configure()` takes the Ambisonic order (0 to 3, 3D ACN/SN3D), the 2D/3D flag,
the sample rate, the maximum block size that will ever be passed to
`Process()`, and a strictly positive room radius. The bounded history that
holds in-flight sound is allocated once here; its size depends only on the
maximum distance (150 m), the sample rate, the order and the maximum block
size — never on the recording duration. `Process()` and `Finish()` perform no
heap allocation.

`Process()` takes a mono PCM block and one `PolarPosition<float>` per emission
sample (angles in radians, elevation in `[-pi/2, pi/2]`, distance in
`[0, 150]` metres) and writes the same number of B-Format listener frames. A
block containing a non-finite sample or position, an illegal elevation or
distance, or an out-of-range arrival difference is rejected as a whole: no
audio and no position is consumed and the object state is unchanged.

`Finish()` fixes the last position, appends a silent emission frame after the
last real sample and drains the propagation tail. It can be called repeatedly
with buffers of any capacity and returns the number of frames written, zero
once the tail is empty (and zero on every later call). For *N* real input
samples the total number of listener frames is
`ceil(N + lastDistance * sampleRate / 344)`; finishing an empty stream produces
zero frames. After `Finish()`, any non-empty `Process()` call is rejected.
`Reset()` clears both clocks, all in-flight sound and the finished state while
retaining the configuration.

## Code example

The example below generates every sample and position in code. A source moves
on a circle while slowly changing distance; the encoded output is drained to
completion, including the propagating tail.

```c++
#include "AmbisonicMovingEncoder.h"
#include "BFormat.h"
#include "Coordinates.h"

#include <cmath>
#include <vector>

using namespace spaudio;

int main()
{
    const unsigned sampleRate = 48000;
    const unsigned maxBlock = 512;
    const unsigned order = 1;          // 0 to 3, ACN/SN3D
    const float roomRadius = 5.f;      // metres, strictly positive
    const unsigned nSamples = 4000;    // total emission samples

    AmbisonicMovingEncoder encoder;
    if (!encoder.Configure(order, true, sampleRate, maxBlock, roomRadius))
        return 1;

    std::vector<float> pcm(maxBlock);
    std::vector<PolarPosition<float> > positions(maxBlock);
    std::vector<std::vector<float> > recording((order + 1) * (order + 1));

    unsigned emitted = 0;
    while (emitted < nSamples)
    {
        unsigned block = std::min(maxBlock, nSamples - emitted);

        // All input material is generated in code: a 440 Hz tone and a
        // trajectory whose direction rotates while its distance oscillates.
        for (unsigned i = 0; i < block; ++i)
        {
            unsigned n = emitted + i;
            pcm[i] = (float)std::sin(2.0 * M_PI * 440.0 * n / sampleRate);
            positions[i].azimuth = (float)(2.0 * M_PI) * n / 2000.f;
            positions[i].elevation = 0.25f * (float)std::sin(2.0 * M_PI * n / 900.0);
            positions[i].distance = 3.f + 1.5f * (float)std::sin(2.0 * M_PI * n / 5000.0);
        }

        BFormat blockBuffer;
        if (!blockBuffer.Configure(order, true, block))
            return 2;

        // A false return rejects the whole block without consuming anything.
        if (!encoder.Process(pcm.data(), block, positions.data(), &blockBuffer))
            return 3;

        for (unsigned ch = 0; ch < blockBuffer.GetChannelCount(); ++ch)
        {
            float* data = blockBuffer.GetChannelPointer(ch);
            recording[ch].insert(recording[ch].end(), data, data + block);
        }
        emitted += block;
    }

    // Drain the propagation tail in small fixed-capacity buffers until the
    // encoder reports that no more listener frames remain.
    const unsigned tailCapacity = 256;
    while (true)
    {
        BFormat tailBuffer;
        if (!tailBuffer.Configure(order, true, tailCapacity))
            return 4;

        unsigned written = encoder.Finish(&tailBuffer, tailCapacity);
        for (unsigned ch = 0; ch < tailBuffer.GetChannelCount(); ++ch)
        {
            float* data = tailBuffer.GetChannelPointer(ch);
            recording[ch].insert(recording[ch].end(), data, data + written);
        }
        if (written == 0)
            break;
    }

    // recording now holds, per B-Format channel, exactly
    // ceil(N + lastDistance * sampleRate / 344) listener frames.
    return 0;
}
```

## See also

- [Ambisonic Encoding](AmbisonicEncoding.md) for the static `AmbisonicEncoder`
  and the ACN/SN3D convention used by this component.
- `AmbisonicEncoderDist` for distance cues applied to a stationary direction.
