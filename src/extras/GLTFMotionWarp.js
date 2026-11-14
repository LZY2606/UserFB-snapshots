import { Vec3 } from '../math/Vec3.js';
import { Quat } from '../math/Quat.js';
import { Mat4 } from '../math/Mat4.js';

const EPS = 1e-10;
const REACH_EPS = 1e-9;

// Temporary scratch objects (the class itself never keeps per-sample state).
const tmpMat4A = /* @__PURE__ */ new Mat4();
const tmpMat4B = /* @__PURE__ */ new Mat4();
const tmpMat4C = /* @__PURE__ */ new Mat4();
const tmpQuatA = /* @__PURE__ */ new Quat();
const tmpQuatB = /* @__PURE__ */ new Quat();
const tmpQuatC = /* @__PURE__ */ new Quat();

const WORLD_UP = /* @__PURE__ */ new Vec3(0, 1, 0);

function allFinite(a) {
    for (let i = 0; i < a.length; i++) if (!Number.isFinite(a[i])) return false;
    return true;
}

// Orthonormal, proper (det = +1) 3x3 rotation in the upper-left of a
// column-major rigid mat4.
function isRigidRotation(m) {
    const cols = [
        [m[0], m[1], m[2]],
        [m[4], m[5], m[6]],
        [m[8], m[9], m[10]],
    ];
    for (const c of cols) if (Math.abs(dot3(c, c) - 1) > 1e-6) return false;
    if (Math.abs(dot3(cols[0], cols[1])) > 1e-6 || Math.abs(dot3(cols[0], cols[2])) > 1e-6 || Math.abs(dot3(cols[1], cols[2])) > 1e-6) return false;
    const det = dot3(cols[0], crossOut([0, 0, 0], cols[1], cols[2]));
    return Math.abs(det - 1) < 1e-6;
}

// Inverse of a rigid (rotation + translation) matrix.
function rigidInverse(out, m) {
    const m0 = m[0], m1 = m[1], m2 = m[2];
    const m4 = m[4], m5 = m[5], m6 = m[6];
    const m8 = m[8], m9 = m[9], m10 = m[10];
    const x = m[12], y = m[13], z = m[14];

    out[0] = m0;
    out[1] = m4;
    out[2] = m8;
    out[3] = 0;
    out[4] = m1;
    out[5] = m5;
    out[6] = m9;
    out[7] = 0;
    out[8] = m2;
    out[9] = m6;
    out[10] = m10;
    out[11] = 0;
    out[12] = -(m0 * x + m1 * y + m2 * z);
    out[13] = -(m4 * x + m5 * y + m6 * z);
    out[14] = -(m8 * x + m9 * y + m10 * z);
    out[15] = 1;
    return out;
}

function mat4Translation(out, m) {
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return out;
}

// p' = R * p + t for a rigid column-major mat4
function transformPoint(out, m, p) {
    const x = p[0], y = p[1], z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    return out;
}

function crossOut(out, a, b) {
    const ax = a[0], ay = a[1], az = a[2];
    const bx = b[0], by = b[1], bz = b[2];
    out[0] = ay * bz - az * by;
    out[1] = az * bx - ax * bz;
    out[2] = ax * by - ay * bx;
    return out;
}

