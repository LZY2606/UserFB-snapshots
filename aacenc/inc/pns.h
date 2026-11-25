#ifndef _PNS_H
#define _PNS_H

#include "typedef.h"
#include "interface.h"
#include "qc_data.h"

#define PNS_NOISE_OFFSET 90
#define PNS_PREAMBLE     256
#define PNS_PREAMBLE_BITS 9

void PNS_ResetChannel(PSY_OUT_CHANNEL *psyOut, Word16 enable);

void PNS_SelectLong(const Word32 *spectrum,
                    const Word16 *sfbOffset,
                    Word16 sfbCnt,
                    Word32 sampleRate,
                    Word16 mdctScale,
                    PSY_OUT_CHANNEL *psyOut);

void PNS_EvaluateShortWindow(const Word32 *spectrum,
                             const Word16 *sfbOffset,
                             Word16 sfbCnt,
                             Word32 sampleRate,
                             Word16 mdctScale,
                             Word16 window,
                             PSY_OUT_CHANNEL *psyOut);

void PNS_BuildShortGroups(Word16 noOfGroups,
                          const Word16 *groupLen,
                          Word16 shortSfbCnt,
                          Word16 mdctScale,
                          PSY_OUT_CHANNEL *psyOut);

void PNS_ResolveDeltas(PSY_OUT_CHANNEL *psyOut, Word16 groupedSfbCnt);

void PNS_ClearQuantizedBands(const PSY_OUT_CHANNEL *psyOut,
                             QC_OUT_CHANNEL *qcOut);

void PNS_FinalizeBitstreamChannel(const PSY_OUT_CHANNEL *psyOut,
                                  QC_OUT_CHANNEL *qcOut);

#endif
