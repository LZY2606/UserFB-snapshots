import { Vec3 } from '../math/Vec3.js';
import { Quat } from '../math/Quat.js';

const tmpVec3A = /* @__PURE__ */ new Vec3();
const tmpVec3B = /* @__PURE__ */ new Vec3();
const tmpVec3C = /* @__PURE__ */ new Vec3();
const tmpVec3D = /* @__PURE__ */ new Vec3();

const tmpQuatA = /* @__PURE__ */ new Quat();
const tmpQuatB = /* @__PURE__ */ new Quat();
const tmpQuatC = /* @__PURE__ */ new Quat();
const tmpQuatD = /* @__PURE__ */ new Quat();

// Shared LINEAR/STEP sampler. Times are absolute seconds; values are held at
// the endpoints outside the key range. Rotations take the shortest arc.
// Returns an independent plain numeric array (length 3 or 4).
function sampleChannelValue({ transform, interpolation, times, values }, t) {
    const size = transform === 'quaternion' ? 4 : 3;
    const last = times.length - 1;

    let prevIndex = 0;
    let nextIndex = last;
    let alpha = 0;

    if (last > 0) {
        if (t <= times[0]) {
            prevIndex = 0;
            nextIndex = 0;
        } else if (t >= times[last]) {
            prevIndex = last;
            nextIndex = last;
        } else {
            let i = 0;
            while (i < last && times[i] <= t) i++;
            prevIndex = i - 1;
            nextIndex = i;
            alpha = (t - times[prevIndex]) / (times[nextIndex] - times[prevIndex]);
        }
    }

    if (interpolation === 'STEP') alpha = 0;

    const out = new Array(size);
    const prevOffset = prevIndex * size;
    if (alpha === 0 || prevIndex === nextIndex) {
        for (let i = 0; i < size; i++) out[i] = values[prevOffset + i];
        return out;
    }

    if (size === 4) {
        tmpQuatA.fromArray(values, prevOffset);
        tmpQuatB.fromArray(values, nextIndex * size);
        tmpQuatA.slerp(tmpQuatB, alpha);
        return tmpQuatA.toArray(out);
    }

    tmpVec3A.fromArray(values, prevOffset);
    tmpVec3B.fromArray(values, nextIndex * size);
    tmpVec3A.lerp(tmpVec3B, alpha);
    return tmpVec3A.toArray(out);
}

export class GLTFAnimation {
    constructor(data, weight = 1) {
        this.data = data;
        this.elapsed = 0;
        this.weight = weight;

        // Set to false to not apply modulo to elapsed against duration
        this.loop = true;

        // Find starting time as exports from blender (perhaps others too) don't always start from 0
        this.startTime = data.reduce((a, { times }) => Math.min(a, times[0]), Infinity);
        // Get largest final time in all channels to calculate duration
        this.endTime = data.reduce((a, { times }) => Math.max(a, times[times.length - 1]), 0);
        this.duration = this.endTime - this.startTime;
    }

    // Read-only absolute-time pose sampling. Ignores elapsed, loop and weight,
    // and never writes to the channel nodes or key data. Returns the channels
    // in their original order, each value being an independent numeric array.
    samplePose(t) {
        if (!Number.isFinite(t)) throw new Error(`GLTFAnimation.samplePose: t must be finite, got ${t}`);
        return this.data.map((channel) => {
            if (channel.interpolation !== 'LINEAR' && channel.interpolation !== 'STEP') {
                throw new Error(`GLTFAnimation.samplePose: unsupported interpolation "${channel.interpolation}"`);
            }
            return {
                node: channel.node,
                transform: channel.transform,
                value: sampleChannelValue(channel, t),
            };
        });
    }

    update(totalWeight = 1, isSet) {
        const weight = isSet ? 1 : this.weight / totalWeight;
        const elapsed = !this.duration
            ? 0
            : (this.loop ? this.elapsed % this.duration : Math.min(this.elapsed, this.duration - 0.001)) + this.startTime;

        this.data.forEach(({ node, transform, interpolation, times, values }) => {
            if (!this.duration) {
                let val = tmpVec3A;
                let size = 3;
                if (transform === 'quaternion') {
                    val = tmpQuatA;
                    size = 4;
                }
                val.fromArray(values, 0);
                if (size === 4) node[transform].slerp(val, weight);
                else node[transform].lerp(val, weight);
                return;
            }

            let prevVal;
            let size = 3;

            if (interpolation === 'CUBICSPLINE') {
                // Get index of two time values elapsed is between
                const prevIndex =
                    Math.max(
                        1,
                        times.findIndex((t) => t > elapsed)
                    ) - 1;
                const nextIndex = prevIndex + 1;

                // Get linear blend/alpha between the two
                const alpha = (elapsed - times[prevIndex]) / (times[nextIndex] - times[prevIndex]);

                prevVal = tmpVec3A;
                let prevTan = tmpVec3B;
                let nextTan = tmpVec3C;
                let nextVal = tmpVec3D;

                if (transform === 'quaternion') {
                    prevVal = tmpQuatA;
                    prevTan = tmpQuatB;
                    nextTan = tmpQuatC;
                    nextVal = tmpQuatD;
                    size = 4;
                }

                // Get the prev and next values from the indices
                prevVal.fromArray(values, prevIndex * size * 3 + size * 1);
                prevTan.fromArray(values, prevIndex * size * 3 + size * 2);
                nextTan.fromArray(values, nextIndex * size * 3 + size * 0);
                nextVal.fromArray(values, nextIndex * size * 3 + size * 1);

                // interpolate for final value
                prevVal = this.cubicSplineInterpolate(alpha, prevVal, prevTan, nextTan, nextVal);
                if (size === 4) prevVal.normalize();
            } else {
                // LINEAR and STEP share the read-only sampler
                const value = sampleChannelValue({ transform, interpolation, times, values }, elapsed);
                if (transform === 'quaternion') {
                    prevVal = tmpQuatA;
                    size = 4;
                } else {
                    prevVal = tmpVec3A;
                }
                prevVal.fromArray(value);
            }

            // interpolate between multiple possible animations
            if (size === 4) node[transform].slerp(prevVal, weight);
            else node[transform].lerp(prevVal, weight);
        });
    }

    cubicSplineInterpolate(t, prevVal, prevTan, nextTan, nextVal) {
        const t2 = t * t;
        const t3 = t2 * t;

        const s2 = 3 * t2 - 2 * t3;
        const s3 = t3 - t2;
        const s0 = 1 - s2;
        const s1 = s3 - t2 + t;

        for (let i = 0; i < prevVal.length; i++) {
            prevVal[i] = s0 * prevVal[i] + s1 * (1 - t) * prevTan[i] + s2 * nextVal[i] + s3 * t * nextTan[i];
        }

        return prevVal;
    }
}
