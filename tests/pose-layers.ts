import assert from 'node:assert/strict';
import { vec3, quat, mat4 } from 'gl-matrix';

import Armature   from '../src/armature/Armature';
import Pose       from '../src/armature/Pose';
import SkinMTX    from '../src/armature/skins/SkinMTX';
import Transform  from '../src/maths/Transform';
import QuatUtil   from '../src/maths/QuatUtil';
import PoseLayers from '../src/animation/PoseLayers';
import type { PoseLayer } from '../src/animation/PoseLayers';

const EPS = 1e-6;

//#region HELPERS
function qAxis( axis: vec3, deg: number ): quat{
    return quat.setAxisAngle( quat.create(), axis, deg * Math.PI / 180 );
}

function approx( a: number, b: number, eps = EPS ): boolean{
    return Math.abs( a - b ) <= eps;
}

function approxVec( a: vec3 | quat, b: vec3 | quat, eps = EPS ): void{
    assert.equal( a.length, b.length );
    for( let i=0; i < a.length; i++ ){
        assert.ok(
            approx( a[ i ], b[ i ], eps ),
            `component ${i}: ${a[ i ]} != ${b[ i ]}`
        );
    }
}

/** Quaternions q and -q describe the same orientation. */
function approxQuat( a: quat, b: quat, eps = EPS ): void{
    let dot = 0;
    for( let i=0; i < 4; i++ ) dot += a[ i ] * b[ i ];
    assert.ok( approx( Math.abs( dot ), 1, 1e-6 ), `quats not aligned: dot=${dot}` );
    const sign = dot < 0 ? -1 : 1;
    for( let i=0; i < 4; i++ ){
        assert.ok( approx( a[ i ], sign * b[ i ], eps ),
            `quat component ${i}: ${a[ i ]} != ${sign * b[ i ]}` );
    }
}

function approxMat4( a: mat4, b: mat4, eps = EPS ): void{
    for( let i=0; i < 16; i++ ){
        assert.ok( approx( a[ i ], b[ i ], eps ), `mat4[${i}]: ${a[ i ]} != ${b[ i ]}` );
    }
}

/** Deep snapshot of everything a pose carries, including stale world data. */
function snapshot( p: Pose ): { offset: Transform; bones: { pos: vec3; rot: quat; scl: vec3; wpos: vec3; wrot: quat; wscl: vec3 }[] }{
    return {
        offset : p.offset.clone(),
        bones  : p.bones.map( b => ({
            pos  : vec3.clone( b.local.pos ),
            rot  : quat.clone( b.local.rot ),
            scl  : vec3.clone( b.local.scl ),
            wpos : vec3.clone( b.world.pos ),
            wrot : quat.clone( b.world.rot ),
            wscl : vec3.clone( b.world.scl ),
        }) ),
    };
}

function assertPoseEqualsSnapshot( p: Pose, s: ReturnType<typeof snapshot> ): void{
    approxVec( p.offset.pos, s.offset.pos );
    approxVec( p.offset.rot, s.offset.rot );
    approxVec( p.offset.scl, s.offset.scl );
    p.bones.forEach( ( b, i ) => {
        approxVec( b.local.pos, s.bones[ i ].pos );
        approxVec( b.local.rot, s.bones[ i ].rot );
        approxVec( b.local.scl, s.bones[ i ].scl );
        approxVec( b.world.pos, s.bones[ i ].wpos );
        approxVec( b.world.rot, s.bones[ i ].wrot );
        approxVec( b.world.scl, s.bones[ i ].wscl );
    } );
}

function assertPosesEqual( a: Pose, b: Pose ): void{
    assert.equal( a.bones.length, b.bones.length );
    approxVec( a.offset.pos, b.offset.pos );
    approxVec( a.offset.rot, b.offset.rot );
    approxVec( a.offset.scl, b.offset.scl );
    a.bones.forEach( ( bone, i ) => {
        approxVec( bone.local.pos, b.bones[ i ].local.pos );
        approxVec( bone.local.rot, b.bones[ i ].local.rot );
        approxVec( bone.local.scl, b.bones[ i ].local.scl );
        approxVec( bone.world.pos, b.bones[ i ].world.pos );
        approxVec( bone.world.rot, b.bones[ i ].world.rot );
        approxVec( bone.world.scl, b.bones[ i ].world.scl );
    } );
}
//#endregion

