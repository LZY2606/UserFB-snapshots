//#region IMPORTS
import type Pose        from '../armature/Pose';
import { vec3, quat }   from 'gl-matrix';
//#endregion

export type PoseLayerMode = 'override' | 'additive';

/** Bone name -> 0..1 weight. Unlisted bones inherit from their nearest listed ancestor. */
export interface PoseLayerMask{ [ boneName: string ]: number }

export interface PoseLayer{
    /** Pose holding the layer's local TRS. Its scene offset is ignored. */
    pose      : Pose;
    /** Layer weight, 0 to 1. */
    weight    : number;
    mode      : PoseLayerMode;
    /** Required for additive layers, ignored by override layers. */
    reference?: Pose;
    /** Optional per-bone mask with ancestor inheritance. */
    mask?     : PoseLayerMask;
}

const IDENTITY_ROT : quat = [ 0, 0, 0, 1 ];

class PoseLayers{
    /**
     * Blend ordered pose layers on top of a base pose into an output pose.
     *
     * - override: local TRS is interpolated from the current result toward the
     *   layer's local TRS, rotations use shortest-arc slerp.
     * - additive: position adds the delta from the reference, scale is multiplied
     *   by a factor interpolated from 1 toward (layer scale / reference scale) and
     *   rotation applies the shortest-arc slerp of the local rotation difference,
     *   right multiplied onto the current rotation. A reference is mandatory.
     *
     * Layers are applied in array order, one after another. All inputs are
     * validated before the output is touched, so a thrown error never leaves
     * `out` half updated. `out` may alias `base`, any layer pose or reference.
     */
    static apply( base: Pose, layers: PoseLayer[], out: Pose ): Pose{
        if( !base || !base.arm )
            throw new Error( 'PoseLayers.apply : base pose must be bound to an Armature' );

        const arm = base.arm;
        if( !out || out.arm !== arm )
            throw new Error( 'PoseLayers.apply : output pose must share the base Armature' );
        if( !Array.isArray( layers ) )
            throw new Error( 'PoseLayers.apply : layers must be an array' );

        const bCnt = arm.bones.length;

        //#region VALIDATION (run before anything is written, keeps `out` untouched on error)
        const effMasks : Float32Array[] = new Array( layers.length );

        for( let li=0; li < layers.length; li++ ){
            const layer = layers[ li ];

            if( !layer || !layer.pose )
                throw new Error( `PoseLayers.apply : layer ${li} is missing a pose` );
            if( layer.pose.arm !== arm )
                throw new Error( `PoseLayers.apply : layer ${li} pose comes from a different Armature` );

            if( !Number.isFinite( layer.weight ) || layer.weight < 0 || layer.weight > 1 )
                throw new Error( `PoseLayers.apply : layer ${li} weight must be a finite number between 0 and 1` );

            if( layer.mode !== 'override' && layer.mode !== 'additive' )
                throw new Error( `PoseLayers.apply : layer ${li} mode must be "override" or "additive"` );

            if( layer.mode === 'additive' ){
                if( !layer.reference )
                    throw new Error( `PoseLayers.apply : additive layer ${li} requires a reference pose` );
                if( layer.reference.arm !== arm )
                    throw new Error( `PoseLayers.apply : layer ${li} reference comes from a different Armature` );

                for( let i=0; i < bCnt; i++ ){
                    const scl = layer.reference.bones[ i ].local.scl;
                    for( let k=0; k < 3; k++ ){
                        if( !Number.isFinite( scl[ k ] ) || scl[ k ] === 0 )
                            throw new Error( `PoseLayers.apply : reference scale of bone "${arm.bones[ i ].name}" is zero or non-finite` );
                    }
                }
            }

            // Resolve the mask once: own value wins, otherwise the nearest named
            // ancestor's value is inherited; branches with no named ancestor stay 0.
            const eff = new Float32Array( bCnt );
            if( layer.mask ){
                for( const name in layer.mask ){
                    if( !Object.prototype.hasOwnProperty.call( layer.mask, name ) ) continue;
                    if( arm.names.get( name ) === undefined )
                        throw new Error( `PoseLayers.apply : unknown bone name "${name}" in layer ${li} mask` );

                    const v = layer.mask[ name ];
                    if( !Number.isFinite( v ) || v < 0 || v > 1 )
                        throw new Error( `PoseLayers.apply : mask value of "${name}" in layer ${li} must be a finite number between 0 and 1` );
                }

                for( let i=0; i < bCnt; i++ ){
                    const own = layer.mask[ arm.bones[ i ].name ];
                    if( own !== undefined )                 eff[ i ] = own;
                    else if( arm.bones[ i ].pidx === -1 )   eff[ i ] = 0;
                    else                                    eff[ i ] = eff[ arm.bones[ i ].pidx ];
                }
            }else{
                eff.fill( 1 );
            }
            effMasks[ li ] = eff;
        }
        //#endregion

        //#region SNAPSHOTS
        // Work on flat scratch data so `out` may alias any input pose without
        // corrupting reads, and no input pose other than `out` gets mutated.
        const curP = new Float32Array( bCnt * 3 );
        const curR = new Float32Array( bCnt * 4 );
        const curS = new Float32Array( bCnt * 3 );
        for( let i=0; i < bCnt; i++ ){
            const loc = base.bones[ i ].local;
            curP.set( loc.pos, i * 3 );
            curR.set( loc.rot, i * 4 );
            curS.set( loc.scl, i * 3 );
        }

        const layP = new Float32Array( bCnt * 3 );
        const layR = new Float32Array( bCnt * 4 );
        const layS = new Float32Array( bCnt * 3 );
        const refP = new Float32Array( bCnt * 3 );
        const refR = new Float32Array( bCnt * 4 );
        const refS = new Float32Array( bCnt * 3 );

        const vA = vec3.create();
        const qA = quat.create();
        const qB = quat.create();
        const qC = quat.create();
        //#endregion

        //#region LAYER BLENDING (ordered, each layer sees the previous layer's result)
        for( let li=0; li < layers.length; li++ ){
            const layer = layers[ li ];
            const eff   = effMasks[ li ];

            for( let i=0; i < bCnt; i++ ){
                layP.set( layer.pose.bones[ i ].local.pos, i * 3 );
                layR.set( layer.pose.bones[ i ].local.rot, i * 4 );
                layS.set( layer.pose.bones[ i ].local.scl, i * 3 );
            }

            if( layer.mode === 'additive' && layer.reference ){
                for( let i=0; i < bCnt; i++ ){
                    refP.set( layer.reference.bones[ i ].local.pos, i * 3 );
                    refR.set( layer.reference.bones[ i ].local.rot, i * 4 );
                    refS.set( layer.reference.bones[ i ].local.scl, i * 3 );
                }
            }

            for( let i=0; i < bCnt; i++ ){
                const w = layer.weight * eff[ i ];
                if( w === 0 ) continue;

                const p3 = i * 3, q4 = i * 4;

                if( layer.mode === 'override' ){
                    // Position / scale: straight linear interpolation.
                    vec3.lerp( vA, curP.subarray( p3, p3 + 3 ), layP.subarray( p3, p3 + 3 ), w );
                    curP.set( vA, p3 );
                    vec3.lerp( vA, curS.subarray( p3, p3 + 3 ), layS.subarray( p3, p3 + 3 ), w );
                    curS.set( vA, p3 );

                    // Rotation: shortest-arc spherical interpolation.
                    quat.slerp( qA, curR.subarray( q4, q4 + 4 ), layR.subarray( q4, q4 + 4 ), w );
                    curR.set( qA, q4 );
                }else{
                    // Additive position: current + weight * (layer - reference).
                    for( let k=0; k < 3; k++ ){
                        curP[ p3 + k ] += w * ( layP[ p3 + k ] - refP[ p3 + k ] );

                        // Scale factor interpolates from 1 toward (layer / reference).
                        const ratio = layS[ p3 + k ] / refS[ p3 + k ];
                        curS[ p3 + k ] *= 1 + w * ( ratio - 1 );
                    }

                    // Rotation difference in local space: inverse(reference) * layer.
                    quat.invert( qA, refR.subarray( q4, q4 + 4 ) );
                    quat.mul( qB, qA, layR.subarray( q4, q4 + 4 ) );

                    // Walk the shortest arc away from identity, then right multiply
                    // onto the current local rotation.
                    quat.slerp( qC, IDENTITY_ROT, qB, w );
                    quat.mul( qA, curR.subarray( q4, q4 + 4 ), qC );
                    curR.set( qA, q4 );
                }
            }
        }
        //#endregion

        //#region COMMIT (only now does `out` get modified)
        for( let i=0; i < bCnt; i++ ){
            const loc = out.bones[ i ].local;
            vec3.copy( loc.pos, curP.subarray( i * 3, i * 3 + 3 ) );
            quat.copy( loc.rot, curR.subarray( i * 4, i * 4 + 4 ) );
            vec3.copy( loc.scl, curS.subarray( i * 3, i * 3 + 3 ) );
        }

        // Scene offsets never participate in local deltas; keep the base offset.
        out.offset.copy( base.offset );
        out.updateWorld();
        //#endregion

        return out;
    }
}

export default PoseLayers;
export { PoseLayers };
