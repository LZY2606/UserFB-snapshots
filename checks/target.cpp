// checks/target.cpp -- HQ streaming mode checks for WDL_SimplePitchShifter.
//
// Usage: target <clock|spectrum|pitch_tempo|drain>
//
// Each group runs its assertions on self-generated PCM (no external audio,
// no Python, no downloads). On success it prints exactly one line
//   PASS <group> checks=<n>
// and exits 0; any failed assertion prints a FAIL line and the exit code is
// non-zero.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#define WDL_SIMPLEPITCHSHIFT_IMPLEMENT
#include "WDL/simple_pitchshift.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_checks=0;
static int g_failures=0;

static void check(bool ok, const char *msg)
{
  g_checks++;
  if (!ok)
  {
    g_failures++;
    std::printf("FAIL: %s\n",msg);
  }
}

struct Pcm
{
  std::vector<double> v;
  int nch;
  Pcm() : nch(1) {}
  long long frames() const { return nch>0 ? (long long)v.size()/nch : 0; }
};

static std::vector<double> make_tone(int nframes, const std::vector<double> &chgains, double freq)
{
  const int nch=(int)chgains.size();
  std::vector<double> v((size_t)nframes*nch);
  for (int i=0;i<nframes;i++)
  {
    const double s=sin(2.0*M_PI*freq*i);
    for (int c=0;c<nch;c++) v[(size_t)i*nch+c]=s*chgains[c];
  }
  return v;
}

static double project_amp(const double *x, int n, double freq)
{
  double cr=0.0,ci=0.0;
  for (int i=0;i<n;i++)
  {
    cr+=x[i]*cos(2.0*M_PI*freq*i);
    ci+=x[i]*sin(2.0*M_PI*freq*i);
  }
  return 2.0*hypot(cr,ci)/n;
}

static double estimate_freq(const double *x, int n, double fcenter)
{
  double bestf=fcenter,besta=-1.0;
  for (int k=-20;k<=20;k++)
  {
    const double f=fcenter*(1.0+0.0025*k);
    const double a=project_amp(x,n,f);
    if (a>besta) { besta=a; bestf=f; }
  }
  return bestf;
}

static double max_diff(const Pcm &a, const Pcm &b)
{
  if (a.v.size()!=b.v.size()) return 1e30;
  double m=0.0;
  for (size_t i=0;i<a.v.size();i++) m=std::max(m,fabs(a.v[i]-b.v[i]));
  return m;
}

static long long expect_frames(long long n, double tempo)
{
  return (long long)ceil((double)n/tempo-1e-9);
}

// feeds `in` through `s` in blocks, flushes, drains everything, returns frames
static long long feed_and_drain(WDL_SimplePitchShifter &s, const std::vector<double> &in, int nframes, int nch, int block)
{
  long long pos=0,got=0;
  std::vector<double> rd((size_t)4096*nch);
  while (pos<nframes)
  {
    int b=block;
    if ((long long)b>nframes-pos) b=(int)(nframes-pos);
    double *buf=s.GetBuffer(b);
    for (int i=0;i<b*nch;i++) buf[i]=in[(size_t)pos*nch+i];
    s.BufferDone(b);
    pos+=b;
    int m;
    while ((m=s.GetSamples(4096,rd.data()))>0) got+=m;
  }
  s.FlushSamples();
  int m;
  while ((m=s.GetSamples(4096,rd.data()))>0) got+=m;
  return got;
}

static Pcm render(bool hq, double srate, int nch, double shift, double tempo,
                  const std::vector<double> &in, int nframes,
                  const std::vector<int> &blocks, int readcap, bool empty_ops=false)
{
  WDL_SimplePitchShifter s;
  if (hq) check(s.set_hq_mode(true),"set_hq_mode(true) accepted on fresh instance");
  s.set_srate(srate);
  s.set_nch(nch);
  s.set_shift(shift);
  s.set_tempo(tempo);

  Pcm out;
  out.nch=nch;
  std::vector<double> rd((size_t)std::max(readcap,1)*nch);
  long long pos=0;
  size_t bi=0;
  while (pos<nframes)
  {
    int b=blocks[bi++%blocks.size()];
    if ((long long)b>nframes-pos) b=(int)(nframes-pos);
    double *buf=s.GetBuffer(b);
    for (int i=0;i<b*nch;i++) buf[i]=in[(size_t)pos*nch+i];
    s.BufferDone(b);
    pos+=b;
    if (empty_ops)
    {
      double junk[8];
      s.GetBuffer(0);
      s.BufferDone(0); // empty input must not advance the stream
      check(s.GetSamples(0,junk)==0,"GetSamples(0) returns 0");
      check(s.GetSamples(-3,junk)==0,"GetSamples(<0) returns 0");
    }
    int m;
    while ((m=s.GetSamples(readcap,rd.data()))>0)
      out.v.insert(out.v.end(),rd.begin(),rd.begin()+(size_t)m*nch);
  }
  s.FlushSamples();
  int m;
  while ((m=s.GetSamples(readcap,rd.data()))>0)
    out.v.insert(out.v.end(),rd.begin(),rd.begin()+(size_t)m*nch);
  return out;
}