//#region SKELETON : torso, two hands, two legs with feet
const arm = new Armature();
arm.addBone( 'root',      -1, undefined, [  0,   0, 0 ] );  // 0
arm.addBone( 'spine',      0, undefined, [  0,   1, 0 ] );  // 1
arm.addBone( 'chest',      1, undefined, [  0,   1, 0 ] );  // 2
arm.addBone( 'leftHand',   2, undefined, [  1,   0, 0 ] );  // 3
arm.addBone( 'rightHand',  2, undefined, [ -1,   0, 0 ] );  // 4
arm.addBone( 'leftLeg',    0, undefined, [  0.4, 0, 0 ] );  // 5
arm.addBone( 'leftFoot',   5, undefined, [  0,  -1, 0 ] );  // 6
arm.addBone( 'rightLeg',   0, undefined, [ -0.4, 0, 0 ] );  // 7
arm.addBone( 'rightFoot',  7, undefined, [  0,  -1, 0 ] );  // 8
arm.bind( SkinMTX, 1 );

const IDX = {
    root: 0, spine: 1, chest: 2, leftHand: 3, rightHand: 4,
    leftLeg: 5, leftFoot: 6, rightLeg: 7, rightFoot: 8,
};

const base = arm.newPose( true );
const rest = arm.newPose( true );

// Resting world endpoints that later assertions lean on.
approxVec( base.bones[ IDX.leftHand ].world.pos,  [  1,   2, 0 ] );
approxVec( base.bones[ IDX.rightHand ].world.pos, [ -1,   2, 0 ] );
approxVec( base.bones[ IDX.leftFoot ].world.pos,  [  0.4,-1, 0 ] );
approxVec( base.bones[ IDX.rightFoot ].world.pos, [ -0.4,-1, 0 ] );
//#endregion

//#region 1. ORDERED OVERRIDE ROTATIONS DO NOT COMMUTE
{
    const qx = qAxis( [ 1, 0, 0 ], 60 );
    const qz = qAxis( [ 0, 0, 1 ], 45 );

    const poseX = arm.newPose(); poseX.setLocalRot( 'chest', qx );
    const poseZ = arm.newPose(); poseZ.setLocalRot( 'chest', qz );

    const outAB = arm.newPose();
    const outBA = arm.newPose();

    PoseLayers.apply( base, [
        { pose: poseX, weight: 0.5, mode: 'override' },
        { pose: poseZ, weight: 0.5, mode: 'override' },
    ], outAB );

    PoseLayers.apply( base, [
        { pose: poseZ, weight: 0.5, mode: 'override' },
        { pose: poseX, weight: 0.5, mode: 'override' },
    ], outBA );

    // Independent expectation of the ordered slerp chain.
    const expAB = quat.slerp( quat.create(),
        quat.slerp( quat.create(), [ 0, 0, 0, 1 ], qx, 0.5 ), qz, 0.5 );
    const expBA = quat.slerp( quat.create(),
        quat.slerp( quat.create(), [ 0, 0, 0, 1 ], qz, 0.5 ), qx, 0.5 );

    approxQuat( outAB.bones[ IDX.chest ].local.rot, expAB );
    approxQuat( outBA.bones[ IDX.chest ].local.rot, expBA );
    assert.ok( quat.dot( expAB, expBA ) < 0.9999, 'layer order must change the result' );

    // World endpoint of the left hand: chest sits at (0,2,0) since a bone's own
    // rotation never rotates its own local translation; only the hand's does.
    const expectedEnd = vec3.add( vec3.create(), [ 0, 2, 0 ],
        vec3.transformQuat( vec3.create(), [ 1, 0, 0 ], expAB ) );
    approxVec( outAB.bones[ IDX.leftHand ].world.pos, expectedEnd );
    assert.ok(
        vec3.distance( outAB.bones[ IDX.leftHand ].world.pos,
                       outBA.bones[ IDX.leftHand ].world.pos ) > 1e-3,
        'hand endpoints must differ when layer order is swapped'
    );
}
//#endregion

