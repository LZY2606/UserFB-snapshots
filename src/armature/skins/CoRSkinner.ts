//#region IMPORTS
import type Armature    from '../Armature';
import type Pose        from '../Pose';
import { vec3, quat }   from 'gl-matrix';
//#endregion

//#region PUBLIC TYPES
/**
 * CoRSkinner — CPU skinning around surface-estimated Centers of Rotation.
 *
 * Public API
 * ----------
 * bind( arm, positions, indices, weights, opts? ) : CoRBindResult
 *   - arm        : Armature, used only through its bones' local rest transforms
 *                  (full parent chain). All bone local scales must be unit.
 *   - positions  : ArrayLike<number>, xyz per vertex, length 3*n (model space).
 *   - indices    : ArrayLike<number>, triangle vertex indices, multiple of 3.
 *                  The mesh may be open and may have several components, but
 *                  zero-area triangles are rejected. Isolated vertices are fine.
 *   - weights    : one row per vertex; each row is a list of [ boneIndex, weight ]
 *                  pairs (sparse). Weights must be finite and >= 0; duplicate
 *                  bones are merged (summed) and every row is normalized to sum
 *                  to 1. Every vertex needs at least one positive influence.
 *   - opts.sigma        : positive kernel width (default 0.2).
 *   - opts.relTol       : relative integration tolerance (default 1e-6).
 *   - opts.absTol       : absolute integration tolerance (default 1e-9).
 *   - opts.maxIntegrals : refinement budget, max number of integrand
 *                         evaluations for the whole bind (default 1_000_000).
 *   Returns { ok, integrals, errDenom, errMoment }:
 *   - ok        : completion status. false means the budget ran out before the
 *                 requested tolerance was met; the result is then NOT committed
 *                 and any previous valid binding stays in place.
 *   - integrals : actual number of integrand evaluations used.
 *   - errDenom  : max over vertices of the estimated absolute error of the
 *                 denominator integral ∫ S dA.
 *   - errMoment : max over vertices/components of the estimated absolute error
 *                 of the first-moment integral ∫ S p dA.
 *
 * deform( pose, out? ) : Float32Array
 *   Deforms the bound vertices with the given pose and returns an independent
 *   position array (a fresh one unless `out` is supplied). The armature, the
 *   pose and previously returned arrays are never modified.
 *
 * getCenters() / getWeightRows() return copies of the bound data; the object
 * owns its binding data and never borrows caller arrays.
 *
 * Errors
 * ------
 * TypeError   : malformed arguments (wrong shapes, non-finite weights).
 * RangeError  : invalid values — negative weights, empty weight rows, bone
 *               index out of range, zero-area triangles, non-positive sigma /
 *               tolerances, non-unit scale in the bind pose or in the pose
 *               passed to deform (non-rigid input is refused explicitly, scale
 *               is never silently dropped).
 * Error       : state errors — deform() before a successful bind, or a pose
 *               whose bone count does not match the bound armature.
 * A bind() that throws or returns ok:false leaves the previous binding intact.
 */

/** Sparse influence of one bone on one vertex: [ boneIndex, weight ]. */
export type CoRWeightInfluence = ArrayLike< number >;

/** Per-vertex sparse weights; see CoRSkinner.bind(). */
export type CoRWeights = ReadonlyArray< ReadonlyArray< CoRWeightInfluence > >;

/** Options accepted by CoRSkinner.bind(). */
export type CoRBindOptions = {
    sigma        ?: number;   // kernel width, > 0, default 0.2
    relTol       ?: number;   // relative integration tolerance, > 0, default 1e-6
    absTol       ?: number;   // absolute integration tolerance, > 0, default 1e-9
    maxIntegrals ?: number;   // refinement budget in integrand evaluations, default 1_000_000
};

/** Report returned by CoRSkinner.bind(). */
export type CoRBindResult = {
    ok        : boolean;   // completion status
    integrals : number;    // actual number of integrand evaluations
    errDenom  : number;    // max estimated abs. error of ∫ S dA over all vertices
    errMoment : number;    // max estimated abs. error of ∫ S p dA (any component)
};
//#endregion