static void group_clock()
{
  const double srate=48000.0;
  const int N=16000;
  const double tempo=3.7, shift=0.8;
  const std::vector<double> in=make_tone(N,std::vector<double>{0.4,-0.4},0.01);
  const long long expect=expect_frames(N,tempo);

  // the stream clock must not depend on the caller's feed block sizes
  const std::vector<std::vector<int> > pats={ {64}, {1}, {1,7,31}, {256} };
  std::vector<Pcm> outs;
  for (size_t i=0;i<pats.size();i++) outs.push_back(render(true,srate,2,shift,tempo,in,N,pats[i],512));
  for (size_t i=0;i<outs.size();i++)
  {
    check(outs[i].frames()==expect,"clock: output length independent of feed block size");
    check(max_diff(outs[0],outs[i])<=2e-6,"clock: output samples independent of feed block size");
  }

  { // ... nor on the GetSamples capacity
    Pcm r1=render(true,srate,2,shift,tempo,in,N,std::vector<int>{7},1);
    Pcm r2=render(true,srate,2,shift,tempo,in,N,std::vector<int>{7},100000);
    check(r1.frames()==expect&&r2.frames()==expect,"clock: length independent of GetSamples capacity");
    check(max_diff(r1,r2)<=2e-6,"clock: samples independent of GetSamples capacity");
  }

  { // empty input / empty reads do not advance
    Pcm e=render(true,srate,2,shift,tempo,in,N,std::vector<int>{64},512,true);
    check(e.frames()==expect&&max_diff(e,outs[0])<=2e-6,"clock: empty input/reads do not advance the stream");
  }

  { // shared read head / tempo position: ch1 == -ch0
    double m=0.0;
    for (long long i=0;i<outs[0].frames();i++) m=std::max(m,fabs(outs[0].v[2*i]+outs[0].v[2*i+1]));
    check(m<=1e-12,"clock: stereo inversion preserved");
  }

  { // 6-channel ratio relations
    std::vector<double> in6=make_tone(4000,std::vector<double>{0.1,0.2,0.3,0.4,0.5,0.6},0.013);
    Pcm o=render(true,srate,6,1.7,2.9,in6,4000,std::vector<int>{31},256);
    check(o.frames()==expect_frames(4000,2.9),"clock: 6ch length");
    double m=0.0;
    for (long long i=0;i<o.frames();i++)
      for (int c=1;c<6;c++) m=std::max(m,fabs(o.v[6*i+c]-o.v[6*i]*(c+1)));
    check(m<=1e-9,"clock: 6ch ratios preserved");
  }

  { // mono
    std::vector<double> in1=make_tone(3000,std::vector<double>{0.5},0.02);
    Pcm o=render(true,srate,1,2.0,0.5,in1,3000,std::vector<int>{1},64);
    check(o.frames()==expect_frames(3000,0.5),"clock: mono length");
  }

  { // configuration validation + stream locking
    WDL_SimplePitchShifter s;
    s.set_hq_mode(true);
    check(!s.set_tempo(0.49),"clock: tempo below [0.5,4] rejected");
    check(!s.set_tempo(4.01),"clock: tempo above [0.5,4] rejected");
    check(!s.set_tempo(NAN),"clock: NaN tempo rejected");
    check(!s.set_shift(0.0),"clock: zero shift rejected");
    check(!s.set_shift(4.5),"clock: shift above range rejected");
    check(!s.set_nch(3),"clock: nch=3 rejected");
    check(!s.set_nch(0),"clock: nch=0 rejected");
    check(!s.set_srate(-48000.0),"clock: negative srate rejected");
    check(!s.set_srate(INFINITY),"clock: infinite srate rejected");
    check(s.set_tempo(0.5)&&s.set_tempo(4.0)&&s.set_tempo(2.0),"clock: boundary tempos accepted");
    check(s.set_shift(0.5)&&s.set_shift(4.0)&&s.set_shift(1.0),"clock: boundary shifts accepted");
    check(s.set_nch(1)&&s.set_nch(2)&&s.set_nch(6),"clock: nch 1/2/6 accepted");
    check(s.set_srate(48000.0),"clock: valid srate accepted");
    s.set_nch(2);
    // lock the configuration by feeding audio
    std::vector<double> clip=make_tone(64,std::vector<double>{0.3,-0.3},0.02);
    double *b=s.GetBuffer(64);
    for (int i=0;i<128;i++) b[i]=clip[i];
    s.BufferDone(64);
    check(!s.set_tempo(3.0),"clock: tempo fixed while streaming");
    check(!s.set_shift(2.0),"clock: shift fixed while streaming");
    check(!s.set_nch(1),"clock: nch fixed while streaming");
    check(!s.set_srate(44100.0),"clock: srate fixed while streaming");
    check(!s.set_hq_mode(false),"clock: mode switch rejected while streaming");
    // rejected switches must not discard audio: the full clip is still delivered
    s.FlushSamples();
    std::vector<double> rd(4096);
    long long got=0;
    int m;
    while ((m=s.GetSamples(2048,rd.data()))>0) got+=m;
    check(got==expect_frames(64,2.0),"clock: rejected switches keep queued audio intact");
    // legacy mode setters stay permissive
    WDL_SimplePitchShifter l;
    check(l.set_tempo(5.0),"clock: legacy tempo setter permissive");
    check(l.set_nch(4),"clock: legacy nch setter permissive");
    check(l.set_hq_mode(false),"clock: set_hq_mode(false) no-op accepted");
    check(l.set_hq_mode(true),"clock: set_hq_mode(true) accepted before audio");
  }

  { // Reset() retains the configuration (including HQ mode) but re-arms the stream
    WDL_SimplePitchShifter s;
    check(s.set_hq_mode(true),"clock: hq enable");
    s.set_srate(srate); s.set_nch(2); s.set_shift(0.8); s.set_tempo(3.7);
    std::vector<double> clip=make_tone(5000,std::vector<double>{0.3,-0.3},0.02);
    long long a=feed_and_drain(s,clip,5000,2,64);
    check(a==expect_frames(5000,3.7),"clock: first clip length");
    s.Reset();
    check(s.get_hq_mode(),"clock: HQ mode survives Reset");
    long long b=feed_and_drain(s,clip,5000,2,64);
    check(b==a,"clock: Reset re-arms with retained configuration");
    s.Reset();
    check(s.set_hq_mode(false),"clock: mode change allowed after Reset");
    long long c=feed_and_drain(s,clip,5000,2,64);
    check(c>0,"clock: legacy mode still processes after mode change");
  }
}

