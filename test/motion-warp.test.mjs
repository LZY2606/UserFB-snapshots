// Motion warp verification. All reference math below is independent plain
// array/quaternion code; the production classes are only consumed, never
// re-derived. Runs in Node with no WebGL context.
import { Transform } from '../src/core/Transform.js';
import { Mat4 } from '../src/math/Mat4.js';
import { Quat } from '../src/math/Quat.js';
import { GLTFAnimation } from '../src/extras/GLTFAnimation.js';
import { GLTFMotionWarp } from '../src/extras/GLTFMotionWarp.js';
import { GLTFSkin } from '../src/extras/GLTFSkin.js';

const TOL = 2e-5;

// ---------------------------------------------------------------- reference math
const add3 = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const scl3 = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const len3 = (a) => Math.hypot(a[0], a[1], a[2]);
const norm3 = (a) => {
    const l = len3(a) || 1;
    return [a[0] / l, a[1] / l, a[2] / l];
};
const dist3 = (a, b) => len3(sub3(a, b));

const qAxis = (axis, ang) => {
    const n = norm3(axis);
    const h = ang / 2;
    const s = Math.sin(h);
    return [n[0] * s, n[1] * s, n[2] * s, Math.cos(h)];
};
const qRot = (q, p) => {
    const u = [q[0], q[1], q[2]];
    const t = scl3(cross3(u, p), 2);
    return add3(add3(p, scl3(t, q[3])), cross3(u, t));
};
const qSlerp = (a, b, t) => {
    let cos = dot3(a, b) + a[3] * b[3];
    let b2 = b;
    if (cos < 0) {
        cos = -cos;
        b2 = [-b[0], -b[1], -b[2], -b[3]];
    }
    if (1 - cos < 1e-10)
        return qNorm([a[0] * (1 - t) + b2[0] * t, a[1] * (1 - t) + b2[1] * t, a[2] * (1 - t) + b2[2] * t, a[3] * (1 - t) + b2[3] * t]);
    const om = Math.acos(cos);
    const so = Math.sin(om);
    const s0 = Math.sin((1 - t) * om) / so;
    const s1 = Math.sin(t * om) / so;
    return [a[0] * s0 + b2[0] * s1, a[1] * s0 + b2[1] * s1, a[2] * s0 + b2[2] * s1, a[3] * s0 + b2[3] * s1];
};
const qNorm = (q) => {
    const l = Math.hypot(q[0], q[1], q[2], q[3]);
    return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
};

// column-major mat4
const mCompose = (q, p) => {
    const [x, y, z, w] = q;
    const x2 = x + x, y2 = y + y, z2 = z + z;
    const xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    const wx = w * x2, wy = w * y2, wz = w * z2;
    return [
        1 - (yy + zz), xy + wz, xz - wy, 0,
        xy - wz, 1 - (xx + zz), yz + wx, 0,
        xz + wy, yz - wx, 1 - (xx + yy), 0,
        p[0], p[1], p[2], 1,
    ];
};
const mMul = (a, b) => {
    const out = new Array(16);
    for (let c = 0; c < 4; c++) {
        for (let r = 0; r < 4; r++) {
            out[c * 4 + r] =
                a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }
    }
    return out;
};
const mInvRigid = (m) => {
    const out = new Array(16);
    for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) out[c * 4 + r] = m[r * 4 + c];
    out[3] = out[7] = out[11] = 0;
    // t' = -R^T p; row r of R is m[r], m[4+r], m[8+r] in column-major.
    out[12] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
    out[13] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    out[14] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
    out[15] = 1;
    return out;
};
const mPoint = (m, p) => [
    m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
    m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
    m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
];
const mQuat = (m) => qNorm([m[6] - m[9], m[8] - m[2], m[1] - m[4], 1 + m[0] + m[5] + m[10]]);

// Independent SE(3) log/exp (translation coupled to rotation).
const se3Log = (m) => {
    const cos = Math.min(1, Math.max(-1, (m[0] + m[5] + m[10] - 1) / 2));
    const th = Math.acos(cos);
    const p = [m[12], m[13], m[14]];
    if (th < 1e-12) return { w: [0, 0, 0], v: p.slice(), theta: th };
    const axis = norm3([m[6] - m[9], m[8] - m[2], m[1] - m[4]]);
    const w = scl3(axis, th);
    const coef = (2 * (1 - cos) - th * Math.sin(th)) / (2 * th * th * (1 - cos));
    const sk = cross3(w, p);
    const sk2 = cross3(w, sk);
    return { w, v: add3(sub3(p, scl3(sk, 0.5)), scl3(sk2, coef)), theta: th };
};
const se3Exp = (w, v) => {
    const th = len3(w);
    if (th < 1e-12) return mCompose([0, 0, 0, 1], add3(v, scl3(cross3(w, v), 0.5)));
    const q = qAxis(w, th);
    const c1 = (1 - Math.cos(th)) / (th * th);
    const c2 = (th - Math.sin(th)) / (th * th * th);
    const sk = cross3(w, v);
    const sk2 = cross3(w, sk);
    const p = add3(add3(v, scl3(sk, c1)), scl3(sk2, c2));
    return mCompose(q, p);
};
const smooth01 = (u) => 3 * u * u - 2 * u * u * u;

