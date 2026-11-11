/*
 * Programmatic skeleton fixtures for the connected-rig regression.
 *
 * Every fixture builds its chains from the real src classes, drives the real
 * solver, and returns a plain data object.  No assertions live here; run.js
 * derives all checks from the resulting bone endpoints and the untouched
 * inputs (template chains, target vectors).
 *
 * Geometry is intentionally small and exact: bones are 1 or 2 units, targets
 * are chosen so that the reachable cases have a closed-form answer, and the
 * hinge fixtures use asymmetric limits (15 deg clockwise / 55 deg
 * anticlockwise) so a sign or direction mix-up cannot pass by accident.
 *
 * The regression locks in the behaviour of the shipped solver, not an ideal
 * solver.  In this source version M3.createRotationMatrix leaves its X/Y basis
 * equal to the reference direction (V3.cross is a pure function, so the
 * cross-products are dropped) and V3.projectOnPlane returns a closure without
 * applying it.  That is observable behaviour of the production code, so the
 * connected local-constraint expectations below are stated in the terms the
 * solver actually enforces (a directed-angle bound about the transformed
 * axis), not in terms of perfect frame math.
 */

import { Structure3D } from '../../src/core/Structure3D.js';
import { Chain3D } from '../../src/core/Chain3D.js';
import { Bone3D } from '../../src/core/Bone3D.js';
import { V3 } from '../../src/math/V3.js';

export const DEG = 180 / Math.PI;

// Solver tuning shared by every fixture.  The defaults (solveDistanceThreshold
// 1.0, minIterationChange 0.01, maxIteration 20) are too coarse for a
// centimetre-scale regression, so we tighten them once here.  Tightening the
// knobs changes how many FABRIK passes run, never what the solver is allowed to
// do; the original source and all three mutants receive the same tuning.
export function tune( chain ) {

    chain.setSolveDistanceThreshold( 1e-9 );
    chain.setMinIterationChange( 1e-9 );
    chain.setMaxIterationAttempts( 100 );
    return chain;

}

export function snapshotChain( chain ) {

    return chain.bones.map( ( b ) => ( {
        sx: b.start.x, sy: b.start.y, sz: b.start.z,
        ex: b.end.x, ey: b.end.y, ez: b.end.z,
    } ) );

}

export function snapshotV3( v ) {

    return { x: v.x, y: v.y, z: v.z };

}

function hostChain( length ) {

    const host = new Chain3D();
    host.addBone( new Bone3D( new V3(), new V3( 0, 0, length ) ) );
    return tune( host );

}

function twoBoneHost() {

    const host = new Chain3D();
    host.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    host.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    return tune( host );

}

function rotorTemplate( rotorDegs ) {

    const tpl = new Chain3D();
    tpl.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    tpl.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    tpl.setRotorBaseboneConstraint( 'local', new V3( 0, 0, 1 ), rotorDegs );
    return tune( tpl );

}

function hingeTemplate( cwDegs, acwDegs ) {

    const tpl = new Chain3D();
    tpl.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    tpl.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    tpl.setHingeBaseboneConstraint( 'local', new V3( 0, 1, 0 ), cwDegs, acwDegs, new V3( 0, 0, 1 ) );
    return tune( tpl );

}

function singleBone( length ) {

    const c = new Chain3D();
    c.addBone( new Bone3D( new V3(), new V3( 0, 0, length ) ) );
    return tune( c );

}