function dot3(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// SE(3) logarithm of a rigid matrix. Returns {w: [x,y,z], v: [x,y,z]} where
// the translation term is coupled to the rotation (V^-1 applied to p).
function logRigid(m) {
    const trace = m[0] + m[5] + m[10];
    const cosTheta = Math.min(1, Math.max(-1, (trace - 1) / 2));
    const theta = Math.acos(cosTheta);

    const p = [m[12], m[13], m[14]];

    if (theta < EPS) {
        // With rotation negligible, V^-1 ~= I and v reduces to the translation.
        return { w: [0, 0, 0], v: p.slice(), theta };
    }

    const sin = Math.sin(theta);
    const axisFactor = 1 / (2 * sin);
    const axis = [
        (m[6] - m[9]) * axisFactor,
        (m[8] - m[2]) * axisFactor,
        (m[1] - m[4]) * axisFactor,
    ];
    const w = [axis[0] * theta, axis[1] * theta, axis[2] * theta];

    // V^-1 = I - 1/2 [w] + coeff [w]^2
    const coeff = (2 * (1 - cosTheta) - theta * sin) / (2 * theta * theta * (1 - cosTheta));
    const skewP = crossOut([0, 0, 0], w, p);
    const skew2P = crossOut([0, 0, 0], w, skewP);
    const v = [
        p[0] - 0.5 * skewP[0] + coeff * skew2P[0],
        p[1] - 0.5 * skewP[1] + coeff * skew2P[1],
        p[2] - 0.5 * skewP[2] + coeff * skew2P[2],
    ];

    return { w, v, theta };
}

// SE(3) exponential of a screw (w, v). Writes a rigid mat4 into out.
function expRigid(out, w, v) {
    const theta = Math.hypot(w[0], w[1], w[2]);

    if (theta < EPS) {
        tmpQuatA.identity();
        out.fromQuaternion(tmpQuatA);
        // p ~= v + 1/2 w x v
        const half = crossOut([0, 0, 0], w, v);
        out[12] = v[0] + 0.5 * half[0];
        out[13] = v[1] + 0.5 * half[1];
        out[14] = v[2] + 0.5 * half[2];
        return out;
    }

    const axis = [w[0] / theta, w[1] / theta, w[2] / theta];
    tmpQuatA.fromAxisAngle(axis, theta);
    out.fromQuaternion(tmpQuatA);

    // V = I + ((1-cos)/theta^2) [w] + ((theta-sin)/theta^3) [w]^2
    const c1 = (1 - Math.cos(theta)) / (theta * theta);
    const c2 = (theta - Math.sin(theta)) / (theta * theta * theta);
    const skewV = crossOut([0, 0, 0], w, v);
    const skew2V = crossOut([0, 0, 0], w, skewV);
    out[12] = v[0] + c1 * skewV[0] + c2 * skew2V[0];
    out[13] = v[1] + c1 * skewV[1] + c2 * skew2V[1];
    out[14] = v[2] + c1 * skewV[2] + c2 * skew2V[2];
    return out;
}

// Shortest-rotation quaternion (plain [x,y,z,w]) rotating unit vector u
// onto unit vector v. Returned as a plain array to avoid a Transform quaternion's
// per-component euler feedback; callers commit it once via fromArray/copy.
function quatBetween(u, v) {
    const d = Math.min(1, Math.max(-1, dot3(u, v)));
    if (d > 1 - 1e-12) return [0, 0, 0, 1];
    if (d < -1 + 1e-12) {
        // 180 degrees: pick any deterministic perpendicular axis.
        const axis = crossOut([0, 0, 0], u, WORLD_UP);
        if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-12) {
            crossOut(axis, u, [1, 0, 0]);
        }
        const n = Math.hypot(axis[0], axis[1], axis[2]);
        // sin(pi/2) = 1, so the quaternion is (axis, 0).
        return [axis[0] / n, axis[1] / n, axis[2] / n, 0];
    }
    const cross = crossOut([0, 0, 0], u, v);
    const q = [cross[0], cross[1], cross[2], 1 + d];
    const l = Math.hypot(q[0], q[1], q[2], q[3]);
    return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
}

function worldQuat(out, node) {
    node.worldMatrix.getRotation(out);
    return out;
}

// Replace a node's world rotation while keeping its local translation, then
// refresh the world matrix chain below it.
function setWorldRotation(node, qWorld, sceneRoot) {
    if (node.parent) {
        worldQuat(tmpQuatA, node.parent);
        tmpQuatB.inverse(tmpQuatA);
        tmpQuatC.multiply(tmpQuatB, qWorld);
        node.quaternion.copy(tmpQuatC);
    } else {
        node.quaternion.copy(qWorld);
    }
    sceneRoot.updateMatrixWorld(true);
}