//#region QUADRATURE RULES
// Embedded pair of triangle quadrature rules in barycentric coordinates.
// Weights are normalized to sum to 1; integrals are scaled by the true 3D
// triangle area. The difference of the two rules is the per-triangle error
// estimate that drives the deterministic 1 -> 4 adaptive refinement.
//   RULE_HI : 7 points, exact for degree 5 (Dunavant)
//   RULE_LO : 4 points, exact for degree 2 (vertices + centroid); sampling the
//             vertices keeps the estimate honest when the similarity kernel is
//             sharply peaked at a corner (its weight row matches the vertex).
const SQ15  = Math.sqrt( 15 );
const HI_A  = ( 6 - SQ15 ) / 21;
const HI_B  = ( 9 + 2 * SQ15 ) / 21;
const HI_C  = ( 6 + SQ15 ) / 21;
const HI_D  = ( 9 - 2 * SQ15 ) / 21;
const HI_W1 = ( 155 - SQ15 ) / 1200;
const HI_W2 = ( 155 + SQ15 ) / 1200;

// [ lambda0, lambda1, lambda2, weight ]
const RULE_HI: ReadonlyArray< readonly [ number, number, number, number ] > = [
    [ 1/3,  1/3,  1/3,  0.225 ],
    [ HI_A, HI_A, HI_B, HI_W1 ],
    [ HI_A, HI_B, HI_A, HI_W1 ],
    [ HI_B, HI_A, HI_A, HI_W1 ],
    [ HI_C, HI_C, HI_D, HI_W2 ],
    [ HI_C, HI_D, HI_C, HI_W2 ],
    [ HI_D, HI_C, HI_C, HI_W2 ],
];

const RULE_LO: ReadonlyArray< readonly [ number, number, number, number ] > = [
    [ 1.0, 0.0, 0.0, 1/12 ],
    [ 0.0, 1.0, 0.0, 1/12 ],
    [ 0.0, 0.0, 1.0, 1/12 ],
    [ 1/3, 1/3, 1/3, 0.75  ],
];

const EVALS_PER_TRI = RULE_HI.length + RULE_LO.length; // integrand evaluations per triangle
//#endregion

//#region INTEGRATION
// One (sub-)triangle of the adaptive integration. Refinement only serves
// numerical quadrature; it never changes the input mesh topology.
type TTriWork = {
    pos  : Float64Array;   // 9: xyz of the 3 corners
    wts  : Float64Array;   // nBones*3: weight of each bone at each corner (0 if absent)
    area : number;         // true 3D area
    den  : number;         // ∫ S dA estimate
    mx   : number;         // ∫ S p dA estimate
    my   : number;
    mz   : number;
    errD : number;         // error estimate for den
    errM : number;         // error estimate for (mx,my,mz), max component
};

// Evaluate S(u,w) = sum_{a<b} u_a u_b w_a w_b exp( -(u_a w_b - u_b w_a)^2 / sigma^2 )
// and its first moment with one quadrature rule over one triangle.
function sampleRule(
    rule   : ReadonlyArray< readonly [ number, number, number, number ] >,
    tri    : TTriWork,
    uVal   : Float64Array,
    invSig2: number,
    wa     : Float64Array,   // scratch, length = uVal.length
    acc    : Float64Array,   // out: [ S, Sx, Sy, Sz ]
): void {
    const nb = uVal.length;
    let sD = 0, sX = 0, sY = 0, sZ = 0;

    for( let q=0; q < rule.length; q++ ){
        const pt = rule[ q ];
        const l0 = pt[0], l1 = pt[1], l2 = pt[2], wq = pt[3];

        // Interpolate the full weight function w at the sample point.
        // All triangles share one bone index space; missing influences are zero.
        for( let a=0; a < nb; a++ ){
            wa[ a ] = l0 * tri.wts[ a*3 ] + l1 * tri.wts[ a*3+1 ] + l2 * tri.wts[ a*3+2 ];
        }

        let s = 0;
        for( let a=0; a < nb; a++ ){
            const ua = uVal[ a ];
            const Wa = wa[ a ];
            if( Wa === 0 ) continue;
            for( let b=a+1; b < nb; b++ ){
                const Wb = wa[ b ];
                if( Wb === 0 ) continue;
                const d  = ua * Wb - uVal[ b ] * Wa;
                s += ua * uVal[ b ] * Wa * Wb * Math.exp( -d * d * invSig2 );
            }
        }

        const px = l0 * tri.pos[0] + l1 * tri.pos[3] + l2 * tri.pos[6];
        const py = l0 * tri.pos[1] + l1 * tri.pos[4] + l2 * tri.pos[7];
        const pz = l0 * tri.pos[2] + l1 * tri.pos[5] + l2 * tri.pos[8];

        sD += wq * s;
        sX += wq * s * px;
        sY += wq * s * py;
        sZ += wq * s * pz;
    }

    acc[0] = sD; acc[1] = sX; acc[2] = sY; acc[3] = sZ;
}