// Baseline parity: the same skeleton as baseline/current.js, extended with
// geometry assertions.  Host rotates from +X to +Z, the two-bone child must
// follow the connection point and still span its full length.
export function baselineHostRotation() {

    const host = tune( hostChain( 2 ) );
    const template = new Chain3D();
    template.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    template.addConsecutiveRotorConstrainedBone( new V3( 0, 0, 1 ), 1, 0 );
    template.setRotorBaseboneConstraint( 'local', new V3( 0, 0, 1 ), 0 );
    tune( template );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 4, 0, 0 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );

    structure.update();
    const child = structure.getChain( 1 );
    const phaseX = {
        hostEnd: host.bones[ 0 ].end.clone(),
        childBase: child.bones[ 0 ].start.clone(),
        childEnd: child.bones[ 1 ].end.clone(),
    };

    hostTarget.set( 0, 0, 2 );
    childTarget.set( 0, 0, 4 );
    // Snapshot the targets the solver is about to see; they must come back
    // unchanged after the update.
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );
    structure.update();
    const phaseZ = {
        hostEnd: host.bones[ 0 ].end.clone(),
        childBase: child.bones[ 0 ].start.clone(),
        childEnd: child.bones[ 1 ].end.clone(),
    };

    return {
        kind: 'structure',
        host, child, template,
        templateBefore, hostTargetBefore, childTargetBefore,
        phaseX, phaseZ,
        hostTarget, childTarget,
        expectedChildBaseX: new V3( 2, 0, 0 ),
        expectedChildEndX: new V3( 4, 0, 0 ),
        expectedChildBaseZ: new V3( 0, 0, 2 ),
        expectedChildEndZ: new V3( 0, 0, 4 ),
        childLengths: [ 1, 1 ],
        localConstraint: { axis: new V3( 0, 0, 1 ), rotorDegs: 0, hostPhaseX: new V3( 1, 0, 0 ) },
    };

}

// Direct Chain3D solve: global rotor basebone cone (45 deg) plus a free
// second bone; the target is off-axis and reachable, so the effector must land
// on it exactly.
export function directRotorReach() {

    const chain = new Chain3D();
    chain.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    chain.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    chain.setRotorBaseboneConstraint( 'global', new V3( 0, 0, 1 ), 45 );
    tune( chain );

    const target = new V3( 0.9, 0, 1.6 );
    const targetBefore = snapshotV3( target );
    chain.solveForTarget( target );

    return {
        kind: 'direct',
        chain, target, targetBefore,
        base: new V3( 0, 0, 1 ),
        coneAxis: new V3( 0, 0, 1 ), coneDegs: 45,
        expectedEnd: target.clone(),
        lengths: [ 1, 1 ],
    };

}

// Direct Chain3D solve with a single bone and a global hinge basebone.  A
// target 80 deg off the reference axis forces the basebone onto the
// asymmetrical limits: +55 deg anticlockwise, then -15 deg clockwise.
export function directGlobalHingeLimits() {

    const chain = new Chain3D();
    chain.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    chain.setHingeBaseboneConstraint( 'global', new V3( 0, 1, 0 ), 15, 55, new V3( 0, 0, 1 ) );
    tune( chain );

    const acwTarget = new V3( Math.sin( 80 * Math.PI / 180 ) * 0.7, 0, Math.cos( 80 * Math.PI / 180 ) * 0.7 );
    const cwTarget = new V3( - Math.sin( 80 * Math.PI / 180 ) * 0.7, 0, Math.cos( 80 * Math.PI / 180 ) * 0.7 );
    chain.solveForTarget( acwTarget );
    const acwDir = chain.bones[ 0 ].getDirectionUV();
    const acwEnd = chain.bones[ 0 ].end.clone();
    chain.solveForTarget( cwTarget );
    const cwDir = chain.bones[ 0 ].getDirectionUV();
    const cwEnd = chain.bones[ 0 ].end.clone();

    return {
        kind: 'direct',
        chain, acwTarget, cwTarget, acwDir, acwEnd, cwEnd, cwDir,
        acwLimitDegs: 55, cwLimitDegs: 15,
        planeAxis: new V3( 0, 1, 0 ),
        referenceAxis: new V3( 0, 0, 1 ),
        expectedAcwEnd: new V3( Math.sin( 55 * Math.PI / 180 ), 0, Math.cos( 55 * Math.PI / 180 ) ),
        expectedCwEnd: new V3( -Math.sin( 15 * Math.PI / 180 ), 0, Math.cos( 15 * Math.PI / 180 ) ),
        lengths: [ 1 ],
    };

}