export class GLTFMotionWarp {
    constructor(options = {}) {
        const { root, animation, window, target, contacts = [] } = options;
        if (!Array.isArray(window)) throw new Error('GLTFMotionWarp: window [a, b] is required');
        const [a, b] = window;
        if (!root || typeof root.updateMatrixWorld !== 'function') throw new Error('GLTFMotionWarp: root Transform required');
        if (!animation || !Array.isArray(animation.data)) throw new Error('GLTFMotionWarp: GLTFAnimation required');
        if (![a, b].every(Number.isFinite) || !(a < b)) throw new Error('GLTFMotionWarp: window must be [a, b] with finite a < b');
        if (!target || target.length !== 16 || !allFinite(target)) throw new Error('GLTFMotionWarp: target must be a length-16 finite matrix');
        if (target[15] !== 1 || target[3] !== 0 || target[7] !== 0 || target[11] !== 0) {
            throw new Error('GLTFMotionWarp: target must be a rigid body matrix');
        }
        if (!isRigidRotation(target)) throw new Error('GLTFMotionWarp: target rotation must be orthonormal with determinant 1');

        this.root = root;
        this.animation = animation;
        this.window = [a, b];
        this.target = new Mat4().fromArray(target);
        this.contacts = [];

        // Only LINEAR/STEP channels are supported; animated scale breaks the
        // rigid-bone assumptions.
        for (const channel of animation.data) {
            if (channel.interpolation !== 'LINEAR' && channel.interpolation !== 'STEP') {
                throw new Error(`GLTFMotionWarp: unsupported interpolation "${channel.interpolation}"`);
            }
            if (channel.transform === 'scale') throw new Error('GLTFMotionWarp: animated scale is not supported');
        }

        // Top of the (fixed rigid) ancestor chain; world matrices are rebuilt
        // from here on every sample.
        let sceneRoot = root;
        while (sceneRoot.parent) sceneRoot = sceneRoot.parent;
        this.sceneRoot = sceneRoot;

        // Snapshot the construction-time local TRS of every node under root so
        // properties without channels keep their original local pose.
        this.restPose = new Map();
        const snapshot = (node) => {
            if (Math.abs(node.scale[0] - 1) > 1e-9 || Math.abs(node.scale[1] - 1) > 1e-9 || Math.abs(node.scale[2] - 1) > 1e-9) {
                throw new Error('GLTFMotionWarp: static node scale must be 1');
            }
            this.restPose.set(node, {
                position: node.position.toArray(new Array(3)),
                quaternion: node.quaternion.toArray(new Array(4)),
                scale: node.scale.toArray(new Array(3)),
            });
        };
        root.traverse(snapshot);
        // Animated nodes outside root's subtree are still restored so no node
        // state leaks from one sample into the next.
        for (const channel of animation.data) {
            if (!this.restPose.has(channel.node)) snapshot(channel.node);
        }

        // Validate and normalise the contact chains.
        for (const c of contacts) {
            if (!c || !c.upper || !c.lower || !c.foot) throw new Error('GLTFMotionWarp: contact requires upper, lower and foot nodes');
            const { upper, lower, foot } = c;
            if (lower.parent !== upper || foot.parent !== lower) {
                throw new Error('GLTFMotionWarp: contact nodes must be a consecutive parent-child bone chain');
            }
            if (!this.isDescendant(root, upper)) throw new Error('GLTFMotionWarp: root must be an ancestor of every contact chain');
            if (!c.offset || c.offset.length !== 3 || !allFinite(c.offset)) throw new Error('GLTFMotionWarp: contact offset must be a finite vec3');
            if (![c.start, c.end].every(Number.isFinite) || !(c.start < c.end)) {
                throw new Error('GLTFMotionWarp: contact interval must have finite start < end');
            }

            const l1 = lower.position.len();
            const l2 = foot.position.len();
            if (!(l1 > 0) || !(l2 > 0)) throw new Error('GLTFMotionWarp: contact bone lengths must be non-zero');

            // Chains belonging to different feet must not share bone nodes.
            for (const existing of this.contacts) {
                if (existing.foot === foot) continue;
                if ([existing.upper, existing.lower, existing.foot].some((node) => node === upper || node === lower || node === foot)) {
                    throw new Error('GLTFMotionWarp: chains of different feet must not share bones');
                }
            }
            for (const existing of this.contacts) {
                if (existing.foot === foot && c.start < existing.end && existing.start < c.end) {
                    throw new Error('GLTFMotionWarp: contact intervals of the same foot must not overlap');
                }
            }

            this.contacts.push({
                upper,
                lower,
                foot,
                offset: new Vec3().fromArray(c.offset),
                start: c.start,
                end: c.end,
                lengthUpper: l1,
                lengthLower: l2,
                anchor: new Vec3(),
            });
        }

        // Probe the warped pose at b to build C = target * inverse(A(b)), and
        // at each contact start to fix its world anchor. The nodes are restored
        // to their construction pose afterwards.
        this.applyRawPose(b);
        const Ab = new Mat4().copy(root.worldMatrix);
        const C = tmpMat4A.multiply(this.target, rigidInverse(tmpMat4B, Ab));
        this.screw = logRigid(C);
        if (this.screw.theta >= Math.PI - 1e-9) {
            throw new Error('GLTFMotionWarp: relative rotation must be strictly less than pi');
        }

        for (const contact of this.contacts) {
            this.applyWarpedPose(contact.start);
            transformPoint(contact.anchor, contact.foot.worldMatrix, contact.offset);
        }

        this.restoreRestPose();
        this.sceneRoot.updateMatrixWorld(true);
    }