function evalTri(
    tri    : TTriWork,
    uVal   : Float64Array,
    invSig2: number,
    wa     : Float64Array,
    hi     : Float64Array,
    lo     : Float64Array,
): void {
    sampleRule( RULE_HI, tri, uVal, invSig2, wa, hi );
    sampleRule( RULE_LO, tri, uVal, invSig2, wa, lo );

    const a  = tri.area;
    tri.den  = a * hi[0];
    tri.mx   = a * hi[1];
    tri.my   = a * hi[2];
    tri.mz   = a * hi[3];
    tri.errD = a * Math.abs( hi[0] - lo[0] );
    tri.errM = a * Math.max(
        Math.abs( hi[1] - lo[1] ),
        Math.abs( hi[2] - lo[2] ),
        Math.abs( hi[3] - lo[3] ),
    );
}

// Split one triangle into 4 similar ones through the edge midpoints. Weights
// are linear on the triangle, so midpoint weights are corner averages.
function subdivideTri( tri: TTriWork, nb: number ): Array< TTriWork > {
    const p = tri.pos;
    const m01x = 0.5*(p[0]+p[3]), m01y = 0.5*(p[1]+p[4]), m01z = 0.5*(p[2]+p[5]);
    const m12x = 0.5*(p[3]+p[6]), m12y = 0.5*(p[4]+p[7]), m12z = 0.5*(p[5]+p[8]);
    const m20x = 0.5*(p[6]+p[0]), m20y = 0.5*(p[7]+p[1]), m20z = 0.5*(p[8]+p[2]);

    const w0  = new Float64Array( nb );
    const w1  = new Float64Array( nb );
    const w2  = new Float64Array( nb );
    const w01 = new Float64Array( nb );
    const w12 = new Float64Array( nb );
    const w20 = new Float64Array( nb );
    for( let a=0; a < nb; a++ ){
        w0[a]  = tri.wts[ a*3 ];
        w1[a]  = tri.wts[ a*3+1 ];
        w2[a]  = tri.wts[ a*3+2 ];
        w01[a] = 0.5 * ( w0[a] + w1[a] );
        w12[a] = 0.5 * ( w1[a] + w2[a] );
        w20[a] = 0.5 * ( w2[a] + w0[a] );
    }

    const area = tri.area / 4;
    const mk = ( pos: Float64Array, ca: Float64Array, cb: Float64Array, cc: Float64Array ): TTriWork => {
        const wts = new Float64Array( nb * 3 );
        for( let a=0; a < nb; a++ ){
            wts[ a*3 ]   = ca[ a ];
            wts[ a*3+1 ] = cb[ a ];
            wts[ a*3+2 ] = cc[ a ];
        }
        return { pos, wts, area, den:0, mx:0, my:0, mz:0, errD:0, errM:0 };
    };

    return [
        mk( new Float64Array( [ p[0],p[1],p[2],  m01x,m01y,m01z,  m20x,m20y,m20z ] ), w0,  w01, w20 ),
        mk( new Float64Array( [ m01x,m01y,m01z,  p[3],p[4],p[5],  m12x,m12y,m12z ] ), w01, w1,  w12 ),
        mk( new Float64Array( [ m20x,m20y,m20z,  m12x,m12y,m12z,  p[6],p[7],p[8] ] ), w20, w12, w2  ),
        mk( new Float64Array( [ m01x,m01y,m01z,  m12x,m12y,m12z,  m20x,m20y,m20z ] ), w01, w12, w20 ),
    ];
}
//#endregion

