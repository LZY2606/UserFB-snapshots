/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2026             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: POSIX pipe harness for the oggmux command-line tool

 Exercises a real producer/consumer pair across pipes: a forked
 producer feeds oggmux on its stdin while the parent reads the muxed
 pages from a second pipe.  The parent must observe all BOS pages
 before it re-arms the producer; only then may the producer submit
 media pages and watermarks.  A second handshake makes the producer
 wait until the parent has actually received a non-BOS page, proving
 that safe pages were delivered before end of input.  No sleeps are
 used: only pipe/fork/waitpid and blocking reads.

 Usage: mux_pipe /path/to/oggmux

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ogg/ogg.h>

#define MAX_PAGES 32

struct page_copy {
  unsigned char *bytes;
  long len;
};

struct page_vec {
  struct page_copy pg[MAX_PAGES];
  int n;
};

struct packet_spec {
  long size;
  ogg_int64_t granule;
  int eos;
  unsigned seed;
};

struct track_fixture {
  int serial;
  struct packet_spec pk[4];
  int npkts;
  unsigned char *packet[4];
  long packet_len[4];
  ogg_int64_t packet_gran[4];
  struct page_vec pages;
};

static int write_chunked(int fd, const void *buf, size_t n){
  /* Deliberately write in short blocks so both the pipe reader and the
     sync layer see partial records. */
  const unsigned char *p=(const unsigned char *)buf;
  while(n){
    size_t chunk=n>7?7:n;
    ssize_t w=write(fd,p,chunk);
    if(w<0){
      if(errno==EINTR) continue;
      return 0;
    }
    p+=w;
    n-=(size_t)w;
  }
  return 1;
}

static int read_full(int fd, void *buf, size_t n){
  unsigned char *p=(unsigned char *)buf;
  while(n){
    ssize_t r=read(fd,p,n);
    if(r<0){
      if(errno==EINTR) continue;
      return 0;
    }
    if(r==0) return 0;
    p+=r;
    n-=(size_t)r;
  }
  return 1;
}

static void fill_pattern(unsigned char *p, long n, unsigned seed){
  long i;
  for(i=0;i<n;i++) p[i]=(unsigned char)((seed+(unsigned)i*31u)%251u);
}

static int build_track(struct track_fixture *tf){
  ogg_stream_state os;
  ogg_packet op;
  int i;

  memset(&tf->pages,0,sizeof(tf->pages));
  if(ogg_stream_init(&os,tf->serial)!=0) return 0;

  for(i=0;i<tf->npkts;i++){
    unsigned char *data=(unsigned char *)malloc(tf->pk[i].size>0?
                                                (size_t)tf->pk[i].size:1);
    if(!data) return 0;
    fill_pattern(data,tf->pk[i].size,tf->pk[i].seed);
    tf->packet[i]=data;
    tf->packet_len[i]=tf->pk[i].size;
    tf->packet_gran[i]=tf->pk[i].granule;

    memset(&op,0,sizeof(op));
    op.packet=data;
    op.bytes=tf->pk[i].size;
    op.b_o_s=(i==0);
    op.e_o_s=tf->pk[i].eos;
    op.granulepos=tf->pk[i].granule;
    if(ogg_stream_packetin(&os,&op)!=0) return 0;

    for(;;){
      ogg_page page;
      long bytes;
      unsigned char *copy;
      if(!ogg_stream_flush(&os,&page)) break;
      bytes=page.header_len+page.body_len;
      copy=(unsigned char *)malloc((size_t)bytes);
      if(!copy) return 0;
      memcpy(copy,page.header,(size_t)page.header_len);
      memcpy(copy+page.header_len,page.body,(size_t)page.body_len);
      tf->pages.pg[tf->pages.n].bytes=copy;
      tf->pages.pg[tf->pages.n].len=bytes;
      tf->pages.n++;
    }
  }
  ogg_stream_clear(&os);
  return 1;
}

static void free_track(struct track_fixture *tf){
  int i;
  for(i=0;i<tf->npkts;i++) free(tf->packet[i]);
  for(i=0;i<tf->pages.n;i++) free(tf->pages.pg[i].bytes);
}

static void make_page(ogg_page *page,const unsigned char *bytes,long len){
  page->header=(unsigned char *)bytes;
  page->header_len=bytes[26]+27L;
  page->body=(unsigned char *)bytes+page->header_len;
  page->body_len=len-page->header_len;
}

/* Send one P record followed by its page, in short writes. */
static int send_page_record(int fd,const struct page_copy *pg,int serial){
  char line[64];
  int n=snprintf(line,sizeof(line),"P %d %ld\n",serial,pg->len);
  if(n<=0) return 0;
  if(!write_chunked(fd,line,(size_t)n)) return 0;
  return write_chunked(fd,pg->bytes,(size_t)pg->len);
}

