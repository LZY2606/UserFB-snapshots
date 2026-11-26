/*
 * gob_budget_example.c - GOB budget mode configuration example.
 *
 * Build (from the repository root, after task_support/build.sh):
 *   cc -std=c99 -O1 -DH261_PORTABLE -I. -I src/h261 -I task_support \
 *      examples/gob_budget_example.c .task-build/core.a -lm -o /tmp/gob_example
 *
 * The fixture helpers allocate QCIF/CIF frames and a writer; in your own
 * application replace fixture_init/fixture_fill/fixture_encode with your
 * normal h261_context_t setup and encoding loop.
 */

#include "h261_decl.h"
#include "fixture.h"

#include <stdio.h>
#include <string.h>

/*
 * observe is invoked exactly once per committed GOB.  The stats pointer is
 * BORROWED: it remains valid only for the duration of this call and is owned
 * by the encoder instance, so copy anything you need (as shown here).
 */
static void on_gob( const h261_gob_budget_stats_t *ps_stats, void *pv_opaque )
{
	unsigned long *p_calls = ( unsigned long * ) pv_opaque;
	( *p_calls )++;

	printf( "frame=%d gob=%d gquant=%d overflow=%d trials=%d "
	        "bits=%lld quota=%lld coded=%lld target=%lld debt=%lld sse=%lld\n",
	        ps_stats->frame_num, ps_stats->gob_number, ps_stats->gquant,
	        ps_stats->overflow, ps_stats->trials,
	        ( long long ) ps_stats->bits,
	        ( long long ) ps_stats->quota,
	        ( long long ) ps_stats->coded_total,
	        ( long long ) ps_stats->target_total,
	        ( long long ) ps_stats->debt_after,
	        ( long long ) ps_stats->sse );
}

int main( void )
{
	h261_context_t s_ctx;
	h261_gob_budget_config_t s_cfg;
	unsigned long ul_observe_calls = 0;
	int i_frame;

	/* Plain fixed-quant setup also allocates frames, cache and writer. */
	if( !fixture_init( &s_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) )
	{
		return 1;
	}

	memset( &s_cfg, 0, sizeof( s_cfg ) );
	s_cfg.bitrate     = 128000;              /* bits/s,      1..100000000      */
	s_cfg.fps_num     = 30;                  /* frame rate = fps_num/fps_den   */
	s_cfg.fps_den     = 1;                   /* both terms 1..1000000, ratio   */
	                                         /* in [1,120] frames/s            */
	s_cfg.buffer_bits = 16000;               /* bits,        1..1000000000     */
	s_cfg.observe     = on_gob;              /* may be NULL (no callback)      */
	s_cfg.opaque      = &ul_observe_calls;   /* forwarded verbatim, may be NULL*/

	/*
	 * Returns 1 and restarts the budget history on success.  A NULL config or
	 * any out-of-range value makes it return 0 and leaves the control state
	 * unchanged.
	 */
	if( !h261_ratectrl_init_gob_budget( &s_ctx, &s_cfg ) )
	{
		fprintf( stderr, "invalid GOB budget configuration\n" );
		fixture_free( &s_ctx );
		return 1;
	}

	for( i_frame = 0; i_frame < 4; i_frame++ )
	{
		/* Any local picture source works; fixture_fill supplies a texture. */
		fixture_fill( &s_ctx, i_frame );

		/*
		 * fixture_encode runs h261_ratectrl_init_picture,
		 * h261_init_picture, h261_write_picture_header and
		 * h261_encode_gobs.  With the budget enabled, h261_encode_gobs
		 * performs the 31 real trial encodings per GOB and commits the
		 * selected candidate.  The reference-frame update stays the normal
		 * whole-frame copy done here by the fixture/application.
		 */
		fixture_encode( &s_ctx, i_frame, NULL );
	}

	printf( "observe calls=%lu (expected %d = frames * 3 QCIF GOBs)\n",
	        ul_observe_calls, 4 * 3 );

	/* Calling the old initialiser switches GOB budget mode off. */
	h261_init_ratectrl( &s_ctx, 10, 64000, 64000, H261_RATECTRL_MODE_QUANT );

	fixture_free( &s_ctx );
	return ul_observe_calls == 12 ? 0 : 2;
}