//#region HELPERS
const SCALE_EPS = 1e-6;

function isUnitScale( s: ArrayLike< number > ): boolean {
    return Number.isFinite( s[0] ) && Number.isFinite( s[1] ) && Number.isFinite( s[2] )
        && Math.abs( s[0] - 1 ) <= SCALE_EPS
        && Math.abs( s[1] - 1 ) <= SCALE_EPS
        && Math.abs( s[2] - 1 ) <= SCALE_EPS;
}

function isFiniteVec( v: ArrayLike< number >, n: number ): boolean {
    for( let i=0; i < n; i++ ) if( !Number.isFinite( v[i] ) ) return false;
    return true;
}
//#endregion

class CoRSkinner {
    //#region BIND DATA (all owned by this object)
    _boneCount   : number = 0;
    _vertexCount : number = 0;
    _positions   : Float64Array | null = null;   // owned copy of the bind positions
    _centers     : Float64Array | null = null;   // owned per-vertex rotation centers
    _rows        : Array< { bones: Int32Array, weights: Float64Array } > | null = null;
    _bindRot     : Array< quat > | null = null;  // bind-pose world rotations (unit)
    _bindPos     : Array< vec3 > | null = null;  // bind-pose world positions
    _sigma       : number = 0.2;
    //#endregion

    //#region ACCESSORS
    isBound(): boolean{ return this._rows !== null; }
    getVertexCount(): number{ return this._vertexCount; }
    getBoneCount(): number{ return this._boneCount; }

    /** Copy of the per-vertex rotation centers (xyz per vertex), or null. */
    getCenters(): Float64Array | null{
        return this._centers ? new Float64Array( this._centers ) : null;
    }

    /** Copy of the normalized per-vertex weight rows: [ boneIndex, weight ][]. */
    getWeightRows(): Array< Array< [ number, number ] > > | null{
        if( !this._rows ) return null;
        return this._rows.map( r => {
            const out: Array< [ number, number ] > = [];
            for( let i=0; i < r.bones.length; i++ ) out.push( [ r.bones[i], r.weights[i] ] );
            return out;
        } );
    }
    //#endregion