// Control for world-space hinges: the host is rotated to +X, but the child's
// global hinge reference stays the world +Z axis.  A target 80 deg from world
// +Z therefore clamps at world-space +55 deg; it must NOT clamp relative to
// the host direction (which is 90 deg away).
export function globalHingeConnectedControl() {

    const host = hostChain( 2 );
    const template = singleBone( 1 );
    template.setHingeBaseboneConstraint( 'global', new V3( 0, 1, 0 ), 15, 55, new V3( 0, 0, 1 ) );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 2 + Math.sin( 80 * Math.PI / 180 ) * 0.7, 0, Math.cos( 80 * Math.PI / 180 ) * 0.7 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );

    structure.update();
    const child = structure.getChain( 1 );

    return {
        kind: 'structure',
        host, child, template,
        templateBefore, hostTargetBefore, childTargetBefore,
        hostTarget, childTarget,
        base: hostTarget.clone(),
        acwLimitDegs: 55,
        planeAxis: new V3( 0, 1, 0 ),
        referenceAxis: new V3( 0, 0, 1 ), // world axis, deliberately not host-relative
        expectedEnd: new V3( 2 + Math.sin( 55 * Math.PI / 180 ), 0, Math.cos( 55 * Math.PI / 180 ) ),
        lengths: [ 1 ],
    };

}

// Connection-point selection: two children hang off the second host bone, one
// at its start joint and one at its end.  The host both rotates and moves its
// joints; each child base must equal the real host endpoint it was connected to.
export function connectionStartEnd() {

    const host = twoBoneHost();
    const childAtStart = singleBone( 0.5 );
    const childAtEnd = singleBone( 0.5 );
    const hostTarget = new V3( 1.5, 0, 1.0 );
    const startTarget = new V3( 0.6, 0, 1.0 );
    const endTarget = new V3( 1.6, 0, 1.0 );

    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( childAtStart, 0, 1, 'start', startTarget, false );
    structure.connectChain( childAtEnd, 0, 1, 'end', endTarget, false );

    // Before any update the connection points are the host's initial joints.
    const initialHostStart = host.bones[ 1 ].start.clone();
    const initialHostEnd = host.bones[ 1 ].end.clone();
    const initialChildSBase = structure.getChain( 1 ).bones[ 0 ].start.clone();
    const initialChildEBase = structure.getChain( 2 ).bones[ 0 ].start.clone();

    structure.update();
    const cS = structure.getChain( 1 );
    const cE = structure.getChain( 2 );
    const first = {
        hostStart: host.bones[ 1 ].start.clone(),
        hostEnd: host.bones[ 1 ].end.clone(),
        childSBase: cS.bones[ 0 ].start.clone(),
        childEBase: cE.bones[ 0 ].start.clone(),
    };

    hostTarget.set( 1.2, 0, -0.8 );
    structure.update();
    const second = {
        hostStart: host.bones[ 1 ].start.clone(),
        hostEnd: host.bones[ 1 ].end.clone(),
        childSBase: cS.bones[ 0 ].start.clone(),
        childEBase: cE.bones[ 0 ].start.clone(),
    };

    return {
        kind: 'structure',
        host, childAtStart, childAtEnd,
        children: [ cS, cE ],
        initialHostStart, initialHostEnd,
        initialChildSBase, initialChildEBase,
        first, second,
        hostTarget,
        lengthsS: [ 0.5 ], lengthsE: [ 0.5 ],
    };

}

// Host translation: the host base is not pinned, so the whole two-bone host
// slides toward its target.  The child connected to the second bone's start
// joint must track that moving joint exactly.
export function connectionTranslatedHost() {

    const host = twoBoneHost();
    host.setFixedBaseMode( false );
    const template = singleBone( 0.5 );
    const hostTarget = new V3( 1.6, 0, 1.2 );
    const childTarget = new V3( 0.8, 0, 1.0 );

    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 1, 'start', childTarget, false );

    const templateBefore = snapshotChain( template );
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );
    structure.update();
    const child = structure.getChain( 1 );

    return {
        kind: 'structure',
        host, child, template, templateBefore,
        hostTargetBefore, childTargetBefore,
        hostTarget, childTarget,
        hostBase: host.bones[ 0 ].start.clone(),
        hostJoint: host.bones[ 1 ].start.clone(),
        childBase: child.bones[ 0 ].start.clone(),
        lengths: [ 0.5 ],
    };

}

