#include <audiofile.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
  char name[256];
  snprintf(name, sizeof name, "%s/aiff-production-XXXXXX", tmpdir);
  int fd=mkstemp(name); CHECK(fd>=0); close(fd);
  struct Cleanup { char *p; ~Cleanup(){unlink(p);} } cleanup={name};
  AFfilesetup s=afNewFileSetup(); CHECK(s);
  afInitFileFormat(s,AF_FILE_AIFF); afInitChannels(s,AF_DEFAULT_TRACK,2);
  afInitSampleFormat(s,AF_DEFAULT_TRACK,AF_SAMPFMT_TWOSCOMP,16);
  afInitRate(s,AF_DEFAULT_TRACK,48000);
  int marks[]={1,2,3}, inst=AF_DEFAULT_INST, loops[]={1,2};
  afInitMarkIDs(s,AF_DEFAULT_TRACK,marks,3);
  afInitMarkName(s,AF_DEFAULT_TRACK,1,"left anchor");
  afInitMarkName(s,AF_DEFAULT_TRACK,2,"right anchor");
  afInitMarkName(s,AF_DEFAULT_TRACK,3,"end");
  afInitInstIDs(s,&inst,1); afInitLoopIDs(s,inst,loops,2);
  AFfilehandle w=afOpenFile(name,"w",s); CHECK(w); afFreeFileSetup(s);
  int16_t pcm[]={120,-310,2000,-41,-30000,29000,32767,-32768,71,-92,800,-900};
  CHECK(afWriteFrames(w,AF_DEFAULT_TRACK,pcm,6)==6);
  afSetMarkPosition(w,AF_DEFAULT_TRACK,1,1);
  afSetMarkPosition(w,AF_DEFAULT_TRACK,2,4);
  afSetMarkPosition(w,AF_DEFAULT_TRACK,3,6);
  afSetLoopStart(w,inst,1,1); afSetLoopEnd(w,inst,1,2);
  afSetLoopMode(w,inst,1,AF_LOOP_MODE_FORWBAKW);
  afSetLoopStart(w,inst,2,2); afSetLoopEnd(w,inst,2,3);
  CHECK(afCloseFile(w)==0);
  AFfilehandle r=afOpenFile(name,"r+",NULL); CHECK(r);
  CHECK(afGetFrameCount(r,AF_DEFAULT_TRACK)==6);
  CHECK(afGetMarkPosition(r,AF_DEFAULT_TRACK,2)==4);
  CHECK(!std::strcmp(afGetMarkName(r,AF_DEFAULT_TRACK,1),"left anchor"));
  CHECK(afGetLoopStartFrame(r,inst,1)==1 && afGetLoopEndFrame(r,inst,1)==4);
  CHECK(afGetLoopMode(r,inst,1)==AF_LOOP_MODE_FORWBAKW);
  int16_t got[12]={0};
  CHECK(afReadFrames(r,AF_DEFAULT_TRACK,got,2)==2);
  CHECK(afTellFrame(r,AF_DEFAULT_TRACK)==2);
  CHECK(afReadFrames(r,AF_DEFAULT_TRACK,got+4,4)==4);
  CHECK(!std::memcmp(got,pcm,sizeof pcm));
  CHECK(afSeekFrame(r,AF_DEFAULT_TRACK,3)==3);
  CHECK(afReadFrames(r,AF_DEFAULT_TRACK,got,1)==1);
  CHECK(got[0]==32767 && got[1]==-32768);
  CHECK(afCloseFile(r)==0);
  std::puts("AIFF_PCM_MARK_INST_OK frames=6 channels=2 anchor=1:4");
  return 0;
}