//#region 2. ADDITIVE LAYER WITH NON-UNIT REFERENCE SCALE
{
    const qr = qAxis( [ 0, 0, 1 ], 30 );
    const ql = quat.mul( quat.create(), qr, qAxis( [ 1, 0, 0 ], 40 ) );

    const reference = arm.newPose();
    reference.setLocalRot( 'chest', qr );
    reference.setLocalPos( 'chest', [ 0, 1, 0 ] );
    reference.sclLocal( 'chest', [ 2, 2, 2 ] );

    const layer = arm.newPose();
    layer.setLocalRot( 'chest', ql );
    layer.setLocalPos( 'chest', [ 0, 1.6, 0.3 ] );
    layer.sclLocal( 'chest', [ 4, 1, 0.5 ] );

    // Root translation delta gives us a clean SkinMTX offset-buffer check.
    reference.setLocalPos( 'root', [ 0, 0, 0 ] );
    layer.setLocalPos( 'root', [ 0.3, 0, 0 ] );

    const out = arm.newPose();
    PoseLayers.apply( base, [
        { pose: layer, weight: 0.5, mode: 'additive', reference },
    ], out );

    // Rotation: diff = inverse(qr) * ql == qx40, slerp from identity at 0.5 == qx20.
    approxQuat( out.bones[ IDX.chest ].local.rot, qAxis( [ 1, 0, 0 ], 20 ) );

    // Position: base + 0.5 * (layer - reference).
    approxVec( out.bones[ IDX.chest ].local.pos, [ 0, 1.3, 0.15 ] );
    approxVec( out.bones[ IDX.root ].local.pos,  [ 0.15, 0, 0 ] );

    // Scale: 1 + 0.5 * (layer/reference - 1) per component.
    approxVec( out.bones[ IDX.chest ].local.scl, [ 1.5, 0.75, 0.625 ] );

    // Independently recompute the left-hand world endpoint through the TRS chain.
    // A bone's own TRS does not transform its own translation, so the chest sits
    // at root(0.15) + spine(0,1,0) + chest local (0,1.3,0.15); only the hand's
    // offset is rotated and scaled by the chest.
    const chestScl = vec3.fromValues( 1.5, 0.75, 0.625 );
    const chestRot = qAxis( [ 1, 0, 0 ], 20 );
    const chestWorld = vec3.fromValues( 0.15, 2.3, 0.15 );
    const handEnd = vec3.add( vec3.create(), chestWorld,
        vec3.transformQuat( vec3.create(),
            vec3.mul( vec3.create(), chestScl, [ 1, 0, 0 ] ), chestRot ) );
    approxVec( out.bones[ IDX.leftHand ].world.pos, handEnd );

    // Skin matrices must reflect the blended pose: rebuild the chain independently.
    arm.updateSkinFromPose( out );
    const skin = arm.skin as SkinMTX;
    const rootMat  = mat4.fromRotationTranslationScale( mat4.create(),
        out.bones[ IDX.root ].local.rot,  out.bones[ IDX.root ].local.pos,
        out.bones[ IDX.root ].local.scl );
    const spineMat = mat4.mul( mat4.create(), rootMat,
        mat4.fromRotationTranslationScale( mat4.create(),
            out.bones[ IDX.spine ].local.rot, out.bones[ IDX.spine ].local.pos,
            out.bones[ IDX.spine ].local.scl ) );
    const chestMat = mat4.mul( mat4.create(), spineMat,
        mat4.fromRotationTranslationScale( mat4.create(),
            chestRot, out.bones[ IDX.chest ].local.pos, chestScl ) );
    approxMat4( skin.world[ IDX.chest ], chestMat );
    approxVec(
        [ chestMat[ 12 ], chestMat[ 13 ], chestMat[ 14 ] ],
        chestWorld
    );
    // Root bind is identity, so its offset-buffer translation equals world translation.
    const buf = arm.getSkinOffsets()![ 0 ] as Float32Array;
    approx( buf[ IDX.root * 16 + 12 ], 0.15 );
}
//#endregion

