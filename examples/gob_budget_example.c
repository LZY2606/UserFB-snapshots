/*
 * GOB budget rate control - configuration example.
 *
 * Demonstrates how to enable the optional per-GOB budget mode on an encoder
 * context, and how to read the per-GOB statistics record. The legacy fixed
 * quantiser (H261_RATECTRL_MODE_QUANT) and whole-frame VBR
 * (H261_RATECTRL_MODE_VBR) modes keep working unchanged when this mode is
 * not enabled.
 */

#include "h261_decl.h"

/* Observe callback: invoked exactly once per committed GOB. The stats record
 * is only borrowed for the duration of the call; copy what you need. */
static Void on_gob( const h261_gob_budget_stats_t *ps_stats, Void *p_opaque )
{
	FILE *pf_log = ( FILE * ) p_opaque;

	fprintf( pf_log,
		"frame %d gob %d: gquant=%d overflow=%d trials=%d bits=%lld quota=%lld "
		"coded=%lld target=%lld debt=%lld sse=%lld\n",
		ps_stats->i_frame_num, ps_stats->i_gob_number, ps_stats->i_gquant,
		ps_stats->i_overflow, ps_stats->i_trials,
		( long long ) ps_stats->i_bits, ( long long ) ps_stats->i_quota,
		( long long ) ps_stats->i_coded_total, ( long long ) ps_stats->i_target_total,
		( long long ) ps_stats->i_debt_after, ( long long ) ps_stats->i_sse );
}

Int32 example_enable_gob_budget( h261_context_t *ps_ctx, FILE *pf_log )
{
	h261_gob_budget_config_t s_config;

	s_config.i_bitrate = 64000;      /* link budget, bits per second (1..1e8) */
	s_config.i_fps_num = 30000;      /* frame rate numerator (1..1e6) */
	s_config.i_fps_den = 1001;       /* frame rate denominator (1..1e6), 30000/1001 */
	s_config.i_buffer_bits = 128000; /* decoder buffer, bits (1..1e9) */
	s_config.pf_observe = on_gob;    /* may be NULL */
	s_config.p_opaque = pf_log;

	/* returns 1 on success, 0 on NULL/invalid config (state untouched) */
	return h261_ratectrl_init_gob_budget( ps_ctx, &s_config );
}

/* To switch back to a legacy mode, reinitialise the rate controller; this
 * disables the GOB budget mode and restarts the legacy controller:
 *
 *   h261_init_ratectrl( ps_ctx, 10, 0, 0, H261_RATECTRL_MODE_QUANT );
 */
