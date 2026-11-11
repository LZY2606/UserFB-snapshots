/*
 * Assertions and reporting for the connected-rig regression.
 *
 * All checks are derived from the actual bone endpoints produced by the real
 * solver (and from the untouched inputs).  The solver's cached relative
 * constraint vectors are deliberately NOT used as an oracle: expected
 * world-space constraint directions are recomputed from the connected host
 * bone direction with M3, so a mutant that stops transforming the local axis
 * cannot also corrupt the expectation.
 *
 * Tolerances (unit bones make these easy to read in absolute terms):
 *   POS_EXACT    1e-9   coordinates that are copied/constructed, not solved
 *                       (bone lengths, shared joints, connection points,
 *                       input skeletons)
 *   POS_SOLVED   5e-3   reachable effector positions on 1-2 unit chains;
 *                       the solver's own precision default is 1e-3, tuned
 *                       runs converge to <=1.2e-3 on the geometric
 *                       straight-line cases, and a wrong constraint frame
 *                       moves the effector by >=0.3 -- 60x this tolerance
 *   POS_TIGHT    1e-6   full-length straight chains (rotor 0 / collinear),
 *                       matching the tolerance baseline/current.js uses
 *   ANGLE_DEG    0.5    directed-angle limits and rotor cone clamps are
 *                       applied exactly in the final backward pass; the
 *                       0.5 deg allowance is float noise, while swapping
 *                       15/55 or skipping the host transform shifts the
 *                       angle by >=10 deg
 *   PLANE_DOT    1e-6   |direction . hinge rotation axis| for a hinge plane
 *                       the solver projects onto
 */

import { V3 } from '../../src/math/V3.js';
import { M3 } from '../../src/math/M3.js';
import { DEG, CASES } from './fixtures.js';

const POS_EXACT = 1e-9;
const POS_SOLVED = 5e-3;
const POS_TIGHT = 1e-6;
const REPRO_TOL = 1e-4; // re-solving the same target/base pair reproduces the
                        // pose to ~1e-7 in practice; 1e-4 leaves two orders
                        // of headroom without admitting a wrong frame
const ANGLE_DEG = 0.5;
const PLANE_DOT = 1e-6;

let passCount = 0;
let failCount = 0;

function num( v ) {

    return Number( v ).toFixed( 6 );

}

function fmtV( v ) {

    return `(${num( v.x )}, ${num( v.y )}, ${num( v.z )})`;

}

function reportPass( fixture, id ) {

    passCount ++;
    console.log( `PASS fixture=${fixture} check=${id}` );

}

function reportFail( fixture, id, bone, expected, actual, detail ) {

    failCount ++;
    console.log( `FAIL fixture=${fixture} check=${id} bone=${bone} expected=${expected} actual=${actual} detail=${detail}` );

}

function near( fixture, id, bone, actual, expected, tol, what ) {

    const d = actual.distanceTo( expected );
    const ok = Number.isFinite( d ) && d <= tol;
    if ( ok ) reportPass( fixture, id );
    else reportFail( fixture, id, bone, `${what}=${fmtV( expected )}`, `${what}=${fmtV( actual )}`, `distance=${num( d )} tol=${tol}` );
    return ok;

}

function angleLe( fixture, id, bone, actualDeg, limitDeg, tol, what ) {

    const ok = Number.isFinite( actualDeg ) && actualDeg <= limitDeg + tol;
    if ( ok ) reportPass( fixture, id );
    else reportFail( fixture, id, bone, `${what}<=${num( limitDeg )}deg+${num( tol )}`, `${what}=${num( actualDeg )}deg`, 'directed angle outside limit' );
    return ok;

}

function angleNear( fixture, id, bone, actualDeg, expectedDeg, tol, what ) {

    const d = Math.abs( actualDeg - expectedDeg );
    const ok = Number.isFinite( d ) && d <= tol;
    if ( ok ) reportPass( fixture, id );
    else reportFail( fixture, id, bone, `${what}=${num( expectedDeg )}deg`, `${what}=${num( actualDeg )}deg`, `delta=${num( d )}deg tol=${num( tol )}deg` );
    return ok;

}

