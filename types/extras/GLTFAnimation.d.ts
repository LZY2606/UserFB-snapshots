export interface GLTFAnimationData {
    node: any;
    transform: any;
    interpolation: any;
    times: any;
    values: any;
}

export interface GLTFAnimationSample {
    node: any;
    transform: any;
    value: number[];
}

/**
 * A class for animation.
 * @see {@link https://github.com/oframe/ogl/blob/master/src/extras/GLTFAnimation.js | Source}
 */
export class GLTFAnimation {
    data: GLTFAnimationData[];
    elapsed: number;
    weight: number;
    loop: boolean;
    startTime: number;
    endTime: number;
    duration: number;

    constructor(data: GLTFAnimationData[], weight?: number);

    // Read-only absolute-time sampling (seconds); ignores elapsed, loop and
    // weight. Supports LINEAR and STEP channels only.
    samplePose(t: number): GLTFAnimationSample[];

    update(totalWeight?: number, isSet?: boolean): void;

    cubicSplineInterpolate(t: number, prevVal: any, prevTan: any, nextTan: any, nextVal: any): any;
}
