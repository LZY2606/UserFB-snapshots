#ifndef _WDL_SIMPLEPITCHSHIFT_H_
#define _WDL_SIMPLEPITCHSHIFT_H_

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "queue.h"
#include "resample.h"

#ifndef WDL_SIMPLEPITCHSHIFT_SAMPLETYPE
#define WDL_SIMPLEPITCHSHIFT_SAMPLETYPE double
#endif

static inline bool wdl_sps_isfinite(double v) { return v<=DBL_MAX && v>=-DBL_MAX; }


#ifdef WDL_SIMPLEPITCHSHIFT_PARENTCLASS
class WDL_SimplePitchShifter : public WDL_SIMPLEPITCHSHIFT_PARENTCLASS
#else
class WDL_SimplePitchShifter
#endif
{
public:
  WDL_SimplePitchShifter()
  {
    m_last_nch=1;
    m_srate=44100.0;
    m_last_tempo=1.0;
    m_last_shift=1.0;
    m_qual=0;

    m_hq_mode=false;
    m_hq_locked=false;
    m_hq_flushed=false;
    m_hq_bsize=0;
    m_hq_olsize=0;
    m_hq_latpos=0;
    m_hq_inframes=0;
    m_hq_outframes=0;

    Reset();
  }
  ~WDL_SimplePitchShifter() {   }

  // Reset() clears all streaming state: queued output samples, the pitch
  // window history, the HQ resampler tail, the flush flag, and the HQ
  // configuration lock. It does NOT clear the configuration: sample rate,
  // channel count, shift, tempo, quality parameter, and HQ mode all survive
  // Reset(), so a new clip can be processed immediately; in HQ mode the
  // configuration setters may also be called again until the first non-empty
  // BufferDone() of the new clip.
  void Reset()
  {
    m_hadinput=0;
    m_pspos=0.0;
    m_pswritepos=0;
    m_latpos=0;
    m_tempo_fracpos=0.0;
    m_queue.Clear();
    m_rsbuf.Resize(0,false);
    m_psbuf.Resize(0,false);

    m_hq_locked=false;
    m_hq_flushed=false;
    m_hq_latpos=0;
    m_hq_inframes=0;
    m_hq_outframes=0;
    m_hq_rs.Reset();
  }

  bool IsReset()
  {
    return !m_queue.Available() && !m_hadinput;
  }



  // In HQ mode (see set_hq_mode) these setters validate their argument and
  // latch the configuration: values must be finite and in range (srate must
  // be positive, nch must be 1, 2, or 6, shift/tempo must be in [0.5,4]).
  // Invalid values are rejected (return false, nothing changes), and once
  // the first non-empty BufferDone() has locked the stream, all further
  // changes are rejected as well. In legacy mode they keep their historical
  // behavior and always return true.
  bool set_srate(double srate)
  {
    if (m_hq_mode)
    {
      if (m_hq_locked) return false;
      if (!(srate>0.0) || !wdl_sps_isfinite(srate)) return false;
    }
    m_srate=srate;
    return true;
  }
  bool set_nch(int nch)
  {
    if (m_hq_mode)
    {
      if (m_hq_locked) return false;
      if (nch!=1 && nch!=2 && nch!=6) return false;
      m_last_nch=nch;
      m_tempo_fracpos=0.0;
      return true;
    }
    if (m_last_nch!=nch) { m_queue.Clear(); m_last_nch=nch; m_tempo_fracpos=0.0; }
    return true;
  }
  bool set_shift(double shift)
  {
    if (m_hq_mode)
    {
      if (m_hq_locked) return false;
      if (!wdl_sps_isfinite(shift) || shift<0.5 || shift>4.0) return false;
    }
    m_last_shift=shift;
    return true;
  }
  bool set_tempo(double tempo)
  {
    if (m_hq_mode)
    {
      if (m_hq_locked) return false;
      if (!wdl_sps_isfinite(tempo) || tempo<0.5 || tempo>4.0) return false;
    }
    m_last_tempo=tempo;
    return true;
  }
  void set_formant_shift(double shift)
  {
  }

  // ------------------------------------------------------------------
  // HQ streaming mode (opt-in, default off).
  //
  // When enabled, the tempo stage resamples the dual-read-head pitch
  // shifter output with the windowed-sinc kernel of WDL_Resampler instead
  // of linear interpolation, and both stages carry their sample positions
  // across block boundaries: the pitch read head advances shift/tempo per
  // input frame, the sinc stage consumes the intermediate PCM at tempo.
  // The delivered stream therefore does not depend on the caller's block
  // sizes, and large tempo ratios are properly anti-aliased.
  //
  // Delivery latency vs. the pitch window: the pitch read head runs half a
  // pitch window (bsize/2 frames, see GetSizes) behind the write position,
  // and the sinc kernel needs a fixed half-tap of lookahead. Both are fixed
  // startup waits only: the first bsize/2 intermediate frames read the
  // zero-extended history and are skipped (like the legacy m_latpos logic),
  // so they are not delivered as extra silence, and the sinc lookahead
  // merely delays delivery by a fixed number of input frames. Output frame
  // k always maps to input position k*tempo.
  //
  // FlushSamples() declares the input finished: the pitch window is drained
  // with internal zero padding (not counted as input, never repeated end
  // samples) and the sinc tail is delivered, so the total readable output
  // for N input frames is exactly ceil(N/tempo) frames. Flushing is
  // idempotent, non-empty input afterwards is rejected, and Reset() arms
  // the processor for the next clip.
  //
  // Configuration (set_srate/set_nch/set_shift/set_tempo) must be complete
  // before the first non-empty BufferDone(); while a clip is being
  // processed the parameters are fixed. set_hq_mode() itself may only be
  // toggled after construction/Reset() and before any audio: it returns
  // false without touching anything (in particular without discarding
  // queued audio) if a stream has already started.
  bool set_hq_mode(bool on)
  {
    if (on==m_hq_mode) return true;
    if (m_hadinput) return false; // never drop in-flight audio to fake a switch
    m_hq_mode=on;
    m_hq_locked=false;
    m_hq_flushed=false;
    m_hq_latpos=0;
    m_hq_inframes=0;
    m_hq_outframes=0;
    m_hq_rs.Reset();
    return true;
  }
  bool get_hq_mode() const { return m_hq_mode; }

  WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *GetBuffer(int size)
  {
    return m_inbuf.Resize(size*m_last_nch);
  }
  void BufferDone(int input_filled);
  void FlushSamples();

  static const char *enumQual(int q);
  static bool GetSizes(int qv, int *ws, int *os); // if ws or os is NULL, enumerate available ws/os

  int GetSamples(int requested_output, WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *buffer);


 void SetQualityParameter(int parm)
 {
   m_qual=parm;
 }

#ifdef WDL_SIMPLEPITCHSHIFT_EXTRA_INTERFACE
 WDL_SIMPLEPITCHSHIFT_EXTRA_INTERFACE
#endif

private:
  void PitchShiftBlock(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *inputs, WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *outputs, int nch, int length, double pitch, int bsize, int olsize, double srate);
  void hq_lock_config();
  void hq_feed(const WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *src, int len, long long deliver_cap);


private:
  double m_pspos WDL_FIXALIGN;
  double m_tempo_fracpos;
  double m_srate,m_last_tempo,m_last_shift;

  WDL_TypedBuf<WDL_SIMPLEPITCHSHIFT_SAMPLETYPE> m_psbuf;
  WDL_Queue m_queue;
  WDL_TypedBuf<WDL_SIMPLEPITCHSHIFT_SAMPLETYPE> m_inbuf;
  WDL_TypedBuf<WDL_SIMPLEPITCHSHIFT_SAMPLETYPE> m_rsbuf;

  int m_pswritepos;
  int m_last_nch;
  int m_qual;
  int m_hadinput;

  int m_latpos;

  // HQ streaming mode state (see set_hq_mode)
  WDL_Resampler m_hq_rs;
  WDL_TypedBuf<WDL_SIMPLEPITCHSHIFT_SAMPLETYPE> m_hq_tmpbuf;
  WDL_TypedBuf<WDL_SIMPLEPITCHSHIFT_SAMPLETYPE> m_hq_zbuf;
  WDL_TypedBuf<WDL_ResampleSample> m_hq_rsout;
  long long m_hq_inframes;  // real input frames fed (internal zero padding not included)
  long long m_hq_outframes; // frames delivered to the output queue
  int m_hq_bsize, m_hq_olsize; // latched pitch window / overlap
  int m_hq_latpos;          // startup skip progress (bsize/2 frames of zero history)
  bool m_hq_mode, m_hq_locked, m_hq_flushed;

};