    //#region BIND
    bind(
        arm       : Armature,
        positions : ArrayLike< number >,
        indices   : ArrayLike< number >,
        weights   : CoRWeights,
        opts      : CoRBindOptions = {},
    ): CoRBindResult {
        //-------------------- options
        const sigma  = opts.sigma        ?? 0.2;
        const relTol = opts.relTol       ?? 1e-6;
        const absTol = opts.absTol       ?? 1e-9;
        const budget = opts.maxIntegrals ?? 1000000;

        if( !Number.isFinite( sigma )  || sigma  <= 0 ) throw new RangeError( 'CoRSkinner.bind: sigma must be a positive finite number' );
        if( !Number.isFinite( relTol ) || relTol <= 0 ) throw new RangeError( 'CoRSkinner.bind: relTol must be a positive finite number' );
        if( !Number.isFinite( absTol ) || absTol <= 0 ) throw new RangeError( 'CoRSkinner.bind: absTol must be a positive finite number' );
        if( !Number.isInteger( budget ) || budget < 0 ) throw new RangeError( 'CoRSkinner.bind: maxIntegrals must be a non-negative integer' );

        //-------------------- armature (rigid bind pose only)
        if( !arm || !Array.isArray( arm.bones ) || arm.bones.length === 0 )
            throw new TypeError( 'CoRSkinner.bind: an armature with at least one bone is required' );
        const bCnt = arm.bones.length;
        for( let i=0; i < bCnt; i++ ){
            const b = arm.bones[ i ];
            if( !isUnitScale( b.local.scl ) )
                throw new RangeError( `CoRSkinner.bind: bone ${i} has non-unit scale; only rigid (unit-scale) binds are supported` );
            if( !isFiniteVec( b.local.rot, 4 ) || !isFiniteVec( b.local.pos, 3 ) )
                throw new RangeError( `CoRSkinner.bind: bone ${i} has non-finite local transform` );
            if( !Number.isInteger( b.pidx ) || b.pidx < -1 || b.pidx >= bCnt || b.pidx === i )
                throw new RangeError( `CoRSkinner.bind: bone ${i} has invalid parent index ${b.pidx}` );
        }

        // Bind-pose world rotation/position per bone (full parent chain,
        // armature offset excluded — the offset only applies at pose time).
        const bindRot: Array< quat > = new Array( bCnt );
        const bindPos: Array< vec3 > = new Array( bCnt );
        const state = new Int8Array( bCnt );   // 0 unvisited, 1 in progress, 2 done
        const computeWorld = ( i: number ): void => {
            if( state[ i ] === 2 ) return;
            if( state[ i ] === 1 ) throw new RangeError( 'CoRSkinner.bind: cycle in the bone hierarchy' );
            state[ i ] = 1;
            const b  = arm.bones[ i ];
            const lr = quat.normalize( [0,0,0,0], b.local.rot );
            if( b.pidx === -1 ){
                bindRot[ i ] = lr;
                bindPos[ i ] = [ b.local.pos[0], b.local.pos[1], b.local.pos[2] ];
            }else{
                computeWorld( b.pidx );
                const pr = bindRot[ b.pidx ];
                const pp = bindPos[ b.pidx ];
                const wp = vec3.transformQuat( [0,0,0], b.local.pos, pr );
                vec3.add( wp, wp, pp );
                bindRot[ i ] = quat.normalize( [0,0,0,0], quat.mul( [0,0,0,0], pr, lr ) );
                bindPos[ i ] = wp;
            }
            state[ i ] = 2;
        };
        for( let i=0; i < bCnt; i++ ) computeWorld( i );

        //-------------------- positions (owned copy)
        if( !positions || positions.length === 0 || positions.length % 3 !== 0 )
            throw new RangeError( 'CoRSkinner.bind: positions length must be a positive multiple of 3' );
        const vCnt  = positions.length / 3;
        const pos64 = new Float64Array( positions.length );
        for( let i=0; i < positions.length; i++ ){
            const v = positions[ i ];
            if( !Number.isFinite( v ) ) throw new RangeError( 'CoRSkinner.bind: positions must be finite numbers' );
            pos64[ i ] = v;
        }

        //-------------------- weight rows (merge duplicates, then normalize)
        if( !weights || weights.length !== vCnt )
            throw new RangeError( 'CoRSkinner.bind: weights must provide exactly one row per vertex' );
        const rows: Array< { bones: Int32Array, weights: Float64Array } > = new Array( vCnt );
        const maps: Array< Map< number, number > > = new Array( vCnt );
        for( let v=0; v < vCnt; v++ ){
            const row = weights[ v ];
            if( !row ) throw new RangeError( `CoRSkinner.bind: missing weight row for vertex ${v}` );
            const map = new Map< number, number >();
            for( let k=0; k < row.length; k++ ){
                const e = row[ k ];
                if( !e || e.length < 2 ) throw new TypeError( `CoRSkinner.bind: invalid influence at vertex ${v}` );
                const bi = e[0], w = e[1];
                if( !Number.isInteger( bi ) || bi < 0 || bi >= bCnt )
                    throw new RangeError( `CoRSkinner.bind: bone index ${bi} out of range at vertex ${v}` );
                if( typeof w !== 'number' || !Number.isFinite( w ) )
                    throw new TypeError( `CoRSkinner.bind: weight must be a finite number at vertex ${v}` );
                if( w < 0 )
                    throw new RangeError( `CoRSkinner.bind: negative weight ${w} at vertex ${v}` );
                if( w > 0 ) map.set( bi, ( map.get( bi ) ?? 0 ) + w );
            }
            if( map.size === 0 )
                throw new RangeError( `CoRSkinner.bind: vertex ${v} needs at least one positive influence` );
            let sum = 0;
            map.forEach( x => { sum += x; } );
            // Rows are sorted by bone index: deterministic reference-bone tie breaks.
            const keys  = [ ...map.keys() ].sort( ( a, b ) => a - b );
            const bones = new Int32Array( keys.length );
            const wts   = new Float64Array( keys.length );
            const nm    = new Map< number, number >();
            keys.forEach( ( b, i ) => {
                const nw = ( map.get( b ) as number ) / sum;
                bones[ i ] = b;
                wts[ i ]   = nw;
                nm.set( b, nw );
            } );
            rows[ v ] = { bones, weights: wts };
            maps[ v ] = nm;
        }

        //-------------------- triangles (true 3D area, zero area rejected)
        if( !indices || indices.length % 3 !== 0 )
            throw new RangeError( 'CoRSkinner.bind: indices length must be a multiple of 3' );
        const tris: Array< { vtx: [ number, number, number ], pos: Float64Array, area: number } > = [];
        for( let t=0; t < indices.length; t += 3 ){
            const i0 = indices[ t ], i1 = indices[ t+1 ], i2 = indices[ t+2 ];
            for( const ix of [ i0, i1, i2 ] ){
                if( !Number.isInteger( ix ) || ix < 0 || ix >= vCnt )
                    throw new RangeError( `CoRSkinner.bind: vertex index ${ix} out of range` );
            }
            const p = new Float64Array( 9 );
            for( let c=0; c < 3; c++ ){
                const vi = indices[ t + c ];
                p[ c*3 ]   = pos64[ vi*3 ];
                p[ c*3+1 ] = pos64[ vi*3+1 ];
                p[ c*3+2 ] = pos64[ vi*3+2 ];
            }
            const e1x = p[3]-p[0], e1y = p[4]-p[1], e1z = p[5]-p[2];
            const e2x = p[6]-p[0], e2y = p[7]-p[1], e2z = p[8]-p[2];
            const cx  = e1y*e2z - e1z*e2y;
            const cy  = e1z*e2x - e1x*e2z;
            const cz  = e1x*e2y - e1y*e2x;
            const area = 0.5 * Math.sqrt( cx*cx + cy*cy + cz*cz );
            if( !( area > 0 ) )
                throw new RangeError( `CoRSkinner.bind: triangle ${t/3} has zero area` );
            tris.push( { vtx: [ i0, i1, i2 ], pos: p, area } );
        }

        //-------------------- per-vertex adaptive surface integration
        const invSig2  = 1 / ( sigma * sigma );
        const centers  = new Float64Array( vCnt * 3 );
        let   used     = 0;   // integrand evaluations actually performed
        let   maxErrD  = 0;
        let   maxErrM  = 0;
        let   complete = true;
        const hi = new Float64Array( 4 );
        const lo = new Float64Array( 4 );

        for( let v=0; v < vCnt && complete; v++ ){
            const row = rows[ v ];
            const nb  = row.bones.length;

            // S == 0 everywhere for single-influence vertices -> center is the vertex
            if( nb < 2 ){
                centers[ v*3 ]   = pos64[ v*3 ];
                centers[ v*3+1 ] = pos64[ v*3+1 ];
                centers[ v*3+2 ] = pos64[ v*3+2 ];
                continue;
            }

            // Work list: only triangles where at least two of the vertex's
            // bones are present can contribute (S == 0 everywhere else).
            const work: Array< TTriWork > = [];
            for( const tri of tris ){
                const wts = new Float64Array( nb * 3 );
                let present = 0;
                for( let a=0; a < nb; a++ ){
                    const bIdx = row.bones[ a ];
                    let any = false;
                    for( let c=0; c < 3; c++ ){
                        const w = maps[ tri.vtx[ c ] ].get( bIdx ) ?? 0;
                        wts[ a*3 + c ] = w;
                        if( w > 0 ) any = true;
                    }
                    if( any ) present++;
                }
                if( present < 2 ) continue;
                work.push( { pos: tri.pos, wts, area: tri.area, den:0, mx:0, my:0, mz:0, errD:0, errM:0 } );
            }

            if( work.length === 0 ){
                centers[ v*3 ]   = pos64[ v*3 ];
                centers[ v*3+1 ] = pos64[ v*3+1 ];
                centers[ v*3+2 ] = pos64[ v*3+2 ];
                continue;
            }

            const wa = new Float64Array( nb );

            if( used + EVALS_PER_TRI * work.length > budget ){ complete = false; break; }
            let D = 0, Mx = 0, My = 0, Mz = 0, eD = 0, eM = 0;
            for( const t of work ){
                evalTri( t, row.weights, invSig2, wa, hi, lo );
                D += t.den; Mx += t.mx; My += t.my; Mz += t.mz;
                eD += t.errD; eM += t.errM;
            }
            used += EVALS_PER_TRI * work.length;

            // Deterministic greedy refinement: always split the triangle with
            // the largest combined error estimate (lowest index on ties).
            for(;;){
                const mMag = Math.max( Math.abs( Mx ), Math.abs( My ), Math.abs( Mz ) );
                if( eD <= absTol + relTol * Math.abs( D ) && eM <= absTol + relTol * mMag ) break;
                if( used + 4 * EVALS_PER_TRI > budget ){ complete = false; break; }

                let wi = 0, we = -1;
                for( let i=0; i < work.length; i++ ){
                    const e = work[ i ].errD + work[ i ].errM;
                    if( e > we ){ we = e; wi = i; }
                }

                const parent = work[ wi ];
                const kids   = subdivideTri( parent, nb );
                for( const k of kids ) evalTri( k, row.weights, invSig2, wa, hi, lo );
                used += 4 * EVALS_PER_TRI;

                D  -= parent.den; Mx -= parent.mx; My -= parent.my; Mz -= parent.mz;
                eD -= parent.errD; eM -= parent.errM;
                for( const k of kids ){
                    D += k.den; Mx += k.mx; My += k.my; Mz += k.mz;
                    eD += k.errD; eM += k.errM;
                }
                work.splice( wi, 1, ...kids );
            }

            if( !complete ) break;

            if( D > 0 ){
                centers[ v*3 ]   = Mx / D;
                centers[ v*3+1 ] = My / D;
                centers[ v*3+2 ] = Mz / D;
            }else{
                centers[ v*3 ]   = pos64[ v*3 ];
                centers[ v*3+1 ] = pos64[ v*3+1 ];
                centers[ v*3+2 ] = pos64[ v*3+2 ];
            }
            if( eD > maxErrD ) maxErrD = eD;
            if( eM > maxErrM ) maxErrM = eM;
        }

        if( !complete ){
            // Honest failure: the previous binding (if any) stays in place.
            return { ok: false, integrals: used, errDenom: maxErrD, errMoment: maxErrM };
        }

        //-------------------- commit: everything below is owned by this object
        this._boneCount   = bCnt;
        this._vertexCount = vCnt;
        this._positions   = pos64;
        this._centers     = centers;
        this._rows        = rows;
        this._bindRot     = bindRot;
        this._bindPos     = bindPos;
        this._sigma       = sigma;

        return { ok: true, integrals: used, errDenom: maxErrD, errMoment: maxErrM };
    }
    //#endregion