const close = (check, a, b, msg, tol = TOL) => check(Math.abs(a - b) <= tol, `${msg}: ${a} vs ${b}`);
const closeV = (check, a, b, msg, tol = TOL) => a.forEach((x, i) => close(check, x, b[i], `${msg}[${i}]`, tol));
const closeM = (check, a, b, msg, tol = TOL) => a.forEach((x, i) => close(check, x, b[i], `${msg}[${i}]`, tol));

// ---------------------------------------------------------------- rig construction
function buildRig() {
    const scene = new Transform();
    scene.position.set(3, -2, 1.5);
    scene.quaternion.fromAxisAngle([0, 0, 1], 0.35);

    const root = new Transform();
    root.setParent(scene);
    root.position.set(0, 2, 0);

    // Two independent three-bone legs.
    const makeLeg = (side) => {
        const upper = new Transform();
        const lower = new Transform();
        const foot = new Transform();
        upper.setParent(root);
        lower.setParent(upper);
        foot.setParent(lower);
        upper.position.set(side * 0.4, 0, 0);
        lower.position.set(0, -1, 0);
        foot.position.set(0, -1, 0);
        // Slight construction-time knee bend so the pole is well defined.
        lower.quaternion.fromAxisAngle([1, 0, 0], 0.25);
        foot.quaternion.fromAxisAngle([0, 1, 0], side * 0.1);
        return { upper, lower, foot };
    };
    const legA = makeLeg(-1);
    const legB = makeLeg(1);

    // Unrelated branch that locking must never touch.
    const arm = new Transform();
    const hand = new Transform();
    arm.setParent(root);
    hand.setParent(arm);
    arm.position.set(0, 0.5, 0);
    hand.position.set(0.6, 0, 0);

    scene.updateMatrixWorld(true);
    const nodes = [scene, root, arm, hand, legA.upper, legA.lower, legA.foot, legB.upper, legB.lower, legB.foot];
    const rest = new Map();
    for (const n of nodes) {
        rest.set(n, {
            position: n.position.toArray([]),
            quaternion: n.quaternion.toArray([]),
        });
    }
    return { scene, root, legA, legB, arm, hand, nodes, rest };
}

