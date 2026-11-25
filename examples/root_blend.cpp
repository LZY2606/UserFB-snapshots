// *************************************************************************************************
//
// Horde3D
//   Minimal dual-action root-motion blend example (headless, no window or GPU output).
//
//   Builds two tiny procedural H3DA clips and a skinned triangle, binds a RootBlendPlayer and
//   evaluates the integrated world root at a few absolute times.
//
// *************************************************************************************************

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

struct Bytes
{
	std::vector<char> d;
	void u32( uint32_t v ) { for( int i = 0; i < 4; ++i ) d.push_back( char( v >> ( 8 * i ) ) ); }
	void u16( int v ) { d.push_back( char( v ) ); d.push_back( char( v >> 8 ) ); }
	void u8( int v ) { d.push_back( char( v ) ); }
	void f32( float v ) { uint32_t b; std::memcpy( &b, &v, 4 ); u32( b ); }
	void text( const char *s, size_t n )
	{
		size_t len = std::strlen( s );
		for( size_t i = 0; i < n; ++i ) d.push_back( i < len ? s[i] : 0 );
	}
};

void rootFrame( Bytes &b, float yaw, float x, float z )
{
	b.f32( 0.0f );
	b.f32( sinf( yaw * 0.5f ) );
	b.f32( 0.0f );
	b.f32( cosf( yaw * 0.5f ) );
	b.f32( x ); b.f32( 0.0f ); b.f32( z );
	b.f32( 1.0f ); b.f32( 1.0f ); b.f32( 1.0f );
}

void limbFrame( Bytes &b, float y )
{
	b.f32( 0.0f ); b.f32( 0.0f ); b.f32( 0.0f ); b.f32( 1.0f );
	b.f32( 0.0f ); b.f32( y ); b.f32( 0.0f );
	b.f32( 1.0f ); b.f32( 1.0f ); b.f32( 1.0f );
}

// Two planar root clips: a straight walk and a turning run.
Bytes makeWalk()
{
	Bytes b;
	b.text( "H3DA", 4 ); b.u32( 3 ); b.u32( 2 ); b.u32( 3 );
	b.text( "root", 256 ); b.u8( 0 );
	for( int i = 0; i < 3; ++i ) rootFrame( b, 0.0f, float( i ), 0.0f );
	b.text( "limb", 256 ); b.u8( 0 );
	for( int i = 0; i < 3; ++i ) limbFrame( b, 0.5f + 0.25f * i );
	return b;
}

Bytes makeRun()
{
	Bytes b;
	b.text( "H3DA", 4 ); b.u32( 3 ); b.u32( 2 ); b.u32( 4 );
	b.text( "root", 256 ); b.u8( 0 );
	for( int i = 0; i < 4; ++i ) rootFrame( b, 0.4f * i, 1.5f * i, 0.5f * i );
	b.text( "limb", 256 ); b.u8( 0 );
	for( int i = 0; i < 4; ++i ) limbFrame( b, 1.0f + 0.3f * i );
	return b;
}

Bytes makeGeometry()
{
	const float xyz[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	Bytes b;
	b.text( "H3DG", 4 ); b.u32( 5 );
	b.u32( 2 );  // Two joints, identity inverse bind
	for( int j = 0; j < 2; ++j )
		for( int i = 0; i < 16; ++i ) b.f32( i % 5 == 0 ? 1.0f : 0.0f );
	b.u32( 6 ); b.u32( 3 );
	b.u32( 0 ); b.u32( 12 );
	for( auto &p : xyz ) for( float v : p ) b.f32( v );
	for( int stream = 1; stream <= 3; ++stream )
	{
		b.u32( stream ); b.u32( 6 );
		for( int i = 0; i < 3; ++i )
			for( int c = 0; c < 3; ++c ) b.u16( c == stream - 1 ? 32767 : 0 );
	}
	b.u32( 4 ); b.u32( 4 );
	for( int i = 0; i < 3; ++i ) { b.u8( i == 2 ? 1 : 0 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }
	b.u32( 5 ); b.u32( 4 );
	for( int i = 0; i < 3; ++i ) { b.u8( 255 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }
	b.u32( 3 ); b.u32( 0 ); b.u32( 1 ); b.u32( 2 );
	b.u32( 0 );  // No morph targets
	return b;
}

H3DRes load( int type, const char *name, const Bytes &b )
{
	H3DRes r = h3dAddResource( type, name, 0 );
	return r && h3dLoadResource( r, b.d.data(), (int)b.d.size() ) ? r : 0;
}

}  // namespace

int main()
{
	if( !h3dInit( static_cast<H3DRenderDevice::List>( 256 ) ) )
	{
		std::fprintf( stderr, "h3dInit failed\n" );
		return 1;
	}
	h3dSetOption( H3DOptions::FastAnimation, 0 );

	const H3DRes walk = load( H3DResTypes::Animation, "walk.anim", makeWalk() );
	const H3DRes run = load( H3DResTypes::Animation, "run.anim", makeRun() );
	const H3DRes geo = load( H3DResTypes::Geometry, "skin.geo", makeGeometry() );
	const H3DRes mat = h3dAddResource( H3DResTypes::Material, "skin.material", 0 );
	if( !walk || !run || !geo || !mat ) { std::fprintf( stderr, "resource build failed\n" ); return 1; }

	const H3DNode model = h3dAddModelNode( H3DRootNode, "character", geo );
	const H3DNode root = h3dAddJointNode( model, "root", 0 );
	h3dAddJointNode( root, "limb", 1 );
	h3dAddMeshNode( model, "tri", mat, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );
	ModelNode *modelNode = (ModelNode *)Modules::sceneMan().resolveNodeHandle( model );

	// Fade the run clip in between t = 1s (weight 0) and t = 3s (weight 1).
	const std::vector< RootBlendKey > keys = {
		RootBlendKey{ 0.0f, 0.0f }, RootBlendKey{ 1.0f, 0.0f },
		RootBlendKey{ 3.0f, 1.0f }, RootBlendKey{ 30.0f, 1.0f }
	};

	RootBlendPlayer player;
	Matrix4f start = Matrix4f( Quaternion( 0.0f, 0.0f, 0.0f ) );
	start = Matrix4f::TransMat( 0.0f, 0.0f, 0.0f ) * start;

	const bool ok = player.bind(
		(AnimationResource *)Modules::resMan().resolveResHandle( walk ),
		(AnimationResource *)Modules::resMan().resolveResHandle( run ),
		10.0f, 10.0f,   // fps of each clip
		0.0f, 0.0f,     // phases
		1.0f, 1.0f,     // signed playback rates
		"root", modelNode, start, keys );
	if( !ok ) { std::fprintf( stderr, "bind failed\n" ); return 1; }

	for( int i = 0; i <= 4; ++i )
	{
		const float t = (float)i;
		if( !player.evaluate( t ) ) { std::fprintf( stderr, "evaluate failed at %g\n", t ); return 1; }
		Matrix4f world;
		player.getWorldRoot( world );
		std::printf( "t=%g root=(%.5f, %.5f, %.5f) yaw=%.5f\n",
		             t, world.x[12], world.x[13], world.x[14],
		             atan2f( world.c[2][0], world.c[0][0] ) );
	}

	player.unbind();
	h3dRelease();
	std::printf( "ROOT_BLEND_EXAMPLE_OK\n" );
	return 0;
}
