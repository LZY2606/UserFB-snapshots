//#region IMPORTS
import { group, check }   from '../task_support/check.mjs';
import Armature           from '../src/armature/Armature';
import CoRSkinner         from '../src/armature/skins/CoRSkinner';
import { vec3, quat }     from 'gl-matrix';
//#endregion

//#region HELPERS
const near = ( a: number, b: number, eps = 1e-6 ): boolean => Math.abs( a - b ) <= eps;

const vecEq = ( a: ArrayLike< number >, b: ArrayLike< number >, eps = 1e-6 ): boolean => {
    if( a.length !== b.length ) return false;
    for( let i=0; i < a.length; i++ ) if( Math.abs( a[i] - b[i] ) > eps ) return false;
    return true;
};

const throws = ( fn: () => unknown ): boolean => {
    try{ fn(); return false; }catch{ return true; }
};

function twoBoneArm(): Armature{
    const arm = new Armature();
    arm.addBone( 'root',  -1, undefined, [ 0,0,0 ] );
    arm.addBone( 'child',  0, undefined, [ 0,1,0 ] );
    return arm;
}

// Quad in the z=0 plane, x in [0,0.4], y in [0.8,1.2]; all vertices weighted
// (0.5, 0.5), so every rotation center is the area centroid (0.2, 1.0, 0).
function quadMesh(){
    const pos  = [ 0,0.8,0,  0.4,0.8,0,  0.4,1.2,0,  0,1.2,0 ];
    const idx  = [ 0,1,2, 0,2,3 ];
    const rows = [
        [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]],
        [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]],
    ];
    return { pos, idx, rows };
}

function triArea3( a: ArrayLike<number>, b: ArrayLike<number>, c: ArrayLike<number> ): number{
    const e1x = b[0]-a[0], e1y = b[1]-a[1], e1z = b[2]-a[2];
    const e2x = c[0]-a[0], e2y = c[1]-a[1], e2z = c[2]-a[2];
    const cx  = e1y*e2z - e1z*e2y;
    const cy  = e1z*e2x - e1x*e2z;
    const cz  = e1x*e2y - e1y*e2x;
    return 0.5 * Math.sqrt( cx*cx + cy*cy + cz*cz );
}

// Independent high-order reference: composite degree-5 (7-point Dunavant)
// quadrature over a regular nSub x nSub barycentric grid per triangle.
// Shares no code with CoRSkinner's adaptive integrator.
function referenceCenter(
    uRow      : Array< [ number, number ] >,
    rows      : Array< Map< number, number > >,
    positions : ArrayLike< number >,
    indices   : ArrayLike< number >,
    sigma     : number,
    nSub      : number,
): [ number, number, number ] | null{
    const SQ15 = Math.sqrt( 15 );
    const A = ( 6 - SQ15 ) / 21, B = ( 9 + 2*SQ15 ) / 21, W1 = ( 155 - SQ15 ) / 1200;
    const C = ( 6 + SQ15 ) / 21, D = ( 9 - 2*SQ15 ) / 21, W2 = ( 155 + SQ15 ) / 1200;
    const QL = [
        [ 1/3, 1/3, 1/3, 0.225 ],
        [ A, A, B, W1 ], [ A, B, A, W1 ], [ B, A, A, W1 ],
        [ C, C, D, W2 ], [ C, D, C, W2 ], [ D, C, C, W2 ],
    ];
    const uB     = uRow.map( e => e[0] );
    const uW     = uRow.map( e => e[1] );
    const nb     = uB.length;
    const invS2  = 1 / ( sigma * sigma );
    let den = 0, mx = 0, my = 0, mz = 0;

    for( let t=0; t < indices.length; t += 3 ){
        const iv = [ indices[t], indices[t+1], indices[t+2] ];
        const cw = uB.map( b => iv.map( v => rows[v].get( b ) ?? 0 ) );
        let present = 0;
        for( const arr of cw ) if( arr.some( x => x > 0 ) ) present++;
        if( present < 2 ) continue;   // S == 0 on the whole triangle

        const P  = iv.map( v => [ positions[v*3], positions[v*3+1], positions[v*3+2] ] );
        const sa = triArea3( P[0], P[1], P[2] ) / ( nSub * nSub );

        for( let i=0; i < nSub; i++ ){
            for( let j=0; j < nSub - i; j++ ){
                // sub-triangles of cell (i,j) in the (l1,l2) barycentric grid
                const cells = [ [ [i,j], [i+1,j], [i,j+1] ] ];
                if( i + j < nSub - 1 ) cells.push( [ [i+1,j], [i,j+1], [i+1,j+1] ] );

                for( const cell of cells ){
                    for( const q of QL ){
                        // map rule point (q0,q1,q2) of the sub-triangle to the parent
                        let l1 = 0, l2 = 0;
                        for( let k=0; k<3; k++ ){
                            l1 += q[k] * cell[k][0] / nSub;
                            l2 += q[k] * cell[k][1] / nSub;
                        }
                        const l0 = 1 - l1 - l2;

                        let s = 0;
                        for( let a=0; a < nb; a++ ){
                            const Wa = l0*cw[a][0] + l1*cw[a][1] + l2*cw[a][2];
                            if( Wa === 0 ) continue;
                            for( let b=a+1; b < nb; b++ ){
                                const Wb = l0*cw[b][0] + l1*cw[b][1] + l2*cw[b][2];
                                if( Wb === 0 ) continue;
                                const d = uW[a]*Wb - uW[b]*Wa;
                                s += uW[a]*uW[b]*Wa*Wb*Math.exp( -d*d*invS2 );
                            }
                        }
                        const px = l0*P[0][0] + l1*P[1][0] + l2*P[2][0];
                        const py = l0*P[0][1] + l1*P[1][1] + l2*P[2][1];
                        const pz = l0*P[0][2] + l1*P[1][2] + l2*P[2][2];
                        den += sa * q[3] * s;
                        mx  += sa * q[3] * s * px;
                        my  += sa * q[3] * s * py;
                        mz  += sa * q[3] * s * pz;
                    }
                }
            }
        }
    }

    if( den === 0 ) return null;
    return [ mx/den, my/den, mz/den ];
}