// Single-bone child with a local rotor basebone constraint.  The target is
// 80 deg away from the host direction so the basebone is forced onto the
// 40 deg cone; the analytic pose is exactly 40 deg off the transformed axis
// and one unit out from the connection point.
export function localRotorSingle() {

    const host = hostChain( 2 );
    const template = singleBone( 1 );
    template.setRotorBaseboneConstraint( 'local', new V3( 0, 0, 1 ), 40 );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 2 + Math.cos( 80 * Math.PI / 180 ) * 0.7, 0, -Math.sin( 80 * Math.PI / 180 ) * 0.7 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );

    structure.update();
    const child = structure.getChain( 1 );

    return {
        kind: 'structure',
        host, child, template,
        templateBefore, hostTargetBefore, childTargetBefore,
        hostTarget, childTarget,
        base: hostTarget.clone(),
        hostDir: host.bones[ 0 ].getDirectionUV(),
        localAxis: new V3( 0, 0, 1 ),
        rotorDegs: 40,
        lengths: [ 1 ],
    };

}

// Two-bone child with a local hinge basebone constraint (15 cw / 55 acw).
// The target (one unit sideways, one unit back from the connection point) is
// exactly reachable with a right-angle elbow when the basebone sits on the
// host direction, which is the analytic pose the shipped solver converges to.
export function localHingeConnected() {

    const host = hostChain( 2 );
    const template = hingeTemplate( 15, 55 );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 3, 0, -1 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );

    structure.update();
    const child = structure.getChain( 1 );

    return {
        kind: 'structure',
        host, child, template,
        templateBefore, hostTargetBefore, childTargetBefore,
        hostTarget, childTarget,
        base: hostTarget.clone(),
        hostDir: host.bones[ 0 ].getDirectionUV(),
        localRotationAxis: new V3( 0, 1, 0 ),
        cwDegs: 15, acwDegs: 55,
        lengths: [ 1, 1 ],
        expectedEnd: childTarget.clone(),
    };

}

// One host, two children with different constraints solved in the same
// update: a local rotor child and a local hinge child.  The fixtures also
// verify that neither template nor either target vector is rewritten by the
// solve.
export function twoChildrenSharedHost() {

    const host = hostChain( 2 );
    const rotorTpl = singleBone( 1 );
    rotorTpl.setRotorBaseboneConstraint( 'local', new V3( 0, 0, 1 ), 40 );
    const hingeTpl = hingeTemplate( 15, 55 );

    const hostTarget = new V3( 2, 0, 0 );
    const rotorTarget = new V3( 2 + Math.cos( 80 * Math.PI / 180 ) * 0.7, 0, -Math.sin( 80 * Math.PI / 180 ) * 0.7 );
    const hingeTarget = new V3( 3, 0, -1 );

    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( rotorTpl, 0, 0, 'end', rotorTarget, false );
    structure.connectChain( hingeTpl, 0, 0, 'end', hingeTarget, false );

    const rotorTemplateBefore = snapshotChain( rotorTpl );
    const hingeTemplateBefore = snapshotChain( hingeTpl );
    const hostTargetBefore = snapshotV3( hostTarget );
    const rotorTargetBefore = snapshotV3( rotorTarget );
    const hingeTargetBefore = snapshotV3( hingeTarget );

    structure.update();
    const rotorChild = structure.getChain( 1 );
    const hingeChild = structure.getChain( 2 );

    return {
        kind: 'structure',
        host, rotorTpl, hingeTpl, rotorChild, hingeChild,
        rotorTemplateBefore, hingeTemplateBefore,
        hostTargetBefore, rotorTargetBefore, hingeTargetBefore,
        hostTarget, rotorTarget, hingeTarget,
        base: hostTarget.clone(),
        hostDir: host.bones[ 0 ].getDirectionUV(),
        rotorLocalAxis: new V3( 0, 0, 1 ), rotorDegs: 40,
        hingeLocalRotationAxis: new V3( 0, 1, 0 ), cwDegs: 15, acwDegs: 55,
        rotorLengths: [ 1 ], hingeLengths: [ 1, 1 ],
        hingeExpectedEnd: hingeTarget.clone(),
    };

}