//#region 3. MASK INHERITANCE, OWN VALUE OVERRIDE AND ZERO SUBTREES
{
    // Every bone receives the same conspicuous local position shift.
    const layer = arm.newPose();
    for( const b of layer.bones ) vec3.add( b.local.pos, b.local.pos, [ 0.7, 0.3, 0.1 ] );

    const out = arm.newPose();
    PoseLayers.apply( base, [
        { pose: layer, weight: 1, mode: 'override', mask: { spine: 1, leftHand: 0 } },
    ], out );

    // root and the whole leg branches have no named ancestor -> weight 0.
    approxVec( out.bones[ IDX.root ].local.pos,    [  0,   0, 0 ] );
    approxVec( out.bones[ IDX.leftLeg ].local.pos, [  0.4, 0, 0 ] );
    approxVec( out.bones[ IDX.leftFoot ].local.pos, [ 0, -1, 0 ] );
    approxVec( out.bones[ IDX.rightLeg ].local.pos, [ -0.4, 0, 0 ] );
    approxVec( out.bones[ IDX.rightFoot ].local.pos, [ 0, -1, 0 ] );

    // spine named explicitly, chest/rightHand inherit through the spine branch.
    approxVec( out.bones[ IDX.spine ].local.pos,     [ 0.7, 1.3, 0.1 ] );
    approxVec( out.bones[ IDX.chest ].local.pos,     [ 0.7, 1.3, 0.1 ] );
    approxVec( out.bones[ IDX.rightHand ].local.pos, [ -0.3, 0.3, 0.1 ] );

    // leftHand explicitly 0 keeps its own local channels even though ancestors moved.
    approxVec( out.bones[ IDX.leftHand ].local.pos, [ 1, 0, 0 ] );

    // Leg world endpoints are untouched end to end.
    approxVec( out.bones[ IDX.leftFoot ].world.pos,  [  0.4, -1, 0 ] );
    approxVec( out.bones[ IDX.rightFoot ].world.pos, [ -0.4, -1, 0 ] );

    // An explicit 0 on the spine masks the entire subtree.
    const outZero = arm.newPose();
    PoseLayers.apply( base, [
        { pose: layer, weight: 1, mode: 'override', mask: { spine: 0 } },
    ], outZero );
    for( let i=0; i < arm.bones.length; i++ ){
        approxVec( outZero.bones[ i ].local.pos, rest.bones[ i ].local.pos );
        approxVec( outZero.bones[ i ].local.rot, rest.bones[ i ].local.rot );
        approxVec( outZero.bones[ i ].local.scl, rest.bones[ i ].local.scl );
    }

    // Mask values are scaled by the layer weight.
    const outHalf = arm.newPose();
    PoseLayers.apply( base, [
        { pose: layer, weight: 0.5, mode: 'override', mask: { spine: 1 } },
    ], outHalf );
    approxVec( outHalf.bones[ IDX.spine ].local.pos, [ 0.35, 1.15, 0.05 ] );
    approxVec( outHalf.bones[ IDX.root ].local.pos,   [ 0, 0, 0 ] );
}
//#endregion

//#region 4. NEGATED QUATERNIONS ARE THE SAME ORIENTATION
{
    const qx = qAxis( [ 1, 0, 0 ], 60 );
    const posePos = arm.newPose(); posePos.setLocalRot( 'chest', qx );
    const poseNeg = arm.newPose(); poseNeg.setLocalRot( 'chest', QuatUtil.negate( quat.create(), qx ) );

    const outPos = arm.newPose();
    const outNeg = arm.newPose();
    PoseLayers.apply( base, [ { pose: posePos, weight: 0.5, mode: 'override' } ], outPos );
    PoseLayers.apply( base, [ { pose: poseNeg, weight: 0.5, mode: 'override' } ], outNeg );

    approxQuat( outPos.bones[ IDX.chest ].local.rot, outNeg.bones[ IDX.chest ].local.rot );
    approxVec( outPos.bones[ IDX.leftHand ].world.pos,
               outNeg.bones[ IDX.leftHand ].world.pos );

    // Same guarantee for the additive difference quaternion.
    const ref = arm.newPose(); ref.setLocalRot( 'chest', qAxis( [ 0, 0, 1 ], 20 ) );
    const addPos = arm.newPose(); addPos.setLocalRot( 'chest', qx );
    const addNeg = arm.newPose(); addNeg.setLocalRot( 'chest', QuatUtil.negate( quat.create(), qx ) );

    const aPos = arm.newPose();
    const aNeg = arm.newPose();
    PoseLayers.apply( base, [ { pose: addPos, weight: 0.5, mode: 'additive', reference: ref } ], aPos );
    PoseLayers.apply( base, [ { pose: addNeg, weight: 0.5, mode: 'additive', reference: ref } ], aNeg );
    approxQuat( aPos.bones[ IDX.chest ].local.rot, aNeg.bones[ IDX.chest ].local.rot );
}
//#endregion