// Compose world transforms by walking the parent chain (independent of the
// Armature/Pose implementations under test).
function chainWorlds(
    locals : Array< { pidx: number, rot: quat, pos: vec3 } >,
    offset ?: { rot: quat, pos: vec3 },
): Array< { rot: quat, pos: vec3 } >{
    const out: Array< { rot: quat, pos: vec3 } > = new Array( locals.length );
    const solve = ( i: number ): void => {
        if( out[ i ] ) return;
        const l  = locals[ i ];
        const lr = quat.normalize( [0,0,0,0], l.rot );
        if( l.pidx === -1 ){
            out[ i ] = { rot: lr, pos: [ l.pos[0], l.pos[1], l.pos[2] ] };
        }else{
            solve( l.pidx );
            const p  = out[ l.pidx ];
            const wp = vec3.add( [0,0,0], vec3.transformQuat( [0,0,0], l.pos, p.rot ), p.pos );
            out[ i ] = { rot: quat.normalize( [0,0,0,0], quat.mul( [0,0,0,0], p.rot, lr ) ), pos: wp };
        }
    };
    for( let i=0; i < locals.length; i++ ) solve( i );
    if( offset ){
        for( let i=0; i < out.length; i++ ){
            out[ i ] = {
                rot : quat.normalize( [0,0,0,0], quat.mul( [0,0,0,0], offset.rot, out[i].rot ) ),
                pos : vec3.add( [0,0,0], vec3.transformQuat( [0,0,0], out[i].pos, offset.rot ), offset.pos ),
            };
        }
    }
    return out;
}
//#endregion