#ifdef WDL_SIMPLEPITCHSHIFT_IMPLEMENT
void WDL_SimplePitchShifter::BufferDone(int input_filled)
{
  if (m_hq_mode)
  {
    // HQ streaming path (see set_hq_mode): pitch-shift at shift/tempo into
    // an intermediate buffer, then sinc-resample the intermediate PCM at
    // tempo. All positions carry across block boundaries.
    if (input_filled<=0) return; // empty input does not advance the stream
    if (m_hq_flushed) return; // input after FlushSamples() is rejected; Reset() starts a new clip
    if (!m_hq_locked) hq_lock_config();
    m_hadinput=1;
    WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *tmp=m_hq_tmpbuf.Resize(input_filled*m_last_nch,false);
    PitchShiftBlock(m_inbuf.Get(),tmp,m_last_nch,input_filled,m_last_shift/m_last_tempo,m_hq_bsize,m_hq_olsize,m_srate);
    m_hq_inframes+=input_filled;
    hq_feed(tmp,input_filled,0x7FFFFFFFFFFFFFFFLL);
    return;
  }
  // perform pitch shifting
  if (input_filled>0)
  {
    m_hadinput=1;
    int ws,os;
    GetSizes(m_qual,&ws,&os);

    int bsize=(int) (ws * 0.001 * m_srate);
    if (bsize<16) bsize=16;
    else if (bsize>128*1024)bsize=128*1024;

    int olsize=(int) (os * 0.001 * m_srate);
    if (olsize > bsize/2) olsize=bsize/2;
    if (olsize<1)olsize=1;
    if (m_psbuf.GetSize() != bsize*m_last_nch)
    {
      memset(m_psbuf.Resize(bsize*m_last_nch,false),0,sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)*bsize*m_last_nch);
      m_pspos=(double) (bsize/2);
      m_pswritepos=0;
    }

    if (fabs(m_last_tempo-1.0)<0.0000000001)
    {
      PitchShiftBlock(m_inbuf.Get(),(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *)m_queue.Add(NULL,input_filled*m_last_nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)),m_last_nch,input_filled,m_last_shift,bsize,olsize,m_srate);
    }
    else
    {
      int needclear=m_rsbuf.GetSize()<m_last_nch;

      WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *bufi=m_rsbuf.Resize((input_filled+1)*m_last_nch,false);
      if (needclear)
        memset(bufi,0,m_last_nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));

      double tempo=m_last_tempo;
      double itempo=1.0/tempo;
      PitchShiftBlock(m_inbuf.Get(),bufi+m_last_nch,m_last_nch,input_filled,m_last_shift*itempo,bsize,olsize,m_srate);

      double fp=m_tempo_fracpos;


      if (m_latpos < bsize/2)
      {
        int a = bsize/2-m_latpos;
        if (a > input_filled) a=input_filled;
        m_latpos+=a;
        fp += a;
      }

      int outlen = (int) (input_filled*itempo-fp) + 128; // upper bound on possible sample count
      if (outlen<0) 
      {
        outlen=0;
      }

      WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *bufo = (WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *)m_queue.Add(NULL,outlen*m_last_nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));
      // resample bufi to bufo
      int i,nch=m_last_nch;
      for (i = 0; i < outlen; i ++)
      {
        double rdpos=floor(fp);
        int idx=((int)rdpos);
        if (idx>=input_filled) 
        {
          // un-add any missing samples
          m_queue.Add(NULL,(i-outlen)*m_last_nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));
          break;
        }
        rdpos = (fp-rdpos);
        int a;
        idx*=nch;
        for (a = 0; a < nch; a ++)
        {
          *bufo++ = bufi[idx+a]*(1.0-rdpos)+bufi[idx+nch+a]*rdpos;
        }
        fp += tempo;
      }
      
      memcpy(bufi,bufi+m_last_nch*input_filled,m_last_nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)); // save last sample for interpolation later
      //
      m_tempo_fracpos=fp-floor(fp);
    }
  }    
}