static void group_spectrum()
{
  const double srate=48000.0;
  const int N=32768;
  const double Ts[2]={2.0,4.0};
  for (int ti=0;ti<2;ti++)
  {
    const double T=Ts[ti];
    // shift==tempo: the pitch read head advances exactly 1 frame per input
    // frame, isolating the tempo (resampling) stage
    std::vector<double> in=make_tone(N,std::vector<double>{0.4},0.4);
    Pcm hq=render(true,srate,1,T,T,in,N,std::vector<int>{256},4096);
    Pcm old=render(false,srate,1,T,T,in,N,std::vector<int>{256},4096);
    const double af=fmod(0.4*T,1.0); // folded tone frequency in output frames
    const int skip=800,L=2400; // steady state, away from window/filter transients
    check((long long)hq.frames()>=skip+L&&(long long)old.frames()>=skip+L,"spectrum: enough steady-state frames");
    const double ha=project_amp(&hq.v[skip],L,af);
    const double oa=project_amp(&old.v[skip],L,af);
    check(oa>0.1,"spectrum: legacy linear mode folds the 0.4 cycles/frame tone audibly");
    const double db=ha>0.0?20.0*log10(oa/ha):200.0;
    check(db>=35.0,"spectrum: HQ folded image at least 35 dB below legacy linear mode");

    // passband retention, absolute amplitude (not achieved by rescaling)
    std::vector<double> inp=make_tone(N,std::vector<double>{0.4},0.03);
    Pcm hp=render(true,srate,1,T,T,inp,N,std::vector<int>{256},4096);
    const double pa=project_amp(&hp.v[skip],L,0.03*T);
    check(pa>=0.4/1.12201845&&pa<=0.4*1.12201845,"spectrum: passband amplitude within +/-1 dB");
  }
}