//#region INTEGRALS
group( 'integrals', () => {

    // -- constant kernel: center is the area-weighted centroid with true 3D areas
    {
        const arm  = twoBoneArm();
        const pos  = [ 0,0,0,  2,0,0,  2,1,0,  0,1,1 ];
        const idx  = [ 0,1,2, 0,2,3 ];                 // areas 1.0 and 1.5, non-coplanar
        const rows = [ [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]] ];
        const sk   = new CoRSkinner();
        const res  = sk.bind( arm, pos, idx, rows, { sigma: 0.3 } );
        check( res.ok, 'constant kernel bind completes' );
        check( res.integrals === 88, 'deterministic integral count for constant kernel' );
        check( res.errDenom < 1e-12 && res.errMoment < 1e-12, 'constant kernel integrates exactly' );
        // centroid = ( 1.0*(4/3,1/3,0) + 1.5*(2/3,2/3,1/3) ) / 2.5
        const exp = [ (4/3 + 1.5*(2/3)) / 2.5, (1/3 + 1.5*(2/3)) / 2.5, (1.5*(1/3)) / 2.5 ];
        const c   = sk.getCenters()!;
        for( let v=0; v < 4; v++ )
            check( vecEq( [ c[v*3], c[v*3+1], c[v*3+2] ], exp, 1e-9 ), `constant kernel center ${v} is the surface centroid` );
    }

    // -- sharp kernel, non-uniform weights, unequal areas vs independent quadrature
    {
        const arm   = twoBoneArm();
        const pos   = [ 0,0,0,  1.5,0,0,  0,1,0,  1,1,0.5 ];
        const idx   = [ 0,1,2, 1,3,2 ];
        const rowsIn = [
            [[0,1]],
            [[0,0.25],[1,0.75]],
            [[0,0.5],[1,0.5]],
            [[1,1]],
        ];
        const sigma = 0.05;
        const sk    = new CoRSkinner();
        const res   = sk.bind( arm, pos, idx, rowsIn, { sigma } );
        check( res.ok, 'sharp kernel bind completes' );
        check( res.errDenom <= 1e-6 && res.errMoment <= 1e-6, 'reported error estimates are within tolerance' );
        check( res.integrals > 88, 'sharp kernel triggers adaptive refinement' );

        const rowMaps = rowsIn.map( r => new Map< number, number >( r as Array<[number,number]> ) );
        const cen     = sk.getCenters()!;
        for( let v=0; v < 4; v++ ){
            const uRow    = [ ...rowMaps[v].entries() ];
            const rCoarse = referenceCenter( uRow, rowMaps, pos, idx, sigma, 128 );
            const rFine   = referenceCenter( uRow, rowMaps, pos, idx, sigma, 256 );
            if( rCoarse === null || rFine === null ){
                check( vecEq( [ cen[v*3], cen[v*3+1], cen[v*3+2] ], [ pos[v*3], pos[v*3+1], pos[v*3+2] ], 1e-12 ),
                    `vertex ${v}: zero denominator falls back to the vertex position` );
                continue;
            }
            check( vecEq( rCoarse, rFine, 1e-5 ), `vertex ${v}: reference quadrature self-converges` );
            check( vecEq( [ cen[v*3], cen[v*3+1], cen[v*3+2] ], rFine, 1e-4 ),
                `vertex ${v}: center matches independent high-order quadrature` );
        }
    }

    // -- subdividing the mesh must not move the centers
    {
        const arm   = twoBoneArm();
        const pos   = [ 0,0,0,  1.2,0.1,0,  0.2,1.1,0.3,  1.0,1.0,-0.2 ];
        const idx   = [ 0,1,2, 1,3,2 ];
        const rowsIn = [
            [[0,1]],
            [[0,0.5],[1,0.5]],
            [[0,0.2],[1,0.8]],
            [[1,1]],
        ];
        const skA = new CoRSkinner();
        check( skA.bind( arm, pos, idx, rowsIn, { sigma: 0.15 } ).ok, 'base bind completes' );

        // 1 -> 4 split of every triangle; midpoint weights are interpolated
        const pos2  = pos.slice();
        const rows2: Array< Array< [number,number] > > = rowsIn.map( r => r.map( e => [ e[0], e[1] ] as [number,number] ) );
        const idx2: Array< number > = [];
        const cache = new Map< string, number >();
        const midpoint = ( a: number, b: number ): number => {
            const key = a < b ? a+'_'+b : b+'_'+a;
            let m = cache.get( key );
            if( m === undefined ){
                m = pos2.length / 3;
                pos2.push(
                    0.5*( pos[a*3]   + pos[b*3]   ),
                    0.5*( pos[a*3+1] + pos[b*3+1] ),
                    0.5*( pos[a*3+2] + pos[b*3+2] ),
                );
                const merged = new Map< number, number >();
                for( const [ bi, w ] of rowsIn[a] ) merged.set( bi, ( merged.get( bi ) ?? 0 ) + 0.5*w );
                for( const [ bi, w ] of rowsIn[b] ) merged.set( bi, ( merged.get( bi ) ?? 0 ) + 0.5*w );
                rows2.push( [ ...merged.entries() ] );
                cache.set( key, m );
            }
            return m;
        };
        for( let t=0; t < idx.length; t += 3 ){
            const a = idx[t], b = idx[t+1], c = idx[t+2];
            const ab = midpoint( a, b ), bc = midpoint( b, c ), ca = midpoint( c, a );
            idx2.push( a,ab,ca,  ab,b,bc,  ca,bc,c,  ab,bc,ca );
        }
        const skB = new CoRSkinner();
        check( skB.bind( arm, pos2, idx2, rows2, { sigma: 0.15 } ).ok, 'subdivided bind completes' );
        const cA = skA.getCenters()!, cB = skB.getCenters()!;
        for( let v=0; v < 4; v++ )
            check( vecEq( [ cA[v*3], cA[v*3+1], cA[v*3+2] ], [ cB[v*3], cB[v*3+1], cB[v*3+2] ], 1e-5 ),
                `vertex ${v}: subdivision keeps the center within tolerance` );
    }

    // -- zero denominator: center falls back to the vertex position
    {
        const arm = twoBoneArm();
        // two disconnected components (one bone each) + one isolated vertex
        const pos    = [ 0,0,0, 1,0,0, 0,1,0,   5,5,5, 6,5,5, 5,6,5,   9,9,9 ];
        const idx    = [ 0,1,2, 3,4,5 ];
        const rowsIn = [
            [[0,1]], [[0,1]], [[0,1]],
            [[1,1]], [[1,1]], [[1,1]],
            [[0,0.5],[1,0.5]],   // isolated: no triangle carries both bones
        ];
        const sk  = new CoRSkinner();
        const res = sk.bind( arm, pos, idx, rowsIn, { sigma: 0.3 } );
        check( res.ok, 'multi-component bind completes' );
        const c = sk.getCenters()!;
        for( let v=0; v < 7; v++ )
            check( vecEq( [ c[v*3], c[v*3+1], c[v*3+2] ], [ pos[v*3], pos[v*3+1], pos[v*3+2] ], 1e-12 ),
                `vertex ${v}: zero denominator center equals the vertex` );
    }

    // -- budget exhaustion is reported honestly and keeps the old binding
    {
        const arm   = twoBoneArm();
        const pos   = [ 0,0,0,  1.5,0,0,  0,1,0,  1,1,0.5 ];
        const idx   = [ 0,1,2, 1,3,2 ];
        const rowsIn = [ [[0,1]], [[0,0.25],[1,0.75]], [[0,0.5],[1,0.5]], [[1,1]] ];

        const sk  = new CoRSkinner();
        const bad = sk.bind( arm, pos, idx, rowsIn, { sigma: 0.05, maxIntegrals: 10 } );
        check( !bad.ok, 'tiny budget reports incomplete' );
        check( Number.isFinite( bad.errDenom ) && bad.errDenom >= 0, 'incomplete result still reports error estimates' );
        check( !sk.isBound(), 'failed bind leaves the object unbound' );
        check( throws( () => sk.deform( arm.newPose() ) ), 'deform without a binding throws' );

        const good = sk.bind( arm, pos, idx, rowsIn, { sigma: 0.05 } );
        check( good.ok, 'bind with default budget completes' );
        const pose = arm.newPose();
        pose.rotLocal( 1, 90, 'y' );
        const out1 = sk.deform( pose );

        const again = sk.bind( arm, pos, idx, rowsIn, { sigma: 0.05, maxIntegrals: 10 } );
        check( !again.ok, 're-bind with tiny budget reports incomplete' );
        const out2 = sk.deform( pose );
        check( vecEq( out1, out2, 0 ), 'failed re-bind keeps the previous binding usable' );
    }

    // -- input validation
    {
        const arm  = twoBoneArm();
        const pos  = [ 0,0,0, 1,0,0, 0,1,0 ];
        const idx  = [ 0,1,2 ];
        const good = [ [[0,0.5],[1,0.5]], [[0,1]], [[1,1]] ];
        const sk   = new CoRSkinner();
        check( throws( () => sk.bind( arm, pos, idx, [ [[0,-0.5],[1,1.5]], [[0,1]], [[1,1]] ] ) ), 'negative weight rejected' );
        check( throws( () => sk.bind( arm, pos, idx, [ [], [[0,1]], [[1,1]] ] ) ), 'empty weight row rejected' );
        check( throws( () => sk.bind( arm, pos, idx, [ [[0,0]], [[0,1]], [[1,1]] ] ) ), 'all-zero weight row rejected' );
        check( throws( () => sk.bind( arm, pos, idx, [ [[2,1]], [[0,1]], [[1,1]] ] ) ), 'bone index out of range rejected' );
        check( throws( () => sk.bind( arm, pos, idx, good, { sigma: 0 } ) ), 'zero sigma rejected' );
        check( throws( () => sk.bind( arm, pos, idx, good, { sigma: -0.5 } ) ), 'negative sigma rejected' );
        check( throws( () => sk.bind( arm, [ 0,0,0, 1,0,0, 0,0,0 ], idx, good ) ), 'zero-area triangle rejected' );
        check( throws( () => sk.bind( arm, pos, idx, [ [[0,1]], [[0,1]] ] ) ), 'row count mismatch rejected' );
        check( !sk.isBound(), 'validation errors do not bind' );

        // duplicate influences are merged, rows are normalized
        const sk2 = new CoRSkinner();
        const res = sk2.bind( arm, pos, idx, [ [[0,0.2],[0,0.4],[1,0.3]], [[0,1]], [[1,1]] ] );
        check( res.ok, 'merged bind completes' );
        const rows = sk2.getWeightRows()!;
        check( rows[0].length === 2, 'duplicate bone influences are merged' );
        check( near( rows[0][0][1], 0.6/0.9, 1e-12 ) && near( rows[0][1][1], 0.3/0.9, 1e-12 ), 'merged row is normalized' );
        check( rows[0][0][0] === 0 && rows[0][1][0] === 1, 'rows are sorted by bone index' );
    }
} );
//#endregion