void WDL_SimplePitchShifter::FlushSamples()
{
  if (!m_hq_mode) return; // legacy mode: historically a no-op
  if (m_hq_flushed) return; // repeated flush must not produce new audio
  m_hq_flushed=true;
  if (!m_hq_locked) return; // no audio was ever fed

  // exact logical output length: real input frames 0..N-1 map to output
  // positions k*tempo < N, i.e. ceil(N/tempo) frames (the -1e-9 only absorbs
  // floating point dust when N/tempo lands exactly on an integer)
  const long long target=(long long)ceil((double)m_hq_inframes/m_last_tempo - 1e-9);
  const int nch=m_last_nch;

  // drain the dual-read-head window: one window of zeros pushes every real
  // input frame past the read head. The padding is internal (it is not
  // counted in m_hq_inframes) and the tail is zero-extended history, never
  // repeated end samples.
  const int z=m_hq_bsize;
  WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *zb=m_hq_zbuf.Resize(z*nch,false);
  memset(zb,0,(size_t)z*nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));
  WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *tmp=m_hq_tmpbuf.Resize(z*nch,false);
  PitchShiftBlock(zb,tmp,nch,z,m_last_shift/m_last_tempo,m_hq_bsize,m_hq_olsize,m_srate);
  hq_feed(tmp,z,target);

  // deliver the sinc tail: keep offering zero input until the exact logical
  // length has been delivered (only a fixed number of lookahead frames is
  // ever needed, the guard is just a safety net)
  int guard=0;
  while (m_hq_outframes<target && guard++<256)
  {
    const int chunk=512;
    zb=m_hq_zbuf.Resize(chunk*nch,false);
    memset(zb,0,(size_t)chunk*nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));
    hq_feed(zb,chunk,target);
  }
}

