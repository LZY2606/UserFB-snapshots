#ifndef PULSE_TRACE_H
#define PULSE_TRACE_H

/*
 * Test-only observation seam for ACELP_4t64_fx().
 *
 * The production translation units include this header and call
 * pulse_trace_callback() only when PULSE_TRACE is defined.  The test
 * executable provides the callback and copies the observation into its own
 * storage.  Builds without PULSE_TRACE contain neither the call nor the
 * header dependency.
 */

#define PULSE_TRACE_MAGIC 0x50554c53u /* "PULS" */
#define PULSE_TRACE_NB_SAMPLES 64
#define PULSE_TRACE_NB_INDEX 4

typedef struct pulse_trace_sample {
	unsigned int magic;
	int nbbits;
	int ser_size;
	int pulse_amplitude;
	short code[PULSE_TRACE_NB_SAMPLES];
	short index[PULSE_TRACE_NB_INDEX];
} pulse_trace_sample;

/* Implemented by tests/pulse_contract.c. */
void pulse_trace_callback(const pulse_trace_sample *sample);

#endif
