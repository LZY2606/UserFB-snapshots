import type { Transform } from '../core/Transform.js';
import type { Mat4 } from '../math/Mat4.js';
import type { GLTFAnimation } from './GLTFAnimation.js';

export interface GLTFMotionWarpContact {
    upper: Transform;
    lower: Transform;
    foot: Transform;
    offset: ArrayLike<number>;
    start: number;
    end: number;
}

export interface GLTFMotionWarpOptions {
    root: Transform;
    animation: GLTFAnimation;
    window: [number, number];
    target: ArrayLike<number>;
    contacts?: GLTFMotionWarpContact[];
}

export interface GLTFMotionWarpContactResult {
    locked: boolean;
    reachable: boolean;
    error: number;
}

export interface GLTFMotionWarpSample {
    rootWorld: Mat4;
    contacts: GLTFMotionWarpContactResult[];
}

/**
 * Motion warps a glTF step animation onto a target root pose while keeping
 * planted contact feet on their world anchors.
 * @see {@link https://github.com/oframe/ogl/blob/master/src/extras/GLTFMotionWarp.js | Source}
 */
export class GLTFMotionWarp {
    root: Transform;
    animation: GLTFAnimation;
    window: [number, number];
    target: Mat4;
    contacts: GLTFMotionWarpContact[];

    constructor(options: GLTFMotionWarpOptions);

    sample(t: number): GLTFMotionWarpSample;
}