static int send_control(int fd,const char *text){
  return write_chunked(fd,text,strlen(text));
}

/* Read one page from a pipe.  Returns 1 for a page, 0 at clean EOF,
   -1 for malformed/truncated input. */
static int read_page_fd(int fd,ogg_sync_state *oy,ogg_page *page,
                        unsigned char **storage,long *storage_len){
  for(;;){
    long r=ogg_sync_pageout(oy,page);
    if(r==1){
      long len=page->header_len+page->body_len;
      unsigned char *copy;
      if(len>*storage_len){
        unsigned char *tmp=(unsigned char *)realloc(*storage,(size_t)len);
        if(!tmp) return -1;
        *storage=tmp;
        *storage_len=len;
      }
      copy=*storage;
      memcpy(copy,page->header,(size_t)page->header_len);
      memcpy(copy+page->header_len,page->body,(size_t)page->body_len);
      page->header=copy;
      page->body=copy+page->header_len;
      return 1;
    }
    if(r<0) return -1;
    {
      char *buf=ogg_sync_buffer(oy,4096);
      ssize_t got;
      if(!buf) return -1;
      got=read(fd,buf,4096);
      if(got<0){
        if(errno==EINTR) continue;
        return -1;
      }
      if(got==0){
        if(oy->fill!=oy->returned) return -1;
        return 0;
      }
      if(ogg_sync_wrote(oy,(long)got)!=0) return -1;
    }
  }
}

static int decode_track(const struct track_fixture *tf,
                        const struct page_copy *all,int nall){
  ogg_stream_state os;
  ogg_packet op;
  int i,expected=0;

  if(ogg_stream_init(&os,tf->serial)!=0) return 0;
  for(i=0;i<nall;i++){
    ogg_page page;
    make_page(&page,all[i].bytes,all[i].len);
    if(ogg_page_serialno(&page)!=tf->serial) continue;
    if(ogg_stream_pagein(&os,&page)!=0){ ogg_stream_clear(&os); return 0; }
    for(;;){
      int r=ogg_stream_packetout(&os,&op);
      if(r==0) break;
      if(r<0){ ogg_stream_clear(&os); return 0; }
      if(expected>=tf->npkts){ ogg_stream_clear(&os); return 0; }
      if(op.bytes!=tf->packet_len[expected]){ ogg_stream_clear(&os); return 0; }
      if(tf->packet_len[expected]>0 &&
         memcmp(op.packet,tf->packet[expected],
                (size_t)tf->packet_len[expected])!=0){
        ogg_stream_clear(&os);
        return 0;
      }
      if(op.granulepos!=tf->packet_gran[expected]){
        ogg_stream_clear(&os);
        return 0;
      }
      expected++;
    }
  }
  ogg_stream_clear(&os);
  return expected==tf->npkts;
}