    isDescendant(ancestor, node) {
        let current = node;
        while (current) {
            if (current === ancestor) return true;
            current = current.parent;
        }
        return false;
    }

    restoreRestPose() {
        for (const [node, rest] of this.restPose) {
            node.position.fromArray(rest.position);
            node.quaternion.fromArray(rest.quaternion);
            node.scale.fromArray(rest.scale);
        }
    }

    // Sample the raw animation onto the nodes and refresh world matrices.
    // Every sample starts from the construction pose, never from a previous
    // sample's output.
    applyRawPose(t) {
        this.restoreRestPose();
        const pose = this.animation.samplePose(t);
        for (const { node, transform, value } of pose) {
            node[transform].fromArray(value);
        }
        this.sceneRoot.updateMatrixWorld(true);
    }

    // Raw animation followed by the world-space left-multiply warp.
    applyWarpedPose(t) {
        this.applyRawPose(t);

        const [a, b] = this.window;
        let s;
        if (t <= a) s = 0;
        else if (t >= b) s = 1;
        else {
            const u = (t - a) / (b - a);
            s = 3 * u * u - 2 * u * u * u;
        }

        const { w, v } = this.screw;
        expRigid(tmpMat4A, [w[0] * s, w[1] * s, w[2] * s], [v[0] * s, v[1] * s, v[2] * s]);
        // W(t) = D(s) * A(t)
        tmpMat4B.multiply(tmpMat4A, this.root.worldMatrix);

        // Convert the warped world pose back into root's parent frame.
        rigidInverse(tmpMat4C, this.root.parent ? this.root.parent.worldMatrix : this.identityMatrix());
        const rootLocal = tmpMat4A.multiply(tmpMat4C, tmpMat4B);
        rootLocal.getTranslation(this.root.position);
        // Extract into a scratch quaternion first: writing a Transform's linked
        // quaternion component-by-component would fire its euler feedback on
        // partial data. copy() commits the full rotation in a single change.
        rootLocal.getRotation(tmpQuatA);
        this.root.quaternion.copy(tmpQuatA);

        this.sceneRoot.updateMatrixWorld(true);
    }

    identityMatrix() {
        if (!this._identity) this._identity = new Mat4();
        return this._identity;
    }

