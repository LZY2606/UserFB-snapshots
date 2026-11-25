// *************************************************************************************************
//
// Horde3D
//   Minimal dual-animation root motion blend example (headless, Null render device)
//
// This software is distributed under the terms of the Eclipse Public License v1.0.
//
// *************************************************************************************************

// The engine is a static library in this build; the public C entry points are still available.
#include "Horde3D.h"
#include "egModules.h"
#include "egModel.h"
#include "egAnimation.h"
#include "egRootBlend.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Horde3D;

namespace
{

// Tiny procedural resource writer (H3DA/H3DG are simple binary formats).
struct Bytes
{
	std::vector<char> data;
	void u32( uint32_t v ) { for( int i = 0; i < 4; ++i ) data.push_back( char( v >> ( 8 * i ) ) ); }
	void u16( int v ) { data.push_back( char( v ) ); data.push_back( char( v >> 8 ) ); }
	void u8( int v ) { data.push_back( char( v ) ); }
	void f32( float v ) { uint32_t b; std::memcpy( &b, &v, 4 ); u32( b ); }
	void text( const char *s, size_t n )
	{
		size_t len = std::strlen( s );
		for( size_t i = 0; i < n; ++i ) data.push_back( i < len ? s[i] : 0 );
	}
	void frame( float yaw, float x, float z )
	{
		f32( 0.0f ); f32( std::sin( yaw * 0.5f ) ); f32( 0.0f ); f32( std::cos( yaw * 0.5f ) );
		f32( x ); f32( 0.0f ); f32( z ); f32( 1.0f ); f32( 1.0f ); f32( 1.0f );
	}
};


// A planar walk clip: N root frames turning 90 degrees per frame with one-unit forward steps,
// plus a constant limb joint so the skeleton has more than one bone. The playback rate is
// supplied later through RootBlendConfig, so the resource itself needs no fps parameter.
static Bytes makeWalkClip( uint32_t frames )
{
	Bytes b;
	b.text( "H3DA", 4 ); b.u32( 3 ); b.u32( 2 ); b.u32( frames );
	b.text( "root", 256 ); b.u8( 0 );
	for( uint32_t i = 0; i < frames; ++i )
		b.frame( i * 1.5707963267948966f, (float)i, 0.0f );
	b.text( "limb", 256 ); b.u8( 1 );  // compressed constant track
	b.frame( 0.0f, 0.0f, 0.0f );
	return b;
}


// One-joint triangle geometry with identity inverse-bind matrices.
static Bytes makeGeometry()
{
	Bytes b;
	b.text( "H3DG", 4 ); b.u32( 5 ); b.u32( 1 );
	for( int i = 0; i < 16; ++i ) b.f32( i % 5 == 0 ? 1.0f : 0.0f );  // identity inv-bind
	b.u32( 6 ); b.u32( 3 );
	b.u32( 0 ); b.u32( 12 );  // positions
	const float verts[3][3] = { { 0.2f, 0.3f, 0.0f }, { -0.2f, 0.3f, 0.0f }, { 0.0f, 0.8f, 0.1f } };
	for( const float *p : verts ) for( int c = 0; c < 3; ++c ) b.f32( p[c] );
	for( int stream = 1; stream <= 3; ++stream )
	{
		b.u32( stream ); b.u32( 6 );
		for( int i = 0; i < 3; ++i ) for( int c = 0; c < 3; ++c ) b.u16( c == stream - 1 ? 32767 : 0 );
	}
	b.u32( 4 ); b.u32( 4 ); for( int i = 0; i < 12; ++i ) b.u8( 0 );  // joint 0
	b.u32( 5 ); b.u32( 4 );
	for( int i = 0; i < 3; ++i ) { b.u8( 255 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }
	b.u32( 3 ); b.u32( 0 ); b.u32( 1 ); b.u32( 2 );
	b.u32( 0 );  // no morph targets
	return b;
}


static H3DRes load( int type, const char *name, const Bytes &bytes )
{
	H3DRes r = h3dAddResource( type, name, 0 );
	return r && h3dLoadResource( r, bytes.data.data(), (int)bytes.data.size() ) ? r : 0;
}

}  // namespace


int main()
{
	// 256 selects the headless Null backend; no window or GPU is required.
	if( !h3dInit( (H3DRenderDevice::List)256 ) )
	{
		std::fprintf( stderr, "h3dInit failed\n" );
		return 1;
	}
	h3dSetOption( H3DOptions::FastAnimation, 0 );

	// Two shared, read-only clips with different frame rates.
	H3DRes clipA = load( H3DResTypes::Animation, "walkA.anim", makeWalkClip( 4 ) );
	H3DRes clipB = load( H3DResTypes::Animation, "walkB.anim", makeWalkClip( 6 ) );
	H3DRes geo = load( H3DResTypes::Geometry, "skin.geo", makeGeometry() );
	H3DRes material = h3dAddResource( H3DResTypes::Material, "unrendered.material", 0 );
	if( !clipA || !clipB || !geo ) { std::fprintf( stderr, "resource setup failed\n" ); return 1; }

	H3DNode parent = h3dAddGroupNode( H3DRootNode, "placement" );
	H3DNode model = h3dAddModelNode( parent, "character", geo );
	h3dSetNodeParamI( model, H3DModel::SWSkinningI, 1 );
	H3DNode root = h3dAddJointNode( model, "root", 0 );
	h3dAddJointNode( root, "limb", 1 );
	h3dAddMeshNode( model, "triangle", material, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );

	// Blend weight ramps from clip A to clip B over the first second, then stays at 75% B.
	RootBlendWeightKey keys[] = { { 0.0, 0.0f }, { 1.0, 0.75f } };

	RootBlendConfig config;
	config.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( clipA );
	config.anim[1] = (AnimationResource *)Modules::resMan().resolveResHandle( clipB );
	config.fps[0] = 10.0f;  config.fps[1] = 15.0f;
	config.phase[0] = 0.0f; config.phase[1] = 0.0f;
	config.rate[0] = 1.0f;  config.rate[1] = 1.0f;
	config.rootName = "root";
	config.model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( model );
	config.startRoot = Matrix4f();  // Identity: start at the world origin facing +x.
	config.keys = keys;
	config.numKeys = sizeof( keys ) / sizeof( keys[0] );

	RootBlendPlayer player;
	if( !player.initialize( config ) )
	{
		std::fprintf( stderr, "RootBlendPlayer initialize failed\n" );
		return 1;
	}

	std::printf( "time(s)  world x      world z      yaw(rad)\n" );
	for( double t = 0.0; t <= 2.0; t += 0.5 )
	{
		if( !player.evaluate( t ) )
		{
			std::fprintf( stderr, "evaluate failed at t=%g\n", t );
			return 1;
		}
		Matrix4f world;
		player.getWorldRoot( world );
		float yaw = std::atan2( world.x[8], world.x[10] );
		std::printf( "%-8.2f %-12.5f %-12.5f %-9.5f\n", t, world.x[12], world.x[14], yaw );
	}

	// Release the two stages; regular stage animation is available on the model again.
	player.unbind();
	h3dRelease();
	std::printf( "root_blend_example OK\n" );
	return 0;
}
