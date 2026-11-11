//
// sndfilter - Algorithms for sound filters, like reverb, lowpass, etc
// by Sean Connelly (@velipso), https://sean.fun
// Project Home: https://github.com/velipso/sndfilter
// SPDX-License-Identifier: 0BSD
//

#include "snd.h"
#include "mem.h"
#include <string.h>

sf_snd sf_snd_new(int size, int rate, bool clear){
	sf_snd snd = sf_malloc(sizeof(sf_snd_st));
	if (snd == NULL)
		return NULL;
	snd->size = size;
	snd->rate = rate;
	// a zero-length sound needs no sample storage; malloc(0) is allowed to return NULL, which
	// would otherwise look like an allocation failure
	snd->samples = size > 0 ? sf_malloc(sizeof(sf_sample_st) * size) : NULL;
	if (size > 0 && snd->samples == NULL){
		sf_free(snd);
		return NULL;
	}
	if (clear && size > 0)
		memset(snd->samples, 0, sizeof(sf_sample_st) * size);
	return snd;
}

void sf_snd_free(sf_snd snd){
	sf_free(snd->samples);
	sf_free(snd);
}