// Reference forward kinematics: rest pose overridden by sampled channels.
function referenceWorlds(rig, animation, t) {
    const local = new Map();
    for (const [n, r] of rig.rest) local.set(n, { position: r.position.slice(), quaternion: r.quaternion.slice() });
    for (const { node, transform, value } of animation.samplePose(t)) {
        local.get(node)[transform] = value.slice();
    }
    const worlds = new Map();
    const walk = (node, parentWorld) => {
        const l = local.get(node);
        const w = mMul(parentWorld, mCompose(l.quaternion, l.position));
        worlds.set(node, w);
        for (const child of node.children) walk(child, w);
    };
    walk(rig.scene, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    return worlds;
}

function warpReference(rig, animation, window, target, t) {
    const [a, b] = window;
    const rawB = referenceWorlds(rig, animation, b);
    const C = mMul(target, mInvRigid(rawB.get(rig.root)));
    const log = se3Log(C);
    const u = Math.min(1, Math.max(0, (t - a) / (b - a)));
    const s = t <= a ? 0 : t >= b ? 1 : smooth01(u);
    const D = se3Exp(scl3(log.w, s), scl3(log.v, s));
    const raw = referenceWorlds(rig, animation, t);
    const warped = new Map();
    for (const [node, m] of raw) warped.set(node, mMul(D, m));
    return { warped, D, raw, s, log };
}

function makeAnimation(rig, { stepBone = null } = {}) {
    const q = (axis, ang) => new Quat().fromAxisAngle(axis, ang);
    const channels = [
        { node: rig.root, transform: 'position', interpolation: 'LINEAR', times: [0, 4], values: [0, 2, 0, 1.2, 2.6, 0.8] },
        {
            node: rig.root,
            transform: 'quaternion',
            interpolation: 'LINEAR',
            times: [0, 4],
            values: [0, 0, 0, 1, ...q([0, 1, 0], 0.6)],
        },
        {
            node: rig.legA.upper,
            transform: 'quaternion',
            interpolation: 'LINEAR',
            times: [0, 2, 4],
            values: [...q([1, 0, 0], 0.1), ...q([1, 0, 0], 0.5), ...q([0, 0, 1], 0.35)],
        },
        {
            node: rig.legA.lower,
            transform: 'quaternion',
            interpolation: 'LINEAR',
            times: [0, 4],
            values: [...q([1, 0, 0], 0.25), ...q([1, 0, 0], 0.9)],
        },
        {
            node: rig.legB.upper,
            transform: 'quaternion',
            interpolation: 'LINEAR',
            times: [0, 4],
            values: [...q([1, 0, 0], -0.1), ...q([0, 0, 1], -0.4)],
        },
        // STEP channel: foot B snaps at t=2.
        {
            node: rig.legB.foot,
            transform: 'quaternion',
            interpolation: 'STEP',
            times: [0, 2, 4],
            values: [...q([0, 1, 0], -0.1), ...q([0, 1, 0], 0.4), ...q([1, 0, 0], 0.3)],
        },
    ];
    if (stepBone) {
        channels.push({
            node: stepBone,
            transform: 'quaternion',
            interpolation: 'STEP',
            times: [0, 2],
            values: [0, 0, 0, 1, ...q([0, 0, 1], 0.5)],
        });
    }
    return new GLTFAnimation(channels);
}

const targetMatrix = () => {
    const R = qAxis([0.3, 0.8, 0.5], 0.9);
    return mCompose(R, [2, -1, 0.7]);
};

const snapshotAnimation = (animation) => animation.data.map((c) => ({ times: c.times.slice(), values: c.values.slice() }));

// A real GLTFSkin.prototype.updateUniforms driven without a GL context,
// following the same stub shape as task_support/baseline.mjs.
function makeSkinStub(joints) {
    const size = 8;
    const skin = Object.create(GLTFSkin.prototype);
    Object.assign(skin, {
        skeleton: { joints },
        boneMatrices: new Float32Array(size * size * 4),
        boneTexture: { needsUpdate: false },
        boneTextureSize: size,
        program: { uniforms: { boneTexture: {}, boneTextureSize: {} } },
    });
    return skin;
}

// ---------------------------------------------------------------- cases
export const cases = {
    // Screw motion: coupled SE(3) warp, window boundaries, STEP/LINEAR,
    // samplePose, snapshots and untouched inputs.
    async screw(check) {
        const rig = buildRig();
        const animation = makeAnimation(rig, { stepBone: rig.legB.lower });
        const window = [1, 3];
        const target = targetMatrix();
        const warp = new GLTFMotionWarp({ root: rig.root, animation, window, target, contacts: [] });

        // samplePose: channel order, independent arrays, read-only.
        const pose = animation.samplePose(1.5);
        check(pose.length === animation.data.length, 'samplePose returns one entry per channel');
        check(pose.every((p, i) => p.node === animation.data[i].node && p.transform === animation.data[i].transform), 'samplePose preserves channel order');
        pose[0].value[0] = 999;
        const pose2 = animation.samplePose(1.5);
        check(pose2[0].value[0] !== 999, 'samplePose values are independent arrays');
        check(animation.samplePose(0)[1].value[3] === 1, 'samplePose endpoint clamping (t before first key)');
        check(animation.samplePose(99)[0].value[0] === 1.2, 'samplePose holds the last key beyond range');

        const keysBefore = snapshotAnimation(animation);
        const elapsedBefore = animation.elapsed;
        animation.elapsed = 7.7;

        for (const t of [-1, 0.5, 1, 1.5, 2, 2.7, 3, 5]) {
            const ref = warpReference(rig, animation, window, target, t);
            const out = warp.sample(t);

            closeM(check, out.rootWorld, ref.warped.get(rig.root), `rootWorld screw at t=${t}`);
            closeM(check, rig.legA.foot.worldMatrix, ref.warped.get(rig.legA.foot), `child follows root at t=${t}`);
            closeM(check, rig.hand.worldMatrix, ref.warped.get(rig.hand), `unrelated branch warped at t=${t}`);

            // Snapshot ownership: fresh arrays per call.
            const again = warp.sample(t);
            check(again.rootWorld !== out.rootWorld, 'rootWorld is a fresh snapshot each call');
            check(again.contacts !== out.contacts, 'contacts array is a fresh snapshot each call');
            closeM(check, again.rootWorld, out.rootWorld, `repeat sample stable at t=${t}`);
        }

        // Boundary identities: s=0 reproduces the raw animation, s=1 lands
        // exactly on the target rigid matrix.
        const atA = warp.sample(1);
        const rawA = referenceWorlds(rig, animation, 1).get(rig.root);
        closeM(check, atA.rootWorld, rawA, 'at window start warp is identity');
        const atB = warp.sample(3);
        closeM(check, atB.rootWorld, target, 'at window end root equals target');
        const before = warp.sample(0.2);
        closeM(check, before.rootWorld, referenceWorlds(rig, animation, 0.2).get(rig.root), 'outside window (left) uses s=0');
        const after = warp.sample(4.5);
        // s=1 holds the full screw offset: W(t) = C * A(t), not a constant target.
        closeM(check, after.rootWorld, warpReference(rig, animation, window, target, 4.5).warped.get(rig.root), 'outside window (right) uses s=1');
        // And at the far right it stays pinned to C rather than reverting to raw.
        const rawRight = referenceWorlds(rig, animation, 4.5).get(rig.root);
        check(dist3([after.rootWorld[12], after.rootWorld[13], after.rootWorld[14]], [rawRight[12], rawRight[13], rawRight[14]]) > 1e-3, 'right side remains warped relative to the raw pose');

        // Non-commutativity: decoupled lerp(translation)+slerp(rotation) must
        // disagree with the coupled screw inside the window.
        const t = 2;
        const ref = warpReference(rig, animation, window, target, t);
        const rawB = referenceWorlds(rig, animation, 3).get(rig.root);
        const C = mMul(target, mInvRigid(rawB));
        const cq = mQuat(C);
        const cp = [C[12], C[13], C[14]];
        const u = (t - 1) / 2;
        const s = smooth01(u);
        const decoupled = mMul(mCompose(qSlerp([0, 0, 0, 1], cq, s), scl3(cp, s)), ref.raw.get(rig.root));
        const diff = len3(sub3([decoupled[12], decoupled[13], decoupled[14]], [ref.warped.get(rig.root)[12], ref.warped.get(rig.root)[13], ref.warped.get(rig.root)[14]]));
        check(diff > 1e-3, `coupled screw differs from decoupled lerp+slerp (diff=${diff})`);

        // STEP channel holds the left key.
        warp.sample(1.9);
        closeM(check, rig.legB.foot.worldMatrix, warpReference(rig, animation, window, target, 1.9).warped.get(rig.legB.foot), 'STEP key flows through the warp');
        const stepLeft = animation.samplePose(1.9).find((p) => p.node === rig.legB.foot).value;
        const stepKey = animation.data.find((c) => c.node === rig.legB.foot).values.slice(0, 4);
        closeV(check, stepLeft, stepKey, 'STEP holds previous key');
        const stepRight = animation.samplePose(2.1).find((p) => p.node === rig.legB.foot).value;
        check(stepRight[3] !== stepKey[3] || stepRight[1] !== stepKey[1], 'STEP advances after the key time');

        // Legacy update path still shares the sampler semantics.
        const lin = new GLTFAnimation([
            { node: rig.arm, transform: 'position', interpolation: 'LINEAR', times: [0, 2], values: [0, 0, 0, 4, 0, 0] },
        ]);
        lin.elapsed = 1;
        lin.update(1, true);
        close(check, rig.arm.position[0], 2, 'legacy LINEAR update matches sampler');
        closeV(check, lin.samplePose(1)[0].value, [2, 0, 0], 'samplePose matches legacy LINEAR');
        const step = new GLTFAnimation([
            { node: rig.hand, transform: 'position', interpolation: 'STEP', times: [0, 2], values: [0, 0, 0, 4, 0, 0] },
        ]);
        step.elapsed = 1.5;
        step.update(1, true);
        close(check, rig.hand.position[0], 0, 'legacy STEP update holds first key');
        // CUBICSPLINE keeps its own legacy path and is rejected by the sampler.
        const cubic = new GLTFAnimation([
            { node: rig.hand, transform: 'position', interpolation: 'CUBICSPLINE', times: [0, 1], values: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0] },
        ]);
        cubic.elapsed = 0.5;
        cubic.update(1, true);
        close(check, rig.hand.position[0], 1, 'legacy CUBICSPLINE path still works');
        let rejected = false;
        try {
            cubic.samplePose(0.5);
        } catch (e) {
            rejected = true;
        }
        check(rejected, 'samplePose rejects CUBICSPLINE');

        // Inputs must remain untouched.
        check(animation.elapsed === 7.7, 'animation.elapsed untouched by sampling');
        const keysAfter = snapshotAnimation(animation);
        check(keysBefore.every((c, i) => c.times.every((x, j) => x === keysAfter[i].times[j]) && c.values.every((x, j) => x === keysAfter[i].values[j])), 'key data untouched');

        // Validation surface.
        check(
            (() => {
                try {
                    new GLTFMotionWarp({ root: rig.root, animation, window: [2, 1], target });
                    return false;
                } catch (e) {
                    return true;
                }
            })(),
            'rejects invalid window'
        );
        check(
            (() => {
                try {
                    new GLTFMotionWarp({ root: rig.root, animation, window, target: target.slice(0, 15) });
                    return false;
                } catch (e) {
                    return true;
                }
            })(),
            'rejects invalid target'
        );
        let badT = false;
        try {
            warp.sample(NaN);
        } catch (e) {
            badT = true;
        }
        check(badT, 'sample rejects non-finite t');
    },

    // Contact locking: anchors, offset contact points, reachability,
    // degenerate poles, untouched branches, real skin palette deformation.
    async contacts(check) {
        const rig = buildRig();
        const animation = makeAnimation(rig);
        const window = [1, 4];
        const target = targetMatrix();
        const offsetA = [0.12, 0, -0.08];
        const offsetB = [0, 0, 0];

        const contactA = { upper: rig.legA.upper, lower: rig.legA.lower, foot: rig.legA.foot, offset: offsetA, start: 1.5, end: 2.5 };
        const contactA2 = { upper: rig.legA.upper, lower: rig.legA.lower, foot: rig.legA.foot, offset: offsetA, start: 3.2, end: 3.8 };
        const contactB = { upper: rig.legB.upper, lower: rig.legB.lower, foot: rig.legB.foot, offset: offsetB, start: 2.0, end: 3.0 };

        const warp = new GLTFMotionWarp({ root: rig.root, animation, window, target, contacts: [contactA, contactA2, contactB] });
        const unlocked = new GLTFMotionWarp({ root: rig.root, animation, window, target, contacts: [] });

        // Bind inverses from the construction pose for a real GLTFSkin palette.
        rig.scene.updateMatrixWorld(true);
        const joints = [rig.root, rig.arm, rig.hand, rig.legA.upper, rig.legA.lower, rig.legA.foot, rig.legB.upper, rig.legB.lower, rig.legB.foot];
        for (const j of joints) j.bindInverse = new Mat4(...j.worldMatrix).inverse();
        const bindSnapshot = joints.map((j) => j.bindInverse.slice());

        const anchorRef = (contact, t0) => {
            const ref = warpReference(rig, animation, window, target, t0);
            return mPoint(ref.warped.get(contact.foot), contact.offset);
        };
        const anchorA = anchorRef(contactA, 1.5);
        const anchorB = anchorRef(contactB, 2.0);
        check(anchorA.every(Number.isFinite) && anchorB.every(Number.isFinite), 'anchors rebuild directly from channel data');

        const l1 = len3(rig.legA.lower.position.toArray([]));
        const l2 = len3(rig.legA.foot.position.toArray([]));

        for (const t of [1.5, 1.7, 2.1, 2.4999]) {
            const out = warp.sample(t);
            const res = out.contacts[0];
            check(res.locked === true, `foot A locked at t=${t}`);
            check(res.reachable === true, `foot A reachable at t=${t}`);
            close(check, res.error, 0, `foot A zero residual at t=${t}`);

            // Capture the locked pose before any other controller touches it.
            const contactPoint = mPoint(rig.legA.foot.worldMatrix, offsetA);
            const lockedFootQuat = mQuat(rig.legA.foot.worldMatrix);
            const upperPos = [rig.legA.upper.worldMatrix[12], rig.legA.upper.worldMatrix[13], rig.legA.upper.worldMatrix[14]];
            const kneePos = [rig.legA.lower.worldMatrix[12], rig.legA.lower.worldMatrix[13], rig.legA.lower.worldMatrix[14]];
            const footOrigin = [rig.legA.foot.worldMatrix[12], rig.legA.foot.worldMatrix[13], rig.legA.foot.worldMatrix[14]];

            closeV(check, contactPoint, anchorA, `contact point fixed to anchor at t=${t}`);

            // Foot origin accounts for the rotating offset.
            const expectedOrigin = sub3(anchorA, qRot(lockedFootQuat, offsetA));
            closeV(check, footOrigin, expectedOrigin, `offset subtracted in foot rotation frame at t=${t}`);

            // Bone lengths preserved; upper position matches the un-locked pose.
            close(check, dist3(upperPos, kneePos), l1, `upper bone length at t=${t}`);
            close(check, dist3(kneePos, footOrigin), l2, `lower bone length at t=${t}`);
            const refWarped = warpReference(rig, animation, window, target, t).warped;
            closeV(check, upperPos, [refWarped.get(rig.legA.upper)[12], refWarped.get(rig.legA.upper)[13], refWarped.get(rig.legA.upper)[14]], `upper position untouched by IK at t=${t}`);

            // Foot world rotation equals the un-locked warped rotation.
            closeV(check, lockedFootQuat, mQuat(refWarped.get(rig.legA.foot)), `foot rotation un-locked at t=${t}`);
        }

        // Interval semantics: [start, end), multiple intervals per foot.
        // Result order follows the contacts array: A, A(second), B.
        check(warp.sample(2.5).contacts[0].locked === false, 'contact A released at end (half-open)');
        check(warp.sample(3.2).contacts[1].locked === true, 'second interval of same foot locks again');
        check(warp.sample(3.2).contacts[2].locked === false, 'contact B outside its interval reports unlocked');
        const outside = warp.sample(1.0);
        check(outside.contacts.every((c) => !c.locked && c.reachable && c.error === 0), 'unlocked contacts report reachable/error 0');
        const refOutside = warpReference(rig, animation, window, target, 1.0).warped;
        closeM(check, rig.legB.foot.worldMatrix, refOutside.get(rig.legB.foot), 'outside interval pose is the plain warp');

        // Zero-offset foot B locks its origin onto the anchor.
        const outB = warp.sample(2.4);
        check(outB.contacts[2].locked && outB.contacts[2].reachable, 'foot B locked and reachable');
        closeV(check, [rig.legB.foot.worldMatrix[12], rig.legB.foot.worldMatrix[13], rig.legB.foot.worldMatrix[14]], anchorB, 'zero-offset origin is the anchor');

        // Locking must not touch the unrelated branch.
        warp.sample(2.2);
        const armWhileLocked = Array.from(rig.arm.worldMatrix);
        const handWhileLocked = Array.from(rig.hand.worldMatrix);
        const freeSample = unlocked.sample(2.2);
        const refFree = warpReference(rig, animation, window, target, 2.2).warped;
        closeM(check, armWhileLocked, refFree.get(rig.arm), 'arm branch unaffected by foot locking');
        closeM(check, handWhileLocked, refFree.get(rig.hand), 'hand branch unaffected by foot locking');
        closeM(check, freeSample.rootWorld, refFree.get(rig.root), 'no-contact controller matches reference');

        // Real GLTFSkin.updateUniforms palette applied to weighted vertices.
        warp.sample(2.2);
        const skin = makeSkinStub(joints);
        skin.updateUniforms();
        check(skin.boneTexture.needsUpdate === true, 'real updateUniforms marks the texture dirty');
        check(skin.program.uniforms.boneTexture.value === skin.boneTexture, 'updateUniforms wires the texture uniform');
        const expectedPalette = joints.map((j) => mMul(j.worldMatrix, j.bindInverse));
        joints.forEach((j, i) => {
            const palette = Array.from(skin.boneMatrices.slice(i * 16, i * 16 + 16));
            closeM(check, palette, expectedPalette[i], `palette matrix joint ${i}`);
        });
        // Weighted vertex: 60% foot A, 40% lower A, including the w component.
        const vertex = [0.05, -0.9, -0.1, 1];
        const weights = [
            [joints.indexOf(rig.legA.foot), 0.6],
            [joints.indexOf(rig.legA.lower), 0.4],
        ];
        const applyMat = (m, p) => [0, 1, 2, 3].map((r) => m[0 * 4 + r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * p[3]);
        const skinned = [0, 0, 0, 0];
        const referenceSkinned = [0, 0, 0, 0];
        for (const [idx, w] of weights) {
            const palette = Array.from(skin.boneMatrices.slice(idx * 16, idx * 16 + 16));
            const fromPalette = applyMat(palette, vertex);
            const fromWorld = applyMat(mMul(joints[idx].worldMatrix, joints[idx].bindInverse), vertex);
            for (let r = 0; r < 4; r++) {
                skinned[r] += w * fromPalette[r];
                referenceSkinned[r] += w * fromWorld[r];
            }
        }
        closeV(check, skinned, referenceSkinned, 'palette-deformed vertex matches direct skinning', 1e-6);
        check(Math.abs(skinned[3] - 1) < 1e-6, 'skinned vertex homogeneous w is 1');
        // The deformed vertex follows the locked chain rather than the raw pose.
        const rawVertex = (() => {
            const ref = warpReference(rig, animation, window, target, 2.2).warped;
            const footRaw = applyMat(mMul(ref.get(rig.legA.foot), rig.legA.foot.bindInverse), vertex);
            const lowerRaw = applyMat(mMul(ref.get(rig.legA.lower), rig.legA.lower.bindInverse), vertex);
            return add3(scl3(footRaw.slice(0, 3), 0.6), scl3(lowerRaw.slice(0, 3), 0.4));
        })();
        check(dist3(skinned.slice(0, 3), rawVertex) > 1e-4, 'locked palette vertex differs from the un-locked warp vertex');

        // bindInverse never mutated.
        check(joints.every((j, i) => j.bindInverse.every((x, k) => x === bindSnapshot[i][k])), 'bindInverse untouched');

        // Unreachable chain: upper drifts away from a fixed anchor.
        const rig2 = buildRig();
        const anim2 = new GLTFAnimation([
            { node: rig2.root, transform: 'position', interpolation: 'LINEAR', times: [0, 4], values: [0, 2, 0, 6, 2, 0] },
            { node: rig2.root, transform: 'quaternion', interpolation: 'LINEAR', times: [0, 4], values: [0, 0, 0, 1, 0, 0, 0, 1] },
        ]);
        const warp2 = new GLTFMotionWarp({
            root: rig2.root,
            animation: anim2,
            window: [0, 4],
            target: mCompose([0, 0, 0, 1], [0, 2, 0]),
            contacts: [{ upper: rig2.legA.upper, lower: rig2.legA.lower, foot: rig2.legA.foot, offset: [0, 0, 0], start: 0.5, end: 2.5 }],
        });
        const r0 = warp2.sample(0.5);
        check(r0.contacts[0].locked && r0.contacts[0].reachable, 'reachable at the anchor instant');
        close(check, r0.contacts[0].error, 0, 'zero residual at anchor instant');
        const rFar = warp2.sample(2.4);
        check(rFar.contacts[0].locked && rFar.contacts[0].reachable === false, 'unreachable reported once the upper drifts away');
        check(rFar.contacts[0].error > 1e-4, `unreachable residual is positive (${rFar.contacts[0].error})`);
        const up = [rig2.legA.upper.worldMatrix[12], rig2.legA.upper.worldMatrix[13], rig2.legA.upper.worldMatrix[14]];
        const fp = [rig2.legA.foot.worldMatrix[12], rig2.legA.foot.worldMatrix[13], rig2.legA.foot.worldMatrix[14]];
        close(check, dist3(up, fp), 2, 'foot clamped to maximum reach (l1+l2)');
        const anchor2 = mPoint(warpReference(rig2, anim2, [0, 4], mCompose([0, 0, 0, 1], [0, 2, 0]), 0.5).warped.get(rig2.legA.foot), [0, 0, 0]);
        close(check, rFar.contacts[0].error, dist3(fp, anchor2), 'reported error equals the actual residual');

        // Collinear degenerate pole: straight leg, no NaN and deterministic.
        const rig3 = buildRig();
        rig3.legA.lower.quaternion.identity();
        rig3.scene.updateMatrixWorld(true);
        const anim3 = new GLTFAnimation([
            { node: rig3.root, transform: 'position', interpolation: 'LINEAR', times: [0, 2], values: [0, 2, 0, 0.5, 2.2, 0.3] },
        ]);
        const warp3 = new GLTFMotionWarp({
            root: rig3.root,
            animation: anim3,
            window: [0, 2],
            target: mCompose(qAxis([0, 1, 0], 0.4), [1, 2, 0]),
            contacts: [{ upper: rig3.legA.upper, lower: rig3.legA.lower, foot: rig3.legA.foot, offset: [0.05, 0, 0], start: 0.2, end: 1.8 }],
        });
        const d1 = warp3.sample(1.0);
        const d2 = warp3.sample(1.0);
        check(d1.contacts[0].locked && Number.isFinite(d1.contacts[0].error), 'collinear pose produces finite results');
        closeM(check, d2.rootWorld, d1.rootWorld, 'collinear fallback is deterministic');
        check(rig3.legA.lower.worldMatrix.every(Number.isFinite), 'no NaN in degenerate chain matrices');
    },

    // Out-of-order replay: any call order, repeated times and jumps across
    // intervals must reproduce the same snapshots with no feedback.
    async replay(check) {
        const rig = buildRig();
        const animation = makeAnimation(rig);
        const window = [1, 4];
        const target = targetMatrix();
        const contacts = [
            { upper: rig.legA.upper, lower: rig.legA.lower, foot: rig.legA.foot, offset: [0.1, 0, -0.1], start: 1.5, end: 2.2 },
            { upper: rig.legB.upper, lower: rig.legB.lower, foot: rig.legB.foot, offset: [0, 0, 0], start: 2.6, end: 3.4 },
            { upper: rig.legA.upper, lower: rig.legA.lower, foot: rig.legA.foot, offset: [0.1, 0, -0.1], start: 3.5, end: 3.9 },
        ];
        const warp = new GLTFMotionWarp({ root: rig.root, animation, window, target, contacts });
        const fresh = new GLTFMotionWarp({ root: rig.root, animation, window, target, contacts });

        const keysBefore = snapshotAnimation(animation);
        const elapsedBefore = animation.elapsed;

        const times = [3.7, 1.0, 2.0, 2.0, 0.2, 3.4, 1.5, 2.6, 4.5, 2.2, 3.0, 1.5, 2.9];
        const run = () => times.map((t) => ({ t, out: warp.sample(t) }));
        const first = run();
        const second = run();

        for (let i = 0; i < times.length; i++) {
            closeM(check, second[i].out.rootWorld, first[i].out.rootWorld, `replay rootWorld at t=${times[i]}`);
            check(second[i].out.contacts.length === first[i].out.contacts.length, `replay contact count at t=${times[i]}`);
            for (let k = 0; k < contacts.length; k++) {
                check(second[i].out.contacts[k].locked === first[i].out.contacts[k].locked, `replay locked flag t=${times[i]} c=${k}`);
                check(second[i].out.contacts[k].reachable === first[i].out.contacts[k].reachable, `replay reachable flag t=${times[i]} c=${k}`);
                close(check, second[i].out.contacts[k].error, first[i].out.contacts[k].error, `replay error t=${times[i]} c=${k}`);
            }
            // A brand-new controller gives the identical answer.
            const independent = fresh.sample(times[i]);
            closeM(check, independent.rootWorld, first[i].out.rootWorld, `fresh instance matches at t=${times[i]}`);
        }

        // Repeated time returns equal values but owned snapshots.
        const r1 = warp.sample(2.0);
        const r2 = warp.sample(2.0);
        check(r1.rootWorld !== r2.rootWorld && r1.contacts !== r2.contacts, 'repeated calls own distinct snapshots');
        closeM(check, r1.rootWorld, r2.rootWorld, 'repeated calls are value-identical');

        // Direct jump across several intervals lands each foot correctly.
        // Contacts order: A(1.5-2.2), B(2.6-3.4), A(3.5-3.9).
        const jumped = warp.sample(3.7);
        check(jumped.contacts[0].locked === false && jumped.contacts[1].locked === false && jumped.contacts[2].locked === true, 'jump re-locks the right intervals');
        const anchorA2 = mPoint(warpReference(rig, animation, window, target, 3.5).warped.get(rig.legA.foot), contacts[0].offset);
        closeV(check, mPoint(rig.legA.foot.worldMatrix, contacts[0].offset), anchorA2, 'jumped-into contact sits on its anchor');

        // Half-open boundaries.
        check(warp.sample(2.2).contacts[0].locked === false, 'end boundary releases (half-open)');
        check(warp.sample(1.5).contacts[0].locked === true, 'start boundary locks');
        check(warp.sample(3.4).contacts[1].locked === false, 'second foot end boundary releases');

        // Full node-state agreement with the mathematical reference at every
        // replayed time, including unlocked bones inside contact windows.
        for (const t of [...new Set(times)]) {
            const ref = warpReference(rig, animation, window, target, t);
            warp.sample(t);
            for (const node of [rig.root, rig.arm, rig.hand]) {
                closeM(check, node.worldMatrix, ref.warped.get(node), `reference node ${nodes_name(node, rig)} at t=${t}`);
            }
        }

        // No feedback into the animation inputs or bind matrices.
        check(animation.elapsed === elapsedBefore, 'elapsed unchanged after replay');
        const keysAfter = snapshotAnimation(animation);
        check(keysBefore.every((c, i) => c.times.every((x, j) => x === keysAfter[i].times[j]) && c.values.every((x, j) => x === keysAfter[i].values[j])), 'key data unchanged after replay');

        // samplePose stays pure under interleaved warping.
        const p1 = animation.samplePose(2.3);
        warp.sample(0.7);
        warp.sample(3.1);
        const p2 = animation.samplePose(2.3);
        check(p1.length === p2.length && p1.every((c, i) => c.value.every((x, j) => x === p2[i].value[j])), 'samplePose is stable across warped samples');
    },
};

function nodes_name(node, rig) {
    for (const [name, value] of Object.entries(rig)) {
        if (value === node) return name;
    }
    for (const [legName, leg] of [['legA', rig.legA], ['legB', rig.legB]]) {
        for (const [k, v] of Object.entries(leg)) if (v === node) return `${legName}.${k}`;
    }
    return 'node';
}