//#region DEFORMATION
group( 'deformation', () => {

    // -- rest pose restores the input positions
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows, { sigma: 0.3 } ).ok, 'bind completes' );
        const out = sk.deform( arm.newPose() );
        check( vecEq( out, pos, 1e-5 ), 'bind pose restores positions' );
    }

    // -- 180 degree twist: CoR differs from LBS and from dual-quaternion translation
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows, { sigma: 0.3 } ).ok, 'bind completes' );
        const cen = sk.getCenters()!;
        check( vecEq( [ cen[6], cen[7], cen[8] ], [ 0.2, 1.0, 0 ], 1e-9 ), 'center is the quad centroid' );

        const pose = arm.newPose();
        pose.rotLocal( 'child', 180, 'y' );
        const out = sk.deform( pose );

        // Bone transforms: T0 = identity, T1 = ( rot180y, 0 )
        const q0: quat = [ 0,0,0,1 ];
        const q1       = quat.setAxisAngle( [0,0,0,0], [0,1,0], Math.PI );
        const qi       = quat.normalize( [0,0,0,0], quat.add( [0,0,0,0], q0, q1 ) ); // 50/50 blend -> rot90y
        const c        = [ 0.2, 1.0, 0 ];
        const c1       = vec3.transformQuat( [0,0,0], c, q1 );
        const cBlend   = [ 0.5*(c[0]+c1[0]), 0.5*(c[1]+c1[1]), 0.5*(c[2]+c1[2]) ];

        const corExpected = ( p: number[] ): number[] => {
            const d = vec3.sub( [0,0,0], p, c );
            const r = vec3.transformQuat( [0,0,0], d, qi );
            return [ r[0]+cBlend[0], r[1]+cBlend[1], r[2]+cBlend[2] ];
        };
        const lbsExpected = ( p: number[] ): number[] => {
            const r = vec3.transformQuat( [0,0,0], p, q1 );
            return [ 0.5*(p[0]+r[0]), 0.5*(p[1]+r[1]), 0.5*(p[2]+r[2]) ];
        };
        const dqExpected = ( p: number[] ): number[] => {
            // both bone translations are zero -> the dual part blends to zero
            const r = vec3.transformQuat( [0,0,0], p, qi );
            return [ r[0], r[1], r[2] ];
        };

        let maxLbs = 0, maxDq = 0;
        for( let v=0; v < 4; v++ ){
            const p = [ pos[v*3], pos[v*3+1], pos[v*3+2] ];
            const o = [ out[v*3], out[v*3+1], out[v*3+2] ];
            check( vecEq( o, corExpected( p ), 1e-5 ), `vertex ${v}: twist matches the CoR transport formula` );
            const l = lbsExpected( p ), d = dqExpected( p );
            maxLbs = Math.max( maxLbs, Math.abs( o[0]-l[0] ), Math.abs( o[1]-l[1] ), Math.abs( o[2]-l[2] ) );
            maxDq  = Math.max( maxDq,  Math.abs( o[0]-d[0] ), Math.abs( o[1]-d[1] ), Math.abs( o[2]-d[2] ) );
            // rotation around the transported center preserves the radius
            const rOut = Math.hypot( o[0]-cBlend[0], o[1]-cBlend[1], o[2]-cBlend[2] );
            const rIn  = Math.hypot( p[0]-c[0], p[1]-c[1], p[2]-c[2] );
            check( near( rOut, rIn, 1e-5 ), `vertex ${v}: radius around the center is preserved` );
        }
        check( maxLbs > 0.05, 'CoR twist differs from linear blend skinning' );
        check( maxDq  > 0.05, 'CoR twist differs from dual-quaternion translation' );
        // hand-computed: v2 = (0.4,1.2,0) -> rot90y*(0.2,0.2,0) + (0,1,0) = (0,1.2,-0.2)
        check( vecEq( [ out[6], out[7], out[8] ], [ 0, 1.2, -0.2 ], 1e-5 ), 'hand-checked twist value' );
    }

    // -- single-influence vertices move as an exact rigid body
    {
        const arm  = twoBoneArm();
        const pos  = [ 0.3,1.1,0.2,  -0.2,0.9,0.1,  0.1,1.3,-0.3 ];
        const idx  = [ 0,1,2 ];
        const rows = [ [[1,1]], [[1,1]], [[1,1]] ];
        const sk   = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );

        const pose = arm.newPose();
        pose.rotLocal( 'root', 30, 'z' );
        pose.rotLocal( 'child', 25, 'x' );
        const out = sk.deform( pose );

        // manual chain: W1 = ( qz30, (0,0,0) ) * ( qx25, (0,1,0) )
        const qz = quat.setAxisAngle( [0,0,0,0], [0,0,1], 30 * Math.PI/180 );
        const qx = quat.setAxisAngle( [0,0,0,0], [1,0,0], 25 * Math.PI/180 );
        const qW = quat.mul( [0,0,0,0], qz, qx );
        const pW = vec3.transformQuat( [0,0,0], [0,1,0], qz );
        // T1 = W1 * B1^-1 with B1 = ( identity, (0,1,0) )
        const t1 = vec3.sub( [0,0,0], pW, vec3.transformQuat( [0,0,0], [0,1,0], qW ) );
        for( let v=0; v < 3; v++ ){
            const p = [ pos[v*3], pos[v*3+1], pos[v*3+2] ];
            const e = vec3.transformQuat( [0,0,0], p, qW );
            vec3.add( e, e, t1 );
            check( vecEq( [ out[v*3], out[v*3+1], out[v*3+2] ], e, 1e-5 ), `vertex ${v}: single bone influence is exactly rigid` );
        }
    }

    // -- a rigid motion shared by all bones transforms the whole result
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );

        const pose = arm.newPose();
        pose.rotLocal( 'child', 65, 'x' );
        pose.rotLocal( 'root', -20, 'z' );
        const base = sk.deform( pose );

        const axis = vec3.normalize( [0,0,0], [ 0.3, 0.8, 0.5 ] );
        const qOff = quat.setAxisAngle( [0,0,0,0], axis, 0.7 );
        const tOff: vec3 = [ 1.5, -0.5, 2.0 ];
        const pose2 = arm.newPose();
        pose2.rotLocal( 'child', 65, 'x' );
        pose2.rotLocal( 'root', -20, 'z' );
        quat.copy( pose2.offset.rot, qOff );
        vec3.copy( pose2.offset.pos, tOff );
        const moved = sk.deform( pose2 );

        for( let v=0; v < 4; v++ ){
            const e = vec3.transformQuat( [0,0,0], [ base[v*3], base[v*3+1], base[v*3+2] ], qOff );
            vec3.add( e, e, tOff );
            check( vecEq( [ moved[v*3], moved[v*3+1], moved[v*3+2] ], e, 1e-4 ), `vertex ${v}: global rigid motion is covariant` );
        }
    }

    // -- seam duplicates (same position, same weights) deform identically
    {
        const arm  = twoBoneArm();
        const pos  = [ 0,0.8,0, 0.4,0.8,0, 0.4,1.2,0, 0,1.2,0,  0.4,1.2,0,  0.4,1.2,0 ];
        const idx  = [ 0,1,2, 0,2,3 ];
        const r    = [[0,0.5],[1,0.5]];
        const rows = [ r, r, r, r, r, r ];
        const sk   = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );
        const pose = arm.newPose();
        pose.rotLocal( 'child', 90, 'x' );
        pose.rotLocal( 'root', 20, 'z' );
        const out = sk.deform( pose );
        check( vecEq( [ out[6],out[7],out[8] ], [ out[12],out[13],out[14] ], 1e-7 ), 'seam copy A matches the original' );
        check( vecEq( [ out[6],out[7],out[8] ], [ out[15],out[16],out[17] ], 1e-7 ), 'seam copy B matches the original' );
    }

    // -- flipping a bone quaternion's sign must not move any vertex
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );

        const axis  = vec3.normalize( [0,0,0], [ 1, 2, 3 ] );
        const q     = quat.setAxisAngle( [0,0,0,0], axis, 1.1 );
        const poseA = arm.newPose();
        poseA.setLocalRot( 'child', q );
        const outA  = sk.deform( poseA );

        const poseB = arm.newPose();
        poseB.setLocalRot( 'child', [ -q[0], -q[1], -q[2], -q[3] ] );
        const outB  = sk.deform( poseB );
        check( vecEq( outA, outB, 1e-5 ), 'equivalent quaternion sign gives identical positions' );
    }

    // -- non-rigid input is rejected explicitly
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );

        const scaled = arm.newPose();
        scaled.sclLocal( 'child', 1.5 );
        check( throws( () => sk.deform( scaled ) ), 'non-unit bone scale rejected' );

        const scaledOff = arm.newPose();
        vec3.set( scaledOff.offset.scl, 1, 2, 1 );
        check( throws( () => sk.deform( scaledOff ) ), 'non-unit offset scale rejected' );

        const armScaled = twoBoneArm();
        armScaled.bones[1].setLocal( undefined, undefined, [ 2, 2, 2 ] );
        check( throws( () => new CoRSkinner().bind( armScaled, pos, idx, rows ) ), 'non-unit bind scale rejected' );
    }
} );
//#endregion