function finiteV( fixture, id, bone, v, what ) {

    const ok = Number.isFinite( v.x ) && Number.isFinite( v.y ) && Number.isFinite( v.z );
    if ( ok ) reportPass( fixture, id );
    else reportFail( fixture, id, bone, `${what}=finite`, `${what}=(${v.x}, ${v.y}, ${v.z})`, 'non-finite coordinate' );
    return ok;

}

function assertFiniteChain( fixture, chain, label ) {

    const id = `${label}-finite`;
    for ( let i = 0; i < chain.bones.length; i ++ ) {

        const b = chain.bones[ i ];
        finiteV( fixture, id, i, b.start, 'start' );
        finiteV( fixture, id, i, b.end, 'end' );

    }

}

function assertLengths( fixture, chain, lengths, label, tol = POS_EXACT ) {

    const id = `${label}-bone-length`;
    for ( let i = 0; i < chain.bones.length; i ++ ) {

        const b = chain.bones[ i ];
        const d = b.start.distanceTo( b.end );
        const ok = Number.isFinite( d ) && Math.abs( d - lengths[ i ] ) <= tol;
        if ( ok ) reportPass( fixture, id );
        else reportFail( fixture, id, i, `length=${num( lengths[ i ] )}`, `length=${num( d )}`, `delta=${num( Math.abs( d - lengths[ i ] ) )} tol=${tol}` );

    }

}

function assertConnectivity( fixture, chain, label, tol = POS_EXACT ) {

    const id = `${label}-connectivity`;
    for ( let i = 0; i + 1 < chain.bones.length; i ++ ) {

        const d = chain.bones[ i ].end.distanceTo( chain.bones[ i + 1 ].start );
        const ok = Number.isFinite( d ) && d <= tol;
        if ( ok ) reportPass( fixture, id );
        else reportFail( fixture, id, i + 1, `start=${fmtV( chain.bones[ i ].end )}`, `start=${fmtV( chain.bones[ i + 1 ].start )}`, `joint gap=${num( d )} tol=${tol}` );

    }

}

function transformedAxis( hostDir, localAxis ) {

    // Recompute the solver's expected world-space axis from the connected
    // bone direction using the shipped M3.  This is an independent second use
    // of the frame constructor, not a copy of the solve.
    const mtx = new M3().createRotationMatrix( hostDir.clone() );
    return localAxis.clone().applyM3( mtx );

}

function signedFrom( reference, dir, rotationAxis ) {

    return reference.getSignedAngle( dir, rotationAxis ) * DEG;

}

function assertTemplateUnchanged( fixture, template, before, label = 'template' ) {

    const id = `${label}-unchanged`;
    for ( let i = 0; i < before.length; i ++ ) {

        const snap = before[ i ];
        const b = template.bones[ i ];
        const same = b.start.x === snap.sx && b.start.y === snap.sy && b.start.z === snap.sz &&
            b.end.x === snap.ex && b.end.y === snap.ey && b.end.z === snap.ez;
        if ( same ) reportPass( fixture, id );
        else reportFail( fixture, id, i, `start=${snap.sx},${snap.sy},${snap.sz} end=${snap.ex},${snap.ey},${snap.ez}`,
            `start=${b.start.x},${b.start.y},${b.start.z} end=${b.end.x},${b.end.y},${b.end.z}`, 'input skeleton was rewritten by the solve' );

    }

}

function assertTargetUnchanged( fixture, target, before, label ) {

    const id = `${label}-target-unchanged`;
    const same = target.x === before.x && target.y === before.y && target.z === before.z;
    if ( same ) reportPass( fixture, id );
    else reportFail( fixture, id, 'n/a', `target=${before.x},${before.y},${before.z}`, `target=${target.x},${target.y},${target.z}`, 'caller target vector was rewritten by the solve' );

}