void WDL_SimplePitchShifter::hq_lock_config()
{
  // latch the processing configuration for the duration of the clip
  int ws,os;
  GetSizes(m_qual,&ws,&os);

  int bsize=(int) (ws * 0.001 * m_srate);
  if (bsize<16) bsize=16;
  else if (bsize>128*1024)bsize=128*1024;

  int olsize=(int) (os * 0.001 * m_srate);
  if (olsize > bsize/2) olsize=bsize/2;
  if (olsize<1)olsize=1;
  m_hq_bsize=bsize;
  m_hq_olsize=olsize;

  if (m_psbuf.GetSize() != bsize*m_last_nch)
  {
    memset(m_psbuf.Resize(bsize*m_last_nch,false),0,sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)*bsize*m_last_nch);
    m_pspos=(double) (bsize/2);
    m_pswritepos=0;
  }

  m_hq_rs.Reset();
  m_hq_rs.SetMode(true,0,true,64,32); // windowed-sinc anti-aliasing
  m_hq_rs.SetFeedMode(true); // input-driven: short blocks just accumulate filter lookahead
  m_hq_rs.SetRates(m_srate,m_srate/m_last_tempo);
  m_hq_locked=true;
}

void WDL_SimplePitchShifter::hq_feed(const WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *src, int len, long long deliver_cap)
{
  const int nch=m_last_nch;

  // skip the pitch stage's startup latency: the first bsize/2 intermediate
  // frames read the zero-extended window history and are not delivered
  if (m_hq_latpos < m_hq_bsize/2)
  {
    int a=m_hq_bsize/2-m_hq_latpos;
    if (a>len) a=len;
    m_hq_latpos+=a;
    src+=a*nch;
    len-=a;
  }

  while (len>0 && m_hq_outframes<deliver_cap)
  {
    WDL_ResampleSample *rb=NULL;
    int want=m_hq_rs.ResamplePrepare(len,nch,&rb);
    if (want<=0 || !rb) break;
    if (want>len) want=len;
    int i,n=want*nch;
    for (i=0;i<n;i++) rb[i]=(WDL_ResampleSample)src[i];

    int outcap=(int)(want/m_last_tempo)+16;
    const long long remain=deliver_cap-m_hq_outframes;
    if (remain<outcap) outcap=(int)remain;
    WDL_ResampleSample *ob=m_hq_rsout.Resize(outcap*nch,false);
    const int produced=m_hq_rs.ResampleOut(ob,want,outcap,nch);
    if (produced>0)
    {
      WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *q=(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *)m_queue.Add(NULL,produced*nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));
      n=produced*nch;
      for (i=0;i<n;i++) q[i]=(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)ob[i];
      m_hq_outframes+=produced;
    }
    src+=want*nch;
    len-=want;
  }
}

const char *WDL_SimplePitchShifter::enumQual(int q)
{
  int ws,os;
  if (!GetSizes(q,&ws,&os)) return NULL;
  static char buf[128];
  sprintf(buf,"%dms window, %dms fade",ws,os);
  return buf;
}

