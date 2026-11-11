//
//  TableLookupOsc.h
//  Tonic 
//
//  Created by Nick Donaldson on 2/8/13.

//
// See LICENSE.txt for license and usage information.
//


#ifndef TONIC_TABLELOOKUPOSC_H
#define TONIC_TABLELOOKUPOSC_H

#include "Generator.h"
#include "SampleTable.h"

namespace Tonic {
  
  namespace Tonic_ {
    
    // Registry for all static oscillator lookup table data
    TonicDictionary<SampleTable> * s_oscillatorTables();
    
    class TableLookupOsc_ : public Generator_{
      
      //------------------------------------
      //
      // Many of the original STK methods are not applicable in our use case
      // (direct phase/freq manipulation) and have been removed for optimization.
      //
      // A per-sample phase offset input is available through setPhaseOffset.
      // The offset is measured in cycles (not radians or table indices), only
      // shifts the read position, and is never written back into the carrier.
      //
      //-----------------------------------

    protected:

      SampleTable lookupTable_;

      double phase_;

      Generator frequencyGenerator_;
      Generator phaseOffsetGenerator_;
      TonicFrames modFrames_;
      TonicFrames phaseOffsetFrames_;

      void computeSynthesisBlock( const SynthesisContext_ & context );
      
    public:
      
      TableLookupOsc_();

      //! Clear output and reset time pointer to zero.
      void reset( void );
      
      //! Set frequency generator input
      void setFrequency( Generator genArg){
        frequencyGenerator_ = genArg;
      }

      //! Set per-sample phase offset input. Units are cycles (periods), not radians.
      void setPhaseOffset( Generator genArg){
        phaseOffsetGenerator_ = genArg;
      }

      //! set sample table for lookup. MUST BE POWER OF 2 IN LENGTH
      void setLookupTable( SampleTable table );

    };
    
    inline void TableLookupOsc_::computeSynthesisBlock( const SynthesisContext_ & context ){

      // Update the frequency data. Rate conversion is done in modFrames_, so a
      // shared generator's own output frames are never modified.
      frequencyGenerator_.tick(modFrames_, context);

      // Per-sample phase offsets in cycles. A stereo input is averaged down to
      // mono by TonicFrames::copy, so phaseOffsetFrames_ stays one channel.
      phaseOffsetGenerator_.tick(phaseOffsetFrames_, context);

      unsigned long tableSize = lookupTable_.size()-1;

      const TonicFloat rateConstant = (TonicFloat)tableSize / Tonic::sampleRate();

      TonicFloat *samples = &outputFrames_[0];
      TonicFloat *rateBuffer = &modFrames_[0];
      TonicFloat *phaseOffsetBuffer = &phaseOffsetFrames_[0];
      TonicFloat *tableData = lookupTable_.dataPointer();
      
      // R. Hoelderich style fast phasor.
      
      FastPhasor sd;
      
      // pre-multiply rate constant for speed
#ifdef USE_APPLE_ACCELERATE
      vDSP_vsmul(rateBuffer, 1, &rateConstant, rateBuffer, 1, kSynthesisBlockSize);
#else
      for (unsigned int i=0; i<kSynthesisBlockSize; i++){
        *rateBuffer++ *= rateConstant;
      }
      rateBuffer = &modFrames_[0];
#endif
      
      sd.d = BIT32DECPT;
      
      TonicInt32 offs;
      TonicInt32 msbi = sd.i[1];
      
      double frac;
      double ps = phase_ + BIT32DECPT;
      
      TonicFloat *tAddr, f1, f2;

      for ( unsigned int i=0; i<kSynthesisBlockSize; i++ ) {

        // This sample's offset, in cycles. NaN or infinite offsets are treated
        // as no offset for this frame, so a bad input can never poison the
        // carrier or the rest of the output.
        TonicFloat phaseOffset = *phaseOffsetBuffer++;
        if (phaseOffset != phaseOffset || !std::isfinite(phaseOffset)){
          phaseOffset = 0.0f;
        }

        if (phaseOffset == 0.0f){

          // Unmodulated path: the original fast phasor, unchanged.
          sd.d = ps;
          offs = sd.i[1] & (tableSize-1);
          tAddr = tableData + offs;
          sd.i[1] = msbi;
          frac = sd.d - BIT32DECPT;
          f1 = tAddr[0];
          f2 = tAddr[1];

          *samples++ = f1 + frac * (f2 - f1);
        }
        else{

          // Modulated path: read at carrier phase plus this frame's offset.
          // The offset only moves the read head; it is never added to ps, so
          // it cannot be written back into the carrier accumulator or be
          // integrated like a frequency. Whole cycles are folded away in
          // floating point first (fmod), so negative, large and
          // integer-plus-fractional offsets wrap without integer overflow,
          // and offsets differing by an integer number of cycles read the same.
          double readPosition = (ps - BIT32DECPT) + (double)phaseOffset * (double)tableSize;
          double integerPart = floor(readPosition);
          double wrapped = fmod(integerPart, (double)tableSize);
          if (wrapped < 0.0) wrapped += (double)tableSize;
          long tableIndex = (long)wrapped;
          frac = readPosition - integerPart;
          tAddr = tableData + tableIndex;
          f1 = tAddr[0];
          f2 = tAddr[1];

          *samples++ = f1 + (TonicFloat)(frac * (f2 - f1));
        }

        // Advance the carrier by this frame's frequency only after the lookup.
        ps += *rateBuffer++;
      }
      
      sd.d = BIT32DECPT * tableSize;
      msbi = sd.i[1];
      sd.d = ps + (BIT32DECPT * tableSize - BIT32DECPT);
      sd.i[1] = msbi;
      phase_ = sd.d - BIT32DECPT * tableSize;
      
    }

  }
  
  class TableLookupOsc : public TemplatedGenerator<Tonic_::TableLookupOsc_>{
    
    public:
    
      TableLookupOsc & setLookupTable( SampleTable lookupTable );

      TONIC_MAKE_GEN_SETTERS(TableLookupOsc, freq, setFrequency);

      //! Per-sample phase offset in cycles. Accepts float, ControlGenerator or Generator.
      TONIC_MAKE_GEN_SETTERS(TableLookupOsc, phaseOffset, setPhaseOffset);
  };

}

#endif