    // Two-bone IK for one locked contact. Only rotations are written; the
    // bones' local translations are preserved.
    solveContact(contact) {
        const { upper, lower, foot, offset, lengthUpper: restL1, lengthLower: restL2, anchor } = contact;

        // IK only writes rotations, so each segment's length is the norm of the
        // bone's current local translation (animated translations included).
        const l1 = lower.position.len() || restL1;
        const l2 = foot.position.len() || restL2;

        const upperPos = mat4Translation(new Vec3(), upper.worldMatrix);
        // Un-locked foot world rotation is the target rotation for the foot.
        const footWorldRot = worldQuat(new Quat(), foot);
        // Animated knee position polarises the bend plane.
        const animatedKnee = mat4Translation(new Vec3(), lower.worldMatrix);

        // Foot origin target: subtract the rotating offset from the anchor.
        const offsetWorld = new Vec3().copy(offset).applyQuaternion(footWorldRot);
        const footTarget = new Vec3().copy(anchor).sub(offsetWorld);

        const toFoot = new Vec3().copy(footTarget).sub(upperPos);
        let d = toFoot.len();
        const dMax = l1 + l2;
        const dMin = Math.abs(l1 - l2);
        let reachable = true;
        if (d > dMax) {
            d = dMax;
            reachable = false;
        } else if (d < dMin) {
            d = dMin;
            reachable = false;
        }

        // Direction from the upper toward the (possibly clamped) foot target,
        // with a deterministic vertical fallback for collinear degeneracies.
        let dir;
        if (toFoot.len() >= EPS) {
            dir = new Vec3().copy(toFoot).normalize();
        } else {
            reachable = false;
            d = Math.max(d, EPS);
            dir = new Vec3(0, -1, 0);
        }

        // Bend plane normal from the animated pose, with a deterministic
        // vertical fallback when knee, upper and target are collinear.
        const toKnee = new Vec3().copy(animatedKnee).sub(upperPos);
        let pole = null;
        if (toKnee.len() > EPS) {
            toKnee.normalize();
            pole = new Vec3().copy(toKnee).sub(new Vec3().copy(dir).scale(dot3(toKnee, dir)));
            if (pole.squaredLen() < 1e-14) pole = null;
            else pole.normalize();
        }
        if (!pole) {
            pole = new Vec3();
            crossOut(pole, WORLD_UP, dir);
            if (pole.squaredLen() < 1e-14) crossOut(pole, [0, 0, 1], dir);
            if (pole.squaredLen() < 1e-14) crossOut(pole, [1, 0, 0], dir);
            pole.normalize();
        }

        const cosKnee = Math.min(1, Math.max(-1, (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d)));
        const along = l1 * cosKnee;
        const height = l1 * Math.sqrt(Math.max(0, 1 - cosKnee * cosKnee));

        const kneeTarget = new Vec3()
            .copy(upperPos)
            .add(new Vec3().copy(dir).scale(along))
            .add(new Vec3().copy(pole).scale(height));

        // Rotate the upper bone so its child aims at the solved knee.
        const upperRot = worldQuat(new Quat(), upper);
        const upperBoneDir = new Vec3().copy(lower.position).normalize().applyQuaternion(upperRot);
        const upperKneeDir = new Vec3().copy(kneeTarget).sub(mat4Translation(new Vec3(), upper.worldMatrix)).normalize();
        setWorldRotation(upper, new Quat().multiply(quatBetween(upperBoneDir, upperKneeDir), upperRot), this.sceneRoot);

        // Rotate the lower bone so the foot origin lands on its target.
        const lowerRot = worldQuat(new Quat(), lower);
        const lowerBoneDir = new Vec3().copy(foot.position).normalize().applyQuaternion(lowerRot);
        const lowerFootDir = new Vec3().copy(footTarget).sub(mat4Translation(new Vec3(), lower.worldMatrix)).normalize();
        setWorldRotation(lower, new Quat().multiply(quatBetween(lowerBoneDir, lowerFootDir), lowerRot), this.sceneRoot);

        // The foot keeps its un-locked world rotation.
        setWorldRotation(foot, footWorldRot, this.sceneRoot);

        // Residual: distance between the solved contact point and its anchor.
        const contactPoint = transformPoint(new Vec3(), foot.worldMatrix, offset);
        const error = contactPoint.distance(anchor);
        if (reachable && error <= REACH_EPS) return { locked: true, reachable: true, error: 0 };
        return { locked: true, reachable, error };
    }

    sample(t) {
        if (!Number.isFinite(t)) throw new Error(`GLTFMotionWarp.sample: t must be finite, got ${t}`);

        this.applyWarpedPose(t);

        const contactResults = this.contacts.map((contact) => {
            if (t >= contact.start && t < contact.end) return this.solveContact(contact);
            return { locked: false, reachable: true, error: 0 };
        });

        return {
            rootWorld: new Mat4().copy(this.root.worldMatrix),
            contacts: contactResults,
        };
    }
}