//#region 5. OUTPUT ALIASES AND SHARED POSES
{
    const ref = arm.newPose();
    ref.setLocalPos( 'spine', [ 0, 1, 0 ] );
    ref.sclLocal( 'chest', [ 2, 2, 2 ] );

    const layer = arm.newPose();
    layer.setLocalPos( 'spine', [ 0.3, 1.8, 0.2 ] );
    layer.sclLocal( 'chest', [ 4, 1, 0.5 ] );
    layer.setLocalRot( 'chest', qAxis( [ 0, 1, 0 ], 35 ) );

    const layers : PoseLayer[] = [
        { pose: layer, weight: 0.5, mode: 'additive', reference: ref },
        { pose: layer, weight: 0.25, mode: 'override' },
    ];

    const independent = arm.newPose();
    PoseLayers.apply( base, layers, independent );

    // out === base
    const aliasBase = base.clone();
    PoseLayers.apply( aliasBase, layers, aliasBase );
    assertPosesEqual( aliasBase, independent );

    // out === a layer pose
    const layerCopy = layer.clone();
    PoseLayers.apply( base, layers, layerCopy );
    assertPosesEqual( layerCopy, independent );

    // out === the reference
    const refCopy = ref.clone();
    PoseLayers.apply( base, layers, refCopy );
    assertPosesEqual( refCopy, independent );

    // Same pose object referenced by multiple layers must not be mutated.
    const shared = arm.newPose();
    shared.setLocalPos( 'chest', [ 0.2, 1.4, 0.1 ] );
    const sharedSnap = snapshot( shared );
    const refSnap    = snapshot( ref );
    const layerSnap  = snapshot( layer );
    const baseSnap   = snapshot( base );
    const outShared = arm.newPose();
    PoseLayers.apply( base, [
        { pose: shared, weight: 0.4, mode: 'override' },
        { pose: shared, weight: 0.6, mode: 'additive', reference: ref },
    ], outShared );
    assertPoseEqualsSnapshot( shared, sharedSnap );
    assertPoseEqualsSnapshot( ref, refSnap );
    assertPoseEqualsSnapshot( layer, layerSnap );
    assertPoseEqualsSnapshot( base, baseSnap );

    // Repeated calls from the same base are deterministic (no accumulation).
    const again = arm.newPose();
    PoseLayers.apply( base, layers, again );
    assertPosesEqual( again, independent );
}
//#endregion

//#region 6. SCENE OFFSET IS IGNORED FOR LOCAL DELTAS AND KEPT FROM BASE
{
    const offsetBase = base.clone();
    offsetBase.offset.pos = vec3.fromValues( 10, 20, 30 );
    offsetBase.offset.rot = qAxis( [ 0, 1, 0 ], 15 );
    offsetBase.updateWorld();

    const layer = arm.newPose();
    layer.offset.pos = vec3.fromValues( 99, 99, 99 );  // must be ignored
    layer.setLocalPos( 'root', [ 0.2, 0, 0 ] );

    const ref = arm.newPose();
    ref.offset.pos = vec3.fromValues( -7, -7, -7 );    // must be ignored
    ref.setLocalPos( 'root', [ 0, 0, 0 ] );

    const out = arm.newPose();
    PoseLayers.apply( offsetBase, [
        { pose: layer, weight: 0.5, mode: 'additive', reference: ref },
    ], out );

    approxVec( out.offset.pos, [ 10, 20, 30 ] );
    approxVec( out.offset.rot, offsetBase.offset.rot );
    approxVec( out.bones[ IDX.root ].local.pos, [ 0.1, 0, 0 ] );

    // Root world = offset.rot * local.pos + offset.pos, so the base offset survives.
    const rootWorld = vec3.add( vec3.create(), [ 10, 20, 30 ],
        vec3.transformQuat( vec3.create(), [ 0.1, 0, 0 ], out.offset.rot ) );
    approxVec( out.bones[ IDX.root ].world.pos, rootWorld );

    // A layer pose carrying the same offset would not change anything either,
    // because scene offsets never enter the local blend.
    const outNoOffset = arm.newPose();
    PoseLayers.apply( base, [
        { pose: layer, weight: 0.5, mode: 'additive', reference: ref },
    ], outNoOffset );
    approxVec( outNoOffset.bones[ IDX.root ].local.pos, [ 0.1, 0, 0 ] );
    approxVec( outNoOffset.offset.pos, [ 0, 0, 0 ] );
}
//#endregion

