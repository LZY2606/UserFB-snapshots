//
//  SineWave.h
//  Tonic 
//
//  Created by Nick Donaldson on 2/8/13.

//
// See LICENSE.txt for license and usage information.
//


#ifndef TONIC_SINEWAVE_H
#define TONIC_SINEWAVE_H

#include "TableLookupOsc.h"

namespace Tonic {
  
  class SineWave : public TemplatedGenerator<Tonic_::TableLookupOsc_>{
    
  public:
    
    SineWave();
    
    TONIC_MAKE_GEN_SETTERS(SineWave, freq, setFrequency)

    //! Per-sample phase offset in cycles. Accepts float, ControlGenerator or Generator.
    TONIC_MAKE_GEN_SETTERS(SineWave, phaseOffset, setPhaseOffset)

  };
}

#endif