static void group_pitch_tempo()
{
  const double srate=48000.0;
  const int N=20000;
  const double cfgs[2][2]={ {1.25,0.7}, {0.8,3.7} };
  for (int ci=0;ci<2;ci++)
  {
    const double shift=cfgs[ci][0], tempo=cfgs[ci][1];
    const double f0=0.05;
    std::vector<double> in=make_tone(N,std::vector<double>{0.3,-0.3},f0);
    Pcm a=render(true,srate,2,shift,tempo,in,N,std::vector<int>{256},4096);
    Pcm b=render(true,srate,2,shift,tempo,in,N,std::vector<int>{1,7,31},4096);
    Pcm d=render(true,srate,2,shift,tempo,in,N,std::vector<int>{1},4096);
    const long long expect=expect_frames(N,tempo);
    check(a.frames()==expect,"pitch_tempo: exact length");
    check(b.frames()==expect&&d.frames()==expect,"pitch_tempo: length independent of blocking");
    check(max_diff(a,b)<=2e-6&&max_diff(a,d)<=2e-6,"pitch_tempo: full PCM matches across blockings");

    // steady region: skip the pitch window and filter transients at both ends
    const int skip=(int)((2400+64)/tempo)+100;
    const int len=(int)a.frames()-2*skip;
    check(len>1000,"pitch_tempo: steady region exists");
    std::vector<double> mono((size_t)len);
    for (int i=0;i<len;i++) mono[i]=a.v[(size_t)(skip+i)*2];
    const double fexp=f0*shift;
    const double fest=estimate_freq(mono.data(),len,fexp);
    check(fabs(fest-fexp)<=0.02*fexp,"pitch_tempo: pitch within 2% of expected frequency");
    const double amp=project_amp(mono.data(),len,fexp);
    check(amp>0.15&&amp<0.45,"pitch_tempo: tone amplitude sane");

    double m=0.0;
    for (long long i=0;i<a.frames();i++) m=std::max(m,fabs(a.v[2*i]+a.v[2*i+1]));
    check(m<=1e-12,"pitch_tempo: stereo inversion preserved");
  }
}