//#region PIPELINE
group( 'pipeline', () => {

    // -- full parent chain with genuinely non-identity inverse binds
    {
        const arm    = new Armature();
        const qHips  = quat.setAxisAngle( [0,0,0,0], [0,0,1],  0.6 );
        const qSpine = quat.setAxisAngle( [0,0,0,0], [1,0,0], -0.4 );
        const qHand  = quat.setAxisAngle( [0,0,0,0], [0,1,0],  0.9 );
        arm.addBone( 'hips',  -1, qHips,  [ 0.5, -0.25, 0.3 ] );
        arm.addBone( 'spine',  0, qSpine, [ 0.0,  0.8,  0.1 ] );
        arm.addBone( 'hand',   1, qHand,  [ 0.1,  0.7,  0.0 ] );

        // sanity: the leaf's bind world transform is far from identity, so the
        // inverse-bind matrices are genuinely non-trivial
        const bindWorld = chainWorlds( [
            { pidx: -1, rot: qHips,  pos: [ 0.5, -0.25, 0.3 ] },
            { pidx:  0, rot: qSpine, pos: [ 0.0,  0.8,  0.1 ] },
            { pidx:  1, rot: qHand,  pos: [ 0.1,  0.7,  0.0 ] },
        ] );
        check( Math.abs( bindWorld[2].rot[3] ) < 0.95, 'inverse-bind rotation is non-identity' );
        check( vec3.len( bindWorld[2].pos ) > 0.5, 'inverse-bind translation is non-zero' );

        const pos = [
            0.6, 0.5, 0.4,   0.4, 0.6, 0.2,   0.7, 0.9, 0.5,
            0.5, 1.2, 0.3,   0.8, 1.4, 0.4,   0.6, 1.6, 0.6,
        ];
        const idx    = [ 0,1,2, 2,3,0, 2,1,4, 4,5,2 ];
        const rowsIn = [
            [[0,1]],
            [[0,0.6],[1,0.4]],
            [[1,0.5],[2,0.5]],
            [[0,0.3],[1,0.3],[2,0.4]],
            [[1,1]],
            [[1,0.3],[2,0.7]],
        ];
        const sk  = new CoRSkinner();
        const res = sk.bind( arm, pos, idx, rowsIn, { sigma: 0.25 } );
        check( res.ok, 'pipeline bind completes' );

        const pose = arm.newPose();
        pose.rotLocal( 'spine', 50, 'z' );
        pose.rotLocal( 'hand', -35, 'x' );
        const qOff = quat.setAxisAngle( [0,0,0,0], [0,1,0], 0.35 );
        const tOff: vec3 = [ 0.2, 0.4, -0.6 ];
        quat.copy( pose.offset.rot, qOff );
        vec3.copy( pose.offset.pos, tOff );

        const out = sk.deform( pose );

        // independent expectation: compose chains by hand, then apply
        // out = R_i (p - c) + sum_j w_ij (R_j c + t_j)
        const poseWorld = chainWorlds(
            pose.bones.map( b => ({ pidx: b.pidx, rot: b.local.rot, pos: b.local.pos }) ),
            { rot: qOff, pos: tOff },
        );
        const cen   = sk.getCenters()!;
        const wrows = sk.getWeightRows()!;
        const R: Array< quat > = [], T: Array< vec3 > = [];
        for( let j=0; j < 3; j++ ){
            const Rj = quat.mul( [0,0,0,0], quat.normalize( [0,0,0,0], poseWorld[j].rot ), quat.invert( [0,0,0,0], bindWorld[j].rot ) );
            quat.normalize( Rj, Rj );
            const tj = vec3.sub( [0,0,0], poseWorld[j].pos, vec3.transformQuat( [0,0,0], bindWorld[j].pos, Rj ) );
            R.push( Rj ); T.push( tj );
        }
        for( let v=0; v < 6; v++ ){
            const row = wrows[v];
            let ref = 0;
            for( let m=1; m < row.length; m++ ) if( row[m][1] > row[ref][1] ) ref = m;
            const qref = R[ row[ref][0] ];
            const qi: quat = [ 0,0,0,0 ];
            for( const [ bi, w ] of row ){
                const q = R[bi];
                const s = quat.dot( q, qref ) < 0 ? -1 : 1;
                qi[0] += w*s*q[0]; qi[1] += w*s*q[1]; qi[2] += w*s*q[2]; qi[3] += w*s*q[3];
            }
            quat.normalize( qi, qi );
            const c   = [ cen[v*3], cen[v*3+1], cen[v*3+2] ];
            const acc = [ 0,0,0 ];
            for( const [ bi, w ] of row ){
                const rc = vec3.transformQuat( [0,0,0], c, R[bi] );
                vec3.add( rc, rc, T[bi] );
                vec3.scaleAndAdd( acc, acc, rc, w );
            }
            const d = vec3.sub( [0,0,0], [ pos[v*3], pos[v*3+1], pos[v*3+2] ], c );
            const e = vec3.transformQuat( [0,0,0], d, qi );
            vec3.add( e, e, acc );
            check( vecEq( [ out[v*3], out[v*3+1], out[v*3+2] ], e, 1e-4 ), `pipeline vertex ${v} matches manual chain computation` );
        }
    }

    // -- outputs, poses and armatures stay independent across calls and instances
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const skA = new CoRSkinner();
        const skB = new CoRSkinner();
        check( skA.bind( arm, pos, idx, rows ).ok && skB.bind( arm, pos, idx, rows ).ok, 'both instances bind' );

        const poseA = arm.newPose(); poseA.rotLocal( 'child', 45, 'x' );
        const poseB = arm.newPose(); poseB.rotLocal( 'child', -60, 'z' );

        const a1 = skA.deform( poseA );
        const a1Copy = Array.from( a1 );
        const b1 = skB.deform( poseB );
        const a2 = skA.deform( poseB );   // different pose, same skinner
        const a3 = skA.deform( poseA );   // out-of-order resampling
        check( vecEq( a1, a1Copy, 0 ), 'previous output arrays are never overwritten' );
        check( vecEq( a1, a3, 1e-7 ), 're-sampling a pose is deterministic' );
        check( !vecEq( a1, a2, 1e-3 ), 'different poses give different results' );
        check( !vecEq( a1, b1, 1e-3 ), 'instances are independent' );

        // armature and pose remain readable / unmodified
        check( vecEq( arm.bones[1].local.rot, [0,0,0,1], 1e-7 ), 'armature rest pose untouched' );
        const qA = quat.setAxisAngle( [0,0,0,0], [1,0,0], 45 * Math.PI/180 );
        check( vecEq( poseA.bones[1].local.rot, qA, 1e-5 ), 'pose local rotation untouched' );
    }

    // -- a failed re-bind (error or incomplete) never breaks the previous binding
    {
        const arm = twoBoneArm();
        const { pos, idx, rows } = quadMesh();
        const sk = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'initial bind' );
        const pose = arm.newPose(); pose.rotLocal( 'child', 120, 'y' );
        const before = sk.deform( pose );

        check( throws( () => sk.bind( arm, pos, idx, [ [[0,-1]], [[0,1]], [[0,1]], [[0,1]] ] ) ), 'invalid re-bind throws' );
        check( !sk.bind( arm, pos, idx, rows, { maxIntegrals: 5 } ).ok, 'underfunded re-bind reports incomplete' );
        const after = sk.deform( pose );
        check( vecEq( before, after, 0 ), 'old binding still deforms identically' );
    }

    // -- bound data is owned by the skinner, not borrowed from the caller
    {
        const arm  = twoBoneArm();
        const pos  = [ 0,0.8,0, 0.4,0.8,0, 0.4,1.2,0, 0,1.2,0 ];
        const idx  = [ 0,1,2, 0,2,3 ];
        const rows = [ [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]], [[0,0.5],[1,0.5]] ];
        const sk   = new CoRSkinner();
        check( sk.bind( arm, pos, idx, rows ).ok, 'bind completes' );
        const pose = arm.newPose(); pose.rotLocal( 'child', 30, 'y' );
        const before = sk.deform( pose );
        pos[0] = 99; ( rows[0][0] as number[] )[1] = 99; idx[0] = 2;
        const after = sk.deform( pose );
        check( vecEq( before, after, 0 ), 'caller arrays are copied, not borrowed' );
    }
} );
//#endregion