function assertFixedBase( fixture, chain, base, label ) {

    near( fixture, `${label}-fixed-base`, 0, chain.bones[ 0 ].start, base, POS_EXACT, 'base' );

}

function assertCone( fixture, chain, boneIndex, axis, maxDeg, label ) {

    const dir = chain.bones[ boneIndex ].getDirectionUV();
    angleLe( fixture, `${label}-cone`, boneIndex, dir.angleTo( axis ) * DEG, maxDeg, ANGLE_DEG, 'angleToAxis' );

}

function assertHingePlane( fixture, chain, boneIndex, rotationAxis, label ) {

    const dir = chain.bones[ boneIndex ].getDirectionUV();
    const dot = Math.abs( dir.dot( rotationAxis ) );
    const ok = Number.isFinite( dot ) && dot <= PLANE_DOT;
    if ( ok ) reportPass( fixture, `${label}-plane` );
    else reportFail( fixture, `${label}-plane`, boneIndex, `|dot(rotationAxis)|<=${PLANE_DOT}`, `|dot|=${num( dot )}`, `direction=${fmtV( dir )} axis=${fmtV( rotationAxis )}` );

}

// ---------------------------------------------------------------- fixtures

function checkBaselineHostRotation( f ) {

    const F = 'baseline-host-rotation';

    assertFixedBase( F, f.host, new V3( 0, 0, 0 ), 'host' );
    assertLengths( F, f.host, [ 2 ], 'host' );
    assertLengths( F, f.child, f.childLengths, 'child' );
    assertConnectivity( F, f.child, 'child' );

    // Phase where the host points +X.
    near( F, 'phase-x-host-end', 0, f.phaseX.hostEnd, f.expectedChildBaseX, POS_TIGHT, 'hostEnd' );
    near( F, 'phase-x-child-base', 0, f.phaseX.childBase, f.expectedChildBaseX, POS_EXACT, 'childBase' );
    near( F, 'phase-x-child-end', 1, f.phaseX.childEnd, f.expectedChildEndX, POS_TIGHT, 'childEnd' );

    // Phase where the host points +Z.
    near( F, 'phase-z-host-end', 0, f.phaseZ.hostEnd, f.expectedChildBaseZ, POS_TIGHT, 'hostEnd' );
    near( F, 'phase-z-child-base', 0, f.phaseZ.childBase, f.expectedChildBaseZ, POS_EXACT, 'childBase' );
    near( F, 'phase-z-child-end', 1, f.phaseZ.childEnd, f.expectedChildEndZ, POS_TIGHT, 'childEnd' );

    // The template chain and the caller's target vectors must be untouched.
    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkDirectRotorReach( f ) {

    const F = 'direct-rotor-reach';
    assertFixedBase( F, f.chain, new V3( 0, 0, 0 ), 'chain' );
    assertLengths( F, f.chain, f.lengths, 'chain' );
    assertConnectivity( F, f.chain, 'chain' );
    assertCone( F, f.chain, 0, f.coneAxis, f.coneDegs, 'basebone' );
    near( F, 'effector-position', 1, f.chain.bones[ 1 ].end, f.expectedEnd, POS_SOLVED, 'end' );
    assertTargetUnchanged( F, f.target, f.targetBefore, 'target' );

}

function checkDirectGlobalHingeLimits( f ) {

    const F = 'direct-global-hinge-limits';
    assertFixedBase( F, f.chain, new V3( 0, 0, 0 ), 'chain' );
    assertLengths( F, f.chain, f.lengths, 'chain' );

    // Anticlockwise solve: the basebone must sit exactly on +55 deg.
    angleNear( F, 'acw-solve-angle', 0, signedFrom( f.referenceAxis, f.acwDir, f.planeAxis ), f.acwLimitDegs, ANGLE_DEG, 'signedFromReference' );
    assertHingePlane( F, f.chain, 0, f.planeAxis, 'acw-solve' );
    near( F, 'acw-solve-end', 0, f.acwEnd, f.expectedAcwEnd, POS_TIGHT, 'end' );

    // Clockwise solve: the same chain must swing to exactly -15 deg.
    angleNear( F, 'cw-solve-angle', 0, signedFrom( f.referenceAxis, f.cwDir, f.planeAxis ), - f.cwLimitDegs, ANGLE_DEG, 'signedFromReference' );
    const dot = Math.abs( f.cwDir.dot( f.planeAxis ) );
    if ( dot <= PLANE_DOT ) reportPass( F, 'cw-solve-plane' );
    else reportFail( F, 'cw-solve-plane', 0, `|dot|<=${PLANE_DOT}`, `|dot|=${num( dot )}`, 'left hinge plane' );
    near( F, 'cw-solve-end', 0, f.chain.bones[ 0 ].end, f.expectedCwEnd, POS_TIGHT, 'end' );

}

function checkGlobalHingeConnectedControl( f ) {

    const F = 'global-hinge-connected-control';
    assertFixedBase( F, f.host, new V3( 0, 0, 0 ), 'host' );
    assertLengths( F, f.host, [ 2 ], 'host' );
    assertLengths( F, f.child, f.lengths, 'child' );
    near( F, 'child-base', 0, f.child.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );

    // The child's hinge reference is world +Z.  With the host at +X, a
    // correct world-space hinge clamps 55 deg from +Z, i.e. 35 deg from the
    // host direction.  If the global axis had been dragged along with the host
    // (or the limits had been swapped) this angle would be 55 or 75 deg.
    const dir = f.child.bones[ 0 ].getDirectionUV();
    angleNear( F, 'world-reference-angle', 0, signedFrom( f.referenceAxis, dir, f.planeAxis ), f.acwLimitDegs, ANGLE_DEG, 'signedFromWorldZ' );
    angleNear( F, 'world-axis-not-docked', 0, dir.angleTo( f.host.bones[ 0 ].getDirectionUV() ) * DEG, 90 - f.acwLimitDegs, ANGLE_DEG, 'angleToHostDir' );
    assertHingePlane( F, f.child, 0, f.planeAxis, 'hinge' );
    near( F, 'effector-position', 0, f.child.bones[ 0 ].end, f.expectedEnd, POS_TIGHT, 'end' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkConnectionStartEnd( f ) {

    const F = 'connection-start-end';
    assertFixedBase( F, f.host, new V3( 0, 0, 0 ), 'host' );
    assertLengths( F, f.host, [ 1, 1 ], 'host' );
    assertLengths( F, f.children[ 0 ], f.lengthsS, 'child-start' );
    assertLengths( F, f.children[ 1 ], f.lengthsE, 'child-end' );

    // Connection points at connect time (before any solve).
    near( F, 'initial-start-base', 0, f.initialChildSBase, f.initialHostStart, POS_EXACT, 'childBase' );
    near( F, 'initial-end-base', 0, f.initialChildEBase, f.initialHostEnd, POS_EXACT, 'childBase' );

    // Host's bone joints must be distinct, otherwise the swap cannot be seen.
    const jointLen = f.initialHostStart.distanceTo( f.initialHostEnd );
    if ( jointLen > 0.4 ) reportPass( F, 'joints-distinct' );
    else reportFail( F, 'joints-distinct', 1, 'joint separation>0.4', `separation=${num( jointLen )}`, 'host bone too short to distinguish start/end' );

    // After first solve.
    near( F, 'phase1-start-child-base', 0, f.first.childSBase, f.first.hostStart, POS_EXACT, 'childBase' );
    near( F, 'phase1-end-child-base', 0, f.first.childEBase, f.first.hostEnd, POS_EXACT, 'childBase' );

    // After retargeting the host (host rotates and its joints translate).
    near( F, 'phase2-start-child-base', 0, f.second.childSBase, f.second.hostStart, POS_EXACT, 'childBase' );
    near( F, 'phase2-end-child-base', 0, f.second.childEBase, f.second.hostEnd, POS_EXACT, 'childBase' );
    const moved = f.second.hostStart.distanceTo( f.first.hostStart );
    if ( moved > 0.05 ) reportPass( F, 'phase2-joint-moved' );
    else reportFail( F, 'phase2-joint-moved', 1, 'joint movement>0.05', `movement=${num( moved )}`, 'host did not move; fixture lost its point' );

}

function checkConnectionTranslatedHost( f ) {

    const F = 'connection-translated-host';
    assertLengths( F, f.host, [ 1, 1 ], 'host' );
    assertLengths( F, f.child, f.lengths, 'child' );

    near( F, 'child-base-tracks-joint', 0, f.childBase, f.hostJoint, POS_EXACT, 'childBase' );

    // The host base is not pinned, so it must actually have slid; otherwise
    // the fixture would not exercise translation at all.
    const baseMove = f.hostBase.distanceTo( new V3( 0, 0, 0 ) );
    if ( Number.isFinite( baseMove ) && baseMove > 0.1 ) reportPass( F, 'host-base-slid' );
    else reportFail( F, 'host-base-slid', 0, 'hostBase distance>0.1', `distance=${num( baseMove )}`, 'non-fixed host did not translate' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkLocalRotorSingle( f ) {

    const F = 'local-rotor-single';
    assertLengths( F, f.child, f.lengths, 'child' );
    near( F, 'child-base', 0, f.child.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );

    const axis = transformedAxis( f.hostDir, f.localAxis );
    const dir = f.child.bones[ 0 ].getDirectionUV();
    const cone = dir.angleTo( axis ) * DEG;
    angleNear( F, 'rotor-cone-angle', 0, cone, f.rotorDegs, ANGLE_DEG, 'angleToTransformedAxis' );
    angleLe( F, 'rotor-cone-bound', 0, cone, f.rotorDegs, ANGLE_DEG, 'angleToTransformedAxis' );

    // Analytic clamp: the basebone is the transformed axis rotated toward the
    // target direction by exactly the rotor limit.
    const targetDir = f.childTarget.clone().minus( f.base ).normalize();
    const correctionAxis = axis.clone().cross( targetDir ).normalize();
    const expectedDir = new M3().rotateAboutAxis( axis, f.rotorDegs * Math.PI / 180, correctionAxis );
    near( F, 'effector-position', 0, f.child.bones[ 0 ].end, f.base.plus( expectedDir.multiply( f.lengths[ 0 ] ) ), POS_TIGHT, 'end' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkLocalHingeConnected( f ) {

    const F = 'local-hinge-connected';
    assertLengths( F, f.child, f.lengths, 'child' );
    assertConnectivity( F, f.child, 'child' );
    near( F, 'child-base', 0, f.child.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );

    // The shipped solver carries the local hinge rotation axis into world
    // space with the host frame; the enforceable invariant is the directed
    // angle bound about that transformed axis.
    const axis = transformedAxis( f.hostDir, f.localRotationAxis );
    const dir = f.child.bones[ 0 ].getDirectionUV();
    angleLe( F, 'hinge-directed-bound', 0, dir.angleTo( axis ) * DEG, f.acwDegs, ANGLE_DEG, 'angleToTransformedAxis' );

    // The selected pose is exactly solvable: basebone on the host direction,
    // elbow at a right angle, effector on the target.
    near( F, 'effector-position', 1, f.child.bones[ 1 ].end, f.expectedEnd, POS_SOLVED, 'end' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkTwoChildrenSharedHost( f ) {

    const F = 'two-children-shared-host';
    assertLengths( F, f.host, [ 2 ], 'host' );
    assertLengths( F, f.rotorChild, f.rotorLengths, 'rotor-child' );
    assertLengths( F, f.hingeChild, f.hingeLengths, 'hinge-child' );

    // Both children must attach to the same real host endpoint.
    near( F, 'rotor-child-base', 0, f.rotorChild.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );
    near( F, 'hinge-child-base', 0, f.hingeChild.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );
    const shared = f.rotorChild.bones[ 0 ].start.distanceTo( f.hingeChild.bones[ 0 ].start );
    if ( Number.isFinite( shared ) && shared <= POS_EXACT ) reportPass( F, 'children-share-host-point' );
    else reportFail( F, 'children-share-host-point', 0, 'base gap<=1e-9', `gap=${num( shared )}`, 'children disagree on the connection point' );

    // Rotor child keeps its cone.
    const rotorAxis = transformedAxis( f.hostDir, f.rotorLocalAxis );
    assertCone( F, f.rotorChild, 0, rotorAxis, f.rotorDegs, 'rotor-child' );

    // Hinge child reaches its analytic elbow pose and stays inside its bound.
    const hingeAxis = transformedAxis( f.hostDir, f.hingeLocalRotationAxis );
    angleLe( F, 'hinge-child-bound', 0, f.hingeChild.bones[ 0 ].getDirectionUV().angleTo( hingeAxis ) * DEG, f.acwDegs, ANGLE_DEG, 'angleToTransformedAxis' );
    near( F, 'hinge-child-effector', 1, f.hingeChild.bones[ 1 ].end, f.hingeExpectedEnd, POS_SOLVED, 'end' );
    assertConnectivity( F, f.hingeChild, 'hinge-child' );

    // Solving one child must not rewrite the other child's input skeleton or
    // any caller target.
    assertTemplateUnchanged( F, f.rotorTpl, f.rotorTemplateBefore, 'rotor-template' );
    assertTemplateUnchanged( F, f.hingeTpl, f.hingeTemplateBefore, 'hinge-template' );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.rotorTarget, f.rotorTargetBefore, 'rotor' );
    assertTargetUnchanged( F, f.hingeTarget, f.hingeTargetBefore, 'hinge' );

}

function checkSequentialRetarget( f ) {

    const F = 'sequential-retarget';
    assertLengths( F, f.child, f.lengths, 'child' );
    assertConnectivity( F, f.child, 'child' );

    near( F, 'first-x-base', 0, f.baseX1, f.expectedBaseX, POS_EXACT, 'childBase' );
    near( F, 'first-x-end', 1, f.endX1, f.expectedEndX, POS_SOLVED, 'end' );
    near( F, 'middle-z-base', 0, f.baseZ, f.expectedBaseZ, POS_EXACT, 'childBase' );
    near( F, 'middle-z-end', 1, f.endZ, f.expectedEndZ, POS_SOLVED, 'end' );
    near( F, 'return-x-base', 0, f.baseX2, f.expectedBaseX, POS_EXACT, 'childBase' );
    near( F, 'return-x-end', 1, f.endX2, f.expectedEndX, POS_SOLVED, 'end' );

    // Returning to the original target must reproduce the original pose.
    near( F, 'return-reproduces-first', 1, f.endX2, f.endX1, REPRO_TOL, 'end' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, new V3( 2, 0, 0 ), 'host' );
    assertTargetUnchanged( F, f.childTarget, new V3( 4, 0, 0 ), 'child' );

}

function checkUnreachableTarget( f ) {

    const F = 'unreachable-target';
    assertFiniteChain( F, f.child, 'child' );
    assertLengths( F, f.child, f.lengths, 'child' );
    assertConnectivity( F, f.child, 'child' );
    near( F, 'child-base', 0, f.child.bones[ 0 ].start, f.base, POS_EXACT, 'childBase' );

    // No global-optimum requirement: just the constraint the solver enforces.
    const axis = transformedAxis( f.hostDir, f.localAxis );
    assertCone( F, f.child, 0, axis, f.rotorDegs, 'basebone' );

    const finite = Number.isFinite( f.child.currentSolveDistance );
    if ( finite ) reportPass( F, 'solve-distance-finite' );
    else reportFail( F, 'solve-distance-finite', 'n/a', 'finite', `${f.child.currentSolveDistance}`, 'non-finite solve distance' );

    assertTemplateUnchanged( F, f.template, f.templateBefore );
    assertTargetUnchanged( F, f.hostTarget, f.hostTargetBefore, 'host' );
    assertTargetUnchanged( F, f.childTarget, f.childTargetBefore, 'child' );

}

function checkDirectSiblingIsolation( f ) {

    const F = 'direct-sibling-isolation';
    assertLengths( F, f.chain, f.lengths, 'chain' );
    assertConnectivity( F, f.chain, 'chain' );
    assertFixedBase( F, f.chain, new V3( 0, 0, 0 ), 'chain' );

    // The sibling chain must be bit-for-bit untouched by the direct solve.
    for ( let i = 0; i < f.untouchedBefore.length; i ++ ) {

        const snap = f.untouchedBefore[ i ];
        const b = f.untouched.bones[ i ];
        const same = b.start.x === snap.sx && b.start.y === snap.sy && b.start.z === snap.sz &&
            b.end.x === snap.ex && b.end.y === snap.ey && b.end.z === snap.ez;
        if ( same ) reportPass( F, 'sibling-unchanged' );
        else reportFail( F, 'sibling-unchanged', i, `start=${snap.sx},${snap.sy},${snap.sz} end=${snap.ex},${snap.ey},${snap.ez}`,
            `start=${b.start.x},${b.start.y},${b.start.z} end=${b.end.x},${b.end.y},${b.end.z}`, 'direct solve rewrote an unrelated chain' );

    }

    assertTargetUnchanged( F, f.target, f.targetBefore, 'target' );
    near( F, 'effector-position', 1, f.chain.bones[ 1 ].end, new V3( 0.9, 0, 1.6 ), POS_SOLVED, 'end' );

}

const CHECKERS = {
    'baseline-host-rotation': checkBaselineHostRotation,
    'direct-rotor-reach': checkDirectRotorReach,
    'direct-global-hinge-limits': checkDirectGlobalHingeLimits,
    'global-hinge-connected-control': checkGlobalHingeConnectedControl,
    'connection-start-end': checkConnectionStartEnd,
    'connection-translated-host': checkConnectionTranslatedHost,
    'local-rotor-single': checkLocalRotorSingle,
    'local-hinge-connected': checkLocalHingeConnected,
    'two-children-shared-host': checkTwoChildrenSharedHost,
    'sequential-retarget': checkSequentialRetarget,
    'unreachable-target': checkUnreachableTarget,
    'direct-sibling-isolation': checkDirectSiblingIsolation,
};

function main() {

    const names = Object.keys( CASES );
    let order = 0;
    for ( const name of names ) {

        order ++;
        console.log( `CONNECTED_RIG_BEGIN fixture=${name} order=${order}` );
        let data;
        try {

            data = CASES[ name ]();

        } catch ( error ) {

            // A fixture that cannot even construct/solve is an
            // infrastructure problem, not a geometric detection.  Mark it
            // distinctly and abort: the shell driver treats a missing summary
            // as a failed run rather than as a caught mutation.
            console.log( `CONNECTED_RIG_ERROR fixture=${name} stage=build message=${error && error.message ? error.message : String( error )}` );
            throw error;

        }

        CHECKERS[ name ]( data );
        console.log( `CONNECTED_RIG_END fixture=${name}` );

    }

    console.log( `CONNECTED_RIG_SUMMARY pass=${passCount} fail=${failCount}` );
    if ( failCount > 0 ) process.exitCode = 1;

}

main();