int main(int argc,char **argv){
  const char *tool;
  struct track_fixture tf[3];
  int in_pipe[2]={-1,-1},out_pipe[2]={-1,-1},ctrl_pipe[2]={-1,-1};
  pid_t producer=-1,toolpid=-1;
  ogg_sync_state oy;
  ogg_stream_state tool_dec;
  int have_sync=0,have_tool_dec=0;
  int tool_packets=0,tool_bad_serial=0;
  ogg_page page;
  unsigned char *page_storage=NULL;
  long page_storage_len=0;
  struct page_copy got[MAX_PAGES];
  int ngot=0;
  int bos_count=0;
  int media_before_ready=0;
  int armed=0;
  int media_after_arm=0;
  int expect_bos[3]={0,0,0};
  int t,i;
  int status=1;
  int producer_status=0,tool_status=0;

  if(argc<2){
    fprintf(stderr,"usage: %s /path/to/oggmux\n",argv[0]);
    return 2;
  }
  tool=argv[1];
  if(access(tool,X_OK)!=0){
    fprintf(stderr,"mux_pipe: tool missing or not executable: %s\n",tool);
    return 2;
  }

  signal(SIGPIPE,SIG_IGN);
  memset(tf,0,sizeof(tf));
  memset(got,0,sizeof(got));

  /* Three distinguishable tracks.  Every track has a BOS page, a
     packet carried with granule -1 that needs an anchor, and an EOS
     page with the anchor. */
  for(t=0;t<3;t++){
    tf[t].serial=900+t;
    tf[t].pk[0].size=5;
    tf[t].pk[0].granule=0;
    tf[t].pk[0].eos=0;
    tf[t].pk[0].seed=(unsigned)(11+t*31);
    tf[t].pk[1].size=700;
    tf[t].pk[1].granule=-1;
    tf[t].pk[1].eos=0;
    tf[t].pk[1].seed=(unsigned)(101+t*47);
    tf[t].pk[2].size=3;
    tf[t].pk[2].granule=960*(t+1);
    tf[t].pk[2].eos=1;
    tf[t].pk[2].seed=(unsigned)(211+t*13);
    tf[t].npkts=3;
    if(!build_track(&tf[t])){
      fprintf(stderr,"mux_pipe: cannot build track %d\n",t);
      goto cleanup;
    }
  }

  if(pipe(in_pipe)!=0 || pipe(out_pipe)!=0 || pipe(ctrl_pipe)!=0){
    fprintf(stderr,"mux_pipe: pipe failed\n");
    goto cleanup;
  }

  producer=fork();
  if(producer<0){ fprintf(stderr,"mux_pipe: fork failed\n"); goto cleanup; }
  if(producer==0){
    int k,p;
    char line[64];
    close(in_pipe[0]); close(out_pipe[0]); close(out_pipe[1]);
    close(ctrl_pipe[1]);

    /* Phase one: BOS pages only. */
    for(k=0;k<3;k++){
      if(tf[k].pages.n<1 || !send_page_record(in_pipe[1],&tf[k].pages.pg[0],
                                              tf[k].serial))
        _exit(20);
    }
    {
      char c;
      if(!read_full(ctrl_pipe[0],&c,1) || c!='B') _exit(21);
    }

    /* Phase two: the remaining media pages and watermarks. */
    for(k=0;k<3;k++){
      for(p=1;p<tf[k].pages.n;p++){
        if(!send_page_record(in_pipe[1],&tf[k].pages.pg[p],tf[k].serial))
          _exit(22);
      }
      snprintf(line,sizeof(line),"W %d %d\n",tf[k].serial,
               960*(k+1)+1000);
      if(!send_control(in_pipe[1],line)) _exit(23);
    }

    /* Wait until the parent has actually received a non-BOS page. */
    {
      char c;
      if(!read_full(ctrl_pipe[0],&c,1) || c!='M') _exit(24);
    }

    if(!send_control(in_pipe[1],"E\n")) _exit(25);
    close(in_pipe[1]);
    _exit(0);
  }

  toolpid=fork();
  if(toolpid<0){ fprintf(stderr,"mux_pipe: fork failed\n"); goto cleanup; }
  if(toolpid==0){
    close(in_pipe[1]); close(out_pipe[0]);
    close(ctrl_pipe[0]); close(ctrl_pipe[1]);
    if(dup2(in_pipe[0],0)<0 || dup2(out_pipe[1],1)<0) _exit(26);
    close(in_pipe[0]); close(out_pipe[1]);
    execl(tool,tool,"--limit","1048576",
          "900:1:1000","901:1:1000","902:1:1000",(char *)NULL);
    _exit(27);
  }

  close(in_pipe[0]); in_pipe[0]=-1;
  close(in_pipe[1]); in_pipe[1]=-1;
  close(out_pipe[1]); out_pipe[1]=-1;
  close(ctrl_pipe[0]); ctrl_pipe[0]=-1;

  if(ogg_sync_init(&oy)!=0){
    fprintf(stderr,"mux_pipe: sync init failed\n");
    goto reap;
  }
  have_sync=1;

  for(;;){
    int r=read_page_fd(out_pipe[0],&oy,&page,&page_storage,&page_storage_len);
    struct page_copy *slot;
    if(r<0){
      fprintf(stderr,"mux_pipe: malformed or truncated output\n");
      goto reap;
    }
    if(r==0) break;
    if(ngot>=MAX_PAGES){
      fprintf(stderr,"mux_pipe: too many pages\n");
      goto reap;
    }
    slot=&got[ngot++];
    slot->len=page.header_len+page.body_len;
    slot->bytes=(unsigned char *)malloc((size_t)slot->len);
    if(!slot->bytes) goto reap;
    memcpy(slot->bytes,page.header,(size_t)page.header_len);
    memcpy(slot->bytes+page.header_len,page.body,(size_t)page.body_len);

    if(ogg_page_bos(&page)){
      int sn=ogg_page_serialno(&page);
      if(!have_tool_dec){
        if(ogg_stream_init(&tool_dec,sn)!=0){
          fprintf(stderr,"mux_pipe: decoder init failed\n");
          goto reap;
        }
        have_tool_dec=1;
      }
      /* Only feed serial 900 pages to this decoder: a decoder rejects
         pages from any other logical stream. */
      if(sn==900 && ogg_stream_pagein(&tool_dec,&page)!=0){
        fprintf(stderr,"mux_pipe: cannot page BOS into decoder\n");
        goto reap;
      }
      int idx=-1;
      for(i=0;i<3;i++) if(sn==tf[i].serial) idx=i;
      if(idx<0 || expect_bos[idx]){
        fprintf(stderr,"mux_pipe: duplicate or unknown BOS\n");
        goto reap;
      }
      if(idx==0 && sn!=tf[0].serial) tool_bad_serial=1;
      expect_bos[idx]=1;
      bos_count++;
      if(bos_count==3){
        /* BOS order must follow registration order. */
        if(!(expect_bos[0] && expect_bos[1] && expect_bos[2])){
          fprintf(stderr,"mux_pipe: missing BOS\n");
          goto reap;
        }
        if(!write_chunked(ctrl_pipe[1],"B",1)) goto reap;
        armed=1;
      }
      continue;
    }

    if(!armed){
      media_before_ready=1;
      goto reap;
    }
    media_after_arm++;
    if(ogg_page_serialno(&page)==900){
      ogg_packet op;
      while(ogg_stream_packetout(&tool_dec,&op)==1) tool_packets++;
      if(ogg_stream_pagein(&tool_dec,&page)!=0){
        fprintf(stderr,"mux_pipe: cannot page media into decoder\n");
        goto reap;
      }
      while(ogg_stream_packetout(&tool_dec,&op)==1) tool_packets++;
    }
    if(media_after_arm==1){
      /* Deliver the second handshake: this page exists before E. */
      if(!write_chunked(ctrl_pipe[1],"M",1)) goto reap;
    }
  }

  if(bos_count!=3 || media_before_ready || media_after_arm<=0){
    fprintf(stderr,"mux_pipe: BOS/media protocol violated\n");
    goto reap;
  }

  /* Independently decode the first track's portion of the muxed stream
     (pages for serial 900, interleaved with the other tracks). */
  {
    ogg_packet op;
    int n=0;
    while(ogg_stream_packetout(&tool_dec,&op)==1) n++;
    tool_packets+=n;
    if(tool_bad_serial || !have_tool_dec || tool_packets!=3){
      fprintf(stderr,"mux_pipe: independent decode of track 0 failed\n");
      goto reap;
    }
  }

  /* The first three output pages must be the three BOS pages in
     registration order. */
  for(i=0;i<3;i++){
    ogg_page bp;
    if(i>=ngot){
      fprintf(stderr,"mux_pipe: too few output pages\n");
      goto reap;
    }
    make_page(&bp,got[i].bytes,got[i].len);
    if(!ogg_page_bos(&bp) || ogg_page_serialno(&bp)!=tf[i].serial){
      fprintf(stderr,"mux_pipe: BOS ordering wrong\n");
      goto reap;
    }
  }

  for(t=0;t<3;t++){
    if(!decode_track(&tf[t],got,ngot)){
      fprintf(stderr,"mux_pipe: track %d did not decode cleanly\n",t);
      goto reap;
    }
  }

  status=0;

reap:
  if(have_sync) ogg_sync_clear(&oy);
  if(have_tool_dec) ogg_stream_clear(&tool_dec);
  if(ctrl_pipe[1]>=0){ close(ctrl_pipe[1]); ctrl_pipe[1]=-1; }
  if(out_pipe[0]>=0){ close(out_pipe[0]); out_pipe[0]=-1; }

  if(producer>0){
    if(waitpid(producer,&producer_status,0)<0) status=1;
  }
  if(toolpid>0){
    if(waitpid(toolpid,&tool_status,0)<0) status=1;
  }
  if(producer>0){
    if(!WIFEXITED(producer_status) || WEXITSTATUS(producer_status)!=0){
      fprintf(stderr,"mux_pipe: producer failed (%d)\n",
              WIFEXITED(producer_status)?WEXITSTATUS(producer_status):-1);
      status=1;
    }
  }
  if(toolpid>0){
    if(!WIFEXITED(tool_status) || WEXITSTATUS(tool_status)!=0){
      fprintf(stderr,"mux_pipe: oggmux failed (%d)\n",
              WIFEXITED(tool_status)?WEXITSTATUS(tool_status):-1);
      status=1;
    }
  }

cleanup:
  close(in_pipe[0]); close(in_pipe[1]);
  close(out_pipe[0]); close(out_pipe[1]);
  close(ctrl_pipe[0]); close(ctrl_pipe[1]);
  for(t=0;t<3;t++) free_track(&tf[t]);
  for(i=0;i<ngot;i++) free(got[i].bytes);
  free(page_storage);

  if(status==0) printf("MUX_PIPE_OK pages=%d media_before_e=%d\n",
                       ngot,media_after_arm);
  return status;
}