// Consecutive retargeting: +X, then +Z, then back to +X.  Both reachable
// targets must be hit on every pass, and returning to the first target must
// reproduce the first effector location (the solver re-solves because the base
// and target both changed in between).
export function sequentialRetarget() {

    const host = hostChain( 2 );
    const template = new Chain3D();
    template.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    template.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    template.setRotorBaseboneConstraint( 'local', new V3( 0, 0, 1 ), 40 );
    tune( template );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 4, 0, 0 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );
    structure.update();
    const child = structure.getChain( 1 );
    const endX1 = child.bones[ 1 ].end.clone();
    const baseX1 = child.bones[ 0 ].start.clone();

    hostTarget.set( 0, 0, 2 );
    childTarget.set( 0, 0, 4 );
    structure.update();
    const endZ = child.bones[ 1 ].end.clone();
    const baseZ = child.bones[ 0 ].start.clone();

    hostTarget.set( 2, 0, 0 );
    childTarget.set( 4, 0, 0 );
    structure.update();
    const endX2 = child.bones[ 1 ].end.clone();
    const baseX2 = child.bones[ 0 ].start.clone();

    return {
        kind: 'structure',
        host, child, template, templateBefore,
        hostTarget, childTarget,
        endX1, endZ, endX2, baseX1, baseZ, baseX2,
        expectedEndX: new V3( 4, 0, 0 ),
        expectedEndZ: new V3( 0, 0, 4 ),
        expectedBaseX: new V3( 2, 0, 0 ),
        expectedBaseZ: new V3( 0, 0, 2 ),
        lengths: [ 1, 1 ],
    };

}

// Unreachable target: the child cannot get closer than its total length, so
// there is no end-position expectation.  What must still hold is finite
// numeric output, preserved bone lengths and connectivity, the true connection
// point, and a legal rotor direction.
export function unreachableTarget() {

    const host = hostChain( 2 );
    const template = rotorTemplate( 40 );

    const hostTarget = new V3( 2, 0, 0 );
    const childTarget = new V3( 2, 0, 10 );
    const structure = new Structure3D();
    structure.add( host, hostTarget, false );
    structure.connectChain( template, 0, 0, 'end', childTarget, false );

    const templateBefore = snapshotChain( template );
    const hostTargetBefore = snapshotV3( hostTarget );
    const childTargetBefore = snapshotV3( childTarget );

    structure.update();
    const child = structure.getChain( 1 );

    return {
        kind: 'structure',
        host, child, template,
        templateBefore, hostTargetBefore, childTargetBefore,
        hostTarget, childTarget,
        base: hostTarget.clone(),
        hostDir: host.bones[ 0 ].getDirectionUV(),
        localAxis: new V3( 0, 0, 1 ), rotorDegs: 40,
        lengths: [ 1, 1 ],
    };

}

// Direct solves must be isolated: solving one chain may not touch another
// chain object that is not part of the structure, nor the target vector passed
// in by the caller.
export function directSiblingIsolation() {

    const solved = new Chain3D();
    solved.addBone( new Bone3D( new V3(), new V3( 0, 0, 1 ) ) );
    solved.addConsecutiveBone( new V3( 0, 0, 1 ), 1 );
    solved.setRotorBaseboneConstraint( 'global', new V3( 0, 0, 1 ), 45 );
    tune( solved );

    const untouched = new Chain3D();
    untouched.addBone( new Bone3D( new V3(), new V3( 1, 0, 0 ) ) );
    untouched.addConsecutiveRotorConstrainedBone( new V3( 1, 0, 0 ), 1, 30 );
    tune( untouched );

    const untouchedBefore = snapshotChain( untouched );
    const target = new V3( 0.9, 0, 1.6 );
    const targetBefore = snapshotV3( target );

    solved.solveForTarget( target );

    return {
        kind: 'direct',
        chain: solved, untouched, untouchedBefore,
        target, targetBefore,
        lengths: [ 1, 1 ],
    };

}

export const CASES = {
    'baseline-host-rotation': baselineHostRotation,
    'direct-rotor-reach': directRotorReach,
    'direct-global-hinge-limits': directGlobalHingeLimits,
    'global-hinge-connected-control': globalHingeConnectedControl,
    'connection-start-end': connectionStartEnd,
    'connection-translated-host': connectionTranslatedHost,
    'local-rotor-single': localRotorSingle,
    'local-hinge-connected': localHingeConnected,
    'two-children-shared-host': twoChildrenSharedHost,
    'sequential-retarget': sequentialRetarget,
    'unreachable-target': unreachableTarget,
    'direct-sibling-isolation': directSiblingIsolation,
};