static void group_drain()
{
  const double srate=48000.0;
  { // exact logical length ceil(N/tempo), incl. clips shorter than the pitch window
    const double tempos[6]={0.5,0.7,1.0,2.0,3.7,4.0};
    const int Ns[5]={1,2,100,1599,16000};
    for (int ti=0;ti<6;ti++) for (int ni=0;ni<5;ni++)
    {
      std::vector<double> in=make_tone(Ns[ni],std::vector<double>{0.3},0.01);
      Pcm o=render(true,srate,1,1.0,tempos[ti],in,Ns[ni],std::vector<int>{64},1024);
      check(o.frames()==expect_frames(Ns[ni],tempos[ti]),"drain: exactly ceil(N/tempo) frames");
    }
  }
  { // unity tempo: transparent, no startup silence, no lost tail
    const int N=5000;
    std::vector<double> in((size_t)N);
    for (int i=0;i<N;i++) in[i]=sin(i*0.05)*0.3+sin(i*0.013)*0.1;
    Pcm o=render(true,srate,1,1.0,1.0,in,N,std::vector<int>{64},1024);
    check(o.frames()==N,"drain: unity length");
    double m=0.0;
    for (int i=0;i<N;i++) m=std::max(m,fabs(o.v[i]-in[i]));
    check(m<1e-9,"drain: unity tempo transparent");
  }
  { // leading impulse: response starts at frame 0 (zero-extended history)
    const int N=2000;
    std::vector<double> in((size_t)N,0.0);
    in[0]=0.8;
    Pcm o=render(true,srate,1,1.0,1.0,in,N,std::vector<int>{64},1024);
    check(o.frames()==N&&fabs(o.v[0]-0.8)<1e-9,"drain: leading impulse at output frame 0");
    double m=0.0;
    for (int i=1;i<N;i++) m=std::max(m,fabs(o.v[i]));
    check(m<1e-9,"drain: no smear after leading impulse");
  }
  { // trailing impulse: the tail is delivered, the stream ends exactly
    const int N=2000;
    std::vector<double> in((size_t)N,0.0);
    in[N-1]=0.8;
    Pcm o=render(true,srate,1,1.0,1.0,in,N,std::vector<int>{64},1024);
    check(o.frames()==N,"drain: trailing impulse length");
    check(fabs(o.v[N-1]-0.8)<1e-9,"drain: trailing impulse at final frame");
    double m=0.0;
    for (int i=0;i<N-1;i++) m=std::max(m,fabs(o.v[i]));
    check(m<1e-9,"drain: no pre-ringing before trailing impulse");
  }
  { // trailing impulse through the sinc stage (tempo=shift=3.7, transparent front)
    const int N=2000;
    std::vector<double> in((size_t)N,0.0);
    in[N-1]=0.8;
    Pcm o=render(true,srate,1,3.7,3.7,in,N,std::vector<int>{64},1024);
    const long long expect=expect_frames(N,3.7);
    check(o.frames()==expect,"drain: sinc tail length");
    double last=0.0;
    for (long long i=expect-20;i<expect;i++) last=std::max(last,fabs(o.v[i]));
    check(last>1e-4,"drain: sinc tail reaches the final frames");
    double peak=0.0;
    for (size_t i=0;i<o.v.size();i++) peak=std::max(peak,fabs(o.v[i]));
    check(peak>0.05,"drain: trailing impulse amplitude preserved");
  }
  { // silence stays silence
    const int N=5000;
    std::vector<double> in((size_t)N,0.0);
    Pcm o=render(true,srate,1,1.0,2.5,in,N,std::vector<int>{64},1024);
    check(o.frames()==expect_frames(N,2.5),"drain: silence length");
    double m=0.0;
    for (size_t i=0;i<o.v.size();i++) m=std::max(m,fabs(o.v[i]));
    check(m==0.0,"drain: silence stays silent");
  }
  { // clip shorter than the pitch window, trailing impulse still delivered
    const int N=300;
    std::vector<double> in((size_t)N,0.0);
    in[N-1]=0.8;
    Pcm o=render(true,srate,1,1.0,1.0,in,N,std::vector<int>{7},64);
    check(o.frames()==N&&fabs(o.v[N-1]-0.8)<1e-9,"drain: short clip tail delivered");
  }
  { // repeated flush / post-flush input / Reset
    WDL_SimplePitchShifter s;
    s.set_hq_mode(true);
    s.set_srate(srate); s.set_nch(1); s.set_shift(1.0); s.set_tempo(3.7);
    std::vector<double> in=make_tone(16000,std::vector<double>{0.3},0.01);
    const long long got=feed_and_drain(s,in,16000,1,256);
    check(got==expect_frames(16000,3.7),"drain: flush length");
    std::vector<double> rd(4096);
    long long more=0;
    int m;
    s.FlushSamples();
    s.FlushSamples(); // repeated flush: no new audio
    while ((m=s.GetSamples(1024,rd.data()))>0) more+=m;
    check(more==0,"drain: repeated flush produces no new audio");
    double *b=s.GetBuffer(100); // input after flush is rejected
    for (int i=0;i<100;i++) b[i]=0.5;
    s.BufferDone(100);
    while ((m=s.GetSamples(1024,rd.data()))>0) more+=m;
    check(more==0,"drain: input after flush rejected");
    check(!s.IsReset(),"drain: rejected input did not silently reset");
    s.Reset();
    const long long got2=feed_and_drain(s,in,16000,1,256);
    check(got2==got,"drain: Reset allows a new clip with retained config");
  }
}

int main(int argc, char **argv)
{
  if (argc<2)
  {
    std::fprintf(stderr,"usage: %s <clock|spectrum|pitch_tempo|drain>\n",argv[0]);
    return 2;
  }
  const std::string g=argv[1];
  if (g=="clock") group_clock();
  else if (g=="spectrum") group_spectrum();
  else if (g=="pitch_tempo") group_pitch_tempo();
  else if (g=="drain") group_drain();
  else
  {
    std::fprintf(stderr,"unknown group '%s'\n",g.c_str());
    return 2;
  }
  if (g_failures)
  {
    std::printf("FAIL %s checks=%d failures=%d\n",g.c_str(),g_checks,g_failures);
    return 1;
  }
  std::printf("PASS %s checks=%d\n",g.c_str(),g_checks);
  return 0;
}