bool WDL_SimplePitchShifter::GetSizes(int qv, int *ws, int *os)
{
  static const short windows[]={50,75,100,150,225,300,40,30,20,10,5,3};
  static const char divs[]={2,3,5,7};
  const int nwindows = (int) (sizeof(windows) / sizeof(windows[0]));
  const int ndivs = (int) (sizeof(divs) / sizeof(divs[0]));

  if (!os)
  {
    if (qv < 0 || qv >= nwindows) return false;
    *ws = windows[qv];
    return true;
  }
  if (!ws)
  {
    if (qv < 0 || qv >= ndivs) return false;
    *os = divs[qv];
    return true;
  }

  int wd=qv/ndivs;
  if (wd >= nwindows) wd=-1;

  *ws=windows[wd>=0?wd:0];
  *os = *ws / divs[qv >= 0 ? qv%ndivs : 0];
  if (*os<1) *os=1;

  return wd>=0;
}

int WDL_SimplePitchShifter::GetSamples(int requested_output, WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *buffer)
{
  if (!m_last_nch||requested_output<1) return 0;

  int l=m_queue.Available()/sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)/m_last_nch;

  if (requested_output>l) requested_output=l;
  int sz=requested_output*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE)*m_last_nch;
  memcpy(buffer,m_queue.Get(),sz);
  m_queue.Advance(sz);
  m_queue.Compact();
  return requested_output;
}

void WDL_SimplePitchShifter::PitchShiftBlock(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *inputs, WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *outputs, int nch, int length, double pitch, int bsize, int olsize, double srate)
{
  double iolsize=1.0/olsize;

  WDL_SIMPLEPITCHSHIFT_SAMPLETYPE *psbuf=m_psbuf.Get();

  double dpi=pitch;

  double pspos=m_pspos;
  int writepos=m_pswritepos;
  int writeposnch = writepos*nch;
  int bsizench = bsize*nch;
  int olsizench = olsize*nch;

  int i=length;
  while (i--)
  {
    int ipos1=(int)pspos;
    double frac0=pspos-ipos1;

    ipos1*=nch;

    int ipos2=ipos1+nch;
    
    if (ipos2 >= bsizench) ipos2=0;

    int a;
    for(a=0;a<nch;a++) outputs[a]=(psbuf[ipos1+a]*(1-frac0)+psbuf[ipos2+a]*frac0);      

    double tv=pspos;
    if (dpi >= 1.0)
    {
      if (tv > writepos) tv-=bsize;

      if (tv >= writepos-olsize && tv < writepos)
      {
        double tfrac=(writepos-tv)*iolsize;
        int tmp=ipos1+olsizench;
        if (tmp>=bsizench) tmp-=bsizench;
        int tmp2=tmp+nch;
        if (tmp2 >= bsizench) tmp2=0;

        for(a=0;a<nch;a++) outputs[a]= outputs[a]*tfrac + (1-tfrac)*(psbuf[tmp+a]*(1-frac0)+psbuf[tmp2+a]*frac0);

        if (tv+pitch >= writepos) pspos+=olsize;
      }

    }
    else
    {
      if (tv<writepos) tv+=bsize;

      if (tv >= writepos && tv < writepos+olsize)
      {
        double tfrac=(tv-writepos)*iolsize;
        int tmp=ipos1+olsizench;
        if (tmp>=bsizench) tmp -= bsizench;
        int tmp2= tmp+nch;
        if (tmp2 >= bsizench) tmp2=0;
        for(a=0;a<nch;a++) outputs[a] = outputs[a]*tfrac + (1-tfrac)*(psbuf[tmp+a]*(1-frac0)+psbuf[tmp2+a]*frac0);
        
        // this is wrong, but blehhh?
        if (tv+pitch < writepos+1) pspos += olsize;
//        if (tv+pitch >= writepos+olsize) pspos += olsize;
      }
    }


    if ((pspos+=pitch) >= bsize) pspos -= bsize;


    memcpy(psbuf+writeposnch,inputs,nch*sizeof(WDL_SIMPLEPITCHSHIFT_SAMPLETYPE));

    writeposnch += nch;
    if (++writepos >= bsize) writeposnch = writepos=0;

    inputs += nch;
    outputs += nch;
  } // sample loop
  m_pspos=pspos;
  m_pswritepos=writepos;
}

#endif

#endif