    //#region DEFORM
    deform( pose: Pose, out ?: Float32Array ): Float32Array {
        if( this._rows === null || this._centers === null || this._positions === null || this._bindRot === null || this._bindPos === null )
            throw new Error( 'CoRSkinner.deform: no valid binding, call bind() first' );
        if( !pose || !Array.isArray( pose.bones ) || pose.bones.length !== this._boneCount )
            throw new Error( 'CoRSkinner.deform: pose does not match the bound armature' );

        // Rigid poses only: refuse to silently drop any scale.
        if( !isUnitScale( pose.offset.scl ) )
            throw new RangeError( 'CoRSkinner.deform: pose offset has non-unit scale; only rigid poses are supported' );
        for( let i=0; i < pose.bones.length; i++ ){
            const b = pose.bones[ i ];
            if( !isUnitScale( b.local.scl ) )
                throw new RangeError( `CoRSkinner.deform: bone ${i} has non-unit scale; only rigid poses are supported` );
            if( !isFiniteVec( b.local.rot, 4 ) || !isFiniteVec( b.local.pos, 3 ) )
                throw new RangeError( `CoRSkinner.deform: bone ${i} has non-finite local transform` );
        }

        const bCnt = this._boneCount;
        const rot  = new Array< quat >( bCnt );   // R_j
        const tra  = new Array< vec3 >( bCnt );   // t_j

        for( let j=0; j < bCnt; j++ ){
            const wt = pose.getWorldTransform( j );   // full parent chain + pose offset
            const qw = quat.normalize( [0,0,0,0], wt.rot );   // quaternions are unitized first
            const Rj = quat.normalize(
                [0,0,0,0],
                quat.mul( [0,0,0,0], qw, quat.invert( [0,0,0,0], this._bindRot[ j ] ) ),
            );
            const tj = vec3.transformQuat( [0,0,0], this._bindPos[ j ], Rj );
            vec3.sub( tj, wt.pos, tj );   // t_j = w_j - R_j * b_j
            rot[ j ] = Rj;
            tra[ j ] = tj;
        }

        const vCnt = this._vertexCount;
        const pos  = this._positions;
        const cen  = this._centers;
        const rows = this._rows;

        if( out === undefined ) out = new Float32Array( vCnt * 3 );
        else if( out.length !== vCnt * 3 )
            throw new RangeError( `CoRSkinner.deform: out array must have length ${vCnt*3}` );

        const tmp = vec3.create();
        for( let v=0; v < vCnt; v++ ){
            const row = rows[ v ];
            const k   = row.bones.length;

            // Hemisphere alignment reference: largest weight, ties broken by
            // bone index (rows are sorted by bone index, first maximum wins).
            let ref = 0;
            for( let m=1; m < k; m++ ) if( row.weights[ m ] > row.weights[ ref ] ) ref = m;
            const qref = rot[ row.bones[ ref ] ];

            // Weighted rotation blend in the reference hemisphere.
            let qx = 0, qy = 0, qz = 0, qw = 0;
            for( let m=0; m < k; m++ ){
                const q   = rot[ row.bones[ m ] ];
                const sgn = ( quat.dot( q, qref ) < 0 ) ? -1 : 1;
                const w   = row.weights[ m ] * sgn;
                qx += w * q[0]; qy += w * q[1]; qz += w * q[2]; qw += w * q[3];
            }
            const ql = 1 / Math.sqrt( qx*qx + qy*qy + qz*qz + qw*qw );
            qx *= ql; qy *= ql; qz *= ql; qw *= ql;

            const v3 = v * 3;
            const cx = cen[ v3 ], cy = cen[ v3+1 ], cz = cen[ v3+2 ];

            // Transport of the center: plain weighted average of the rigidly
            // transformed centers (no dual-quaternion translation blending,
            // and the center is not treated as a per-bone joint origin).
            let ax = 0, ay = 0, az = 0;
            for( let m=0; m < k; m++ ){
                const bi = row.bones[ m ];
                const w  = row.weights[ m ];
                tmp[0] = cx; tmp[1] = cy; tmp[2] = cz;
                vec3.transformQuat( tmp, tmp, rot[ bi ] );
                ax += w * ( tmp[0] + tra[ bi ][0] );
                ay += w * ( tmp[1] + tra[ bi ][1] );
                az += w * ( tmp[2] + tra[ bi ][2] );
            }

            // Local rotation of the vertex around its own center.
            tmp[0] = pos[ v3 ] - cx; tmp[1] = pos[ v3+1 ] - cy; tmp[2] = pos[ v3+2 ] - cz;
            vec3.transformQuat( tmp, tmp, [ qx, qy, qz, qw ] );
            out[ v3 ]   = tmp[0] + ax;
            out[ v3+1 ] = tmp[1] + ay;
            out[ v3+2 ] = tmp[2] + az;
        }

        return out;
    }
    //#endregion
}

export default CoRSkinner;