//#region 7. ERRORS THROW AND NEVER HALF-UPDATE THE OUTPUT
{
    const goodLayer = arm.newPose();
    goodLayer.setLocalPos( 'chest', [ 0.5, 1, 0 ] );

    const expectThrow = ( layers: PoseLayer[], label: string ): void => {
        const out = arm.newPose();
        out.setLocalPos( 'chest', [ 0.11, 0.22, 0.33 ] );
        out.updateWorld();
        const snap = snapshot( out );
        assert.throws( () => PoseLayers.apply( base, layers, out ), undefined, label );
        assertPoseEqualsSnapshot( out, snap );
    };

    // Unknown bone name in mask.
    expectThrow( [
        { pose: goodLayer, weight: 1, mode: 'override', mask: { ghost: 1 } },
    ], 'unknown mask name' );

    // Pose from a different armature.
    const otherArm = new Armature();
    otherArm.addBone( 'root', -1 );
    const foreign = otherArm.newPose();
    expectThrow( [ { pose: foreign as unknown as Pose, weight: 1, mode: 'override' } ],
                 'different armature pose' );

    // Non-finite and out-of-range weights.
    expectThrow( [ { pose: goodLayer, weight: NaN, mode: 'override' } ], 'NaN weight' );
    expectThrow( [ { pose: goodLayer, weight: 1.5, mode: 'override' } ], 'weight > 1' );
    expectThrow( [ { pose: goodLayer, weight: -0.1, mode: 'override' } ], 'weight < 0' );

    // Additive layer missing a reference.
    expectThrow( [ { pose: goodLayer, weight: 1, mode: 'additive' } ], 'missing reference' );

    // Reference containing a zero scale component.
    const zeroRef = arm.newPose();
    zeroRef.sclLocal( 'spine', [ 1, 0, 1 ] );
    expectThrow( [ { pose: goodLayer, weight: 1, mode: 'additive', reference: zeroRef } ],
                 'zero reference scale' );

    // Output bound to another armature is rejected before any write.
    const foreignOut = otherArm.newPose();
    assert.throws( () => PoseLayers.apply( base, [], foreignOut as unknown as Pose ) );
}
//#endregion

//#region 8. WORLD DATA IS REBUILT, NOT LEFT STALE
{
    const layer = arm.newPose();
    layer.setLocalRot( 'spine', qAxis( [ 0, 0, 1 ], 45 ) );
    layer.setLocalPos( 'leftHand', [ 1, 0.5, 0 ] );

    const out = arm.newPose();
    // Deliberately stale, bogus world data.
    for( const b of out.bones ) vec3.set( b.world.pos, 42, 42, 42 );
    PoseLayers.apply( base, [ { pose: layer, weight: 1, mode: 'override' } ], out );

    // Recompute every world transform independently from the blended locals.
    for( let i=0; i < arm.bones.length; i++ ){
        const t = out.getWorldTransform( i, new Transform() );
        approxVec( out.bones[ i ].world.pos, t.pos );
        approxVec( out.bones[ i ].world.rot, t.rot );
        approxVec( out.bones[ i ].world.scl, t.scl );
    }

    // SkinMTX must consume the synced pose directly.
    arm.updateSkinFromPose( out );
    const skin = arm.skin as SkinMTX;
    approxVec(
        [ skin.world[ IDX.leftHand ][ 12 ], skin.world[ IDX.leftHand ][ 13 ], skin.world[ IDX.leftHand ][ 14 ] ],
        out.bones[ IDX.leftHand ].world.pos
    );
    approxVec( out.bones[ IDX.leftFoot ].world.pos, [ 0.4, -1, 0 ] );
}
//#endregion

console.log( 'POSE_LAYERS_OK' );
