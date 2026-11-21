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

 function: command-line driver for the multi-stream Ogg mux

 Reads a control/page stream on stdin and writes raw Ogg pages to
 stdout.  Diagnostics go to stderr only.

   oggmux --limit N serial:num:den [serial:num:den ...]

 stdin records:
   P <serial> <bytes>\n   followed by exactly <bytes> raw page bytes
   W <serial> <ticks>\n   advance that track's watermark
   E\n                    end of input; only physical EOF may follow

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <ogg/ogg.h>
#include <ogg/mux.h>

#define LINE_MAX_LEN 512

static int parse_s64(const char *s, ogg_int64_t *out){
  char *end;
  long long v;
  if(!s || !*s) return 0;
  errno=0;
  v=strtoll(s,&end,10);
  if(errno || *end!='\0') return 0;
  *out=(ogg_int64_t)v;
  return 1;
}

/* Strict unsigned byte count in [0, LONG_MAX]. */
static int parse_count(const char *s, long *out){
  char *end;
  long long v;
  if(!s || !*s) return 0;
  errno=0;
  v=strtoll(s,&end,10);
  if(errno || *end!='\0' || v<0 || v>LONG_MAX) return 0;
  *out=(long)v;
  return 1;
}

static int write_all(FILE *out, const unsigned char *p, size_t n){
  while(n){
    size_t w=fwrite(p,1,n,out);
    if(w==0) return 0;
    p+=w;
    n-=w;
  }
  return 1;
}

static int flush_safe(ogg_mux_t *mux, FILE *out){
  ogg_page page;
  for(;;){
    ogg_mux_result_t r=ogg_mux_next_page(mux,&page);
    if(r==OGG_MUX_AGAIN) break;
    if(r!=OGG_MUX_OK){
      fprintf(stderr,"oggmux: next_page failed (%d)\n",(int)r);
      return 0;
    }
    if(!write_all(out,page.header,(size_t)page.header_len) ||
       !write_all(out,page.body,(size_t)page.body_len)){
      fprintf(stderr,"oggmux: write failed\n");
      return 0;
    }
    if(fflush(out)!=0){
      fprintf(stderr,"oggmux: flush failed\n");
      return 0;
    }
  }
  return 1;
}

/* Read a complete line, without consuming the byte past the newline.
   Returns 1 on success, 0 on EOF before any byte, -1 on error or an
   unterminated line at EOF. */
static int read_line(FILE *in, char *buf, size_t size){
  size_t n=0;
  int c;
  for(;;){
    c=fgetc(in);
    if(c==EOF){
      if(n==0) return 0;
      return -1;
    }
    if(c=='\n'){
      if(n>=size) return -1;
      buf[n]='\0';
      return 1;
    }
    if(n+1>=size) return -1;
    buf[n++]=(char)c;
  }
}

/* Read exactly n raw page bytes and confirm they form one complete,
   checksummed page with no trailing residue.  Uses the regular sync
   layer, then copies the page because the sync buffer is reused. */
static int read_page(FILE *in, ogg_sync_state *oy, long bytes,
                     ogg_page *out, unsigned char **storage){
  long got=0;
  char *buf;
  ogg_page page;
  long seek;

  ogg_sync_reset(oy);
  buf=ogg_sync_buffer(oy,bytes);
  if(!buf) return 0;
  while(got<bytes){
    size_t want=(size_t)(bytes-got);
    size_t r=fread(buf+got,1,want,in);
    if(r==0) return 0;
    got+=(long)r;
  }
  if(ogg_sync_wrote(oy,bytes)!=0) return 0;

  if(ogg_sync_pageout(oy,&page)!=1) return 0;
  if(page.header_len+page.body_len!=bytes) return 0;
  seek=ogg_sync_pageseek(oy,&page);
  if(seek!=0) return 0;       /* must be nothing but the one page */

  free(*storage);
  *storage=(unsigned char *)malloc((size_t)bytes);
  if(!*storage) return 0;
  memcpy(*storage,page.header,(size_t)page.header_len);
  memcpy(*storage+page.header_len,page.body,(size_t)page.body_len);
  out->header=*storage;
  out->header_len=page.header_len;
  out->body=*storage+page.header_len;
  out->body_len=page.body_len;
  return 1;
}

static void usage(void){
  fprintf(stderr,
          "usage: oggmux --limit N serial:num:den [serial:num:den ...]\n"
          "reads P/W/E records on stdin, writes raw Ogg pages on stdout\n");
}

int main(int argc, char **argv){
  size_t limit=0;
  int have_limit=0;
  int nspec=0;
  int *serials=NULL;
  ogg_int64_t *nums=NULL;
  ogg_int64_t *dens=NULL;
  ogg_mux_t *mux=NULL;
  ogg_sync_state oy;
  int have_sync=0;
  char line[LINE_MAX_LEN];
  int saw_e=0;
  int status=1;
  unsigned char *page_storage=NULL;
  int i;

  for(i=1;i<argc;i++){
    if(strcmp(argv[i],"--limit")==0){
      long long v;
      char *end;
      if(have_limit || i+1>=argc){ usage(); goto done; }
      errno=0;
      v=strtoll(argv[++i],&end,10);
      if(errno || *end!='\0' || v<=0){ usage(); goto done; }
      limit=(size_t)v;
      have_limit=1;
    }else{
      break;
    }
  }
  if(!have_limit){ usage(); goto done; }

  nspec=argc-i;
  if(nspec<=0){ usage(); goto done; }
  serials=(int *)calloc((size_t)nspec,sizeof(*serials));
  nums=(ogg_int64_t *)calloc((size_t)nspec,sizeof(*nums));
  dens=(ogg_int64_t *)calloc((size_t)nspec,sizeof(*dens));
  if(!serials || !nums || !dens){ fprintf(stderr,"oggmux: out of memory\n"); goto done; }

  for(i=0;i<nspec;i++){
    char *spec=argv[argc-nspec+i];
    char *c1,*c2;
    ogg_int64_t sn,num,den;
    c1=strchr(spec,':');
    if(!c1){ usage(); goto done; }
    *c1='\0';
    c2=strchr(c1+1,':');
    if(!c2){ usage(); goto done; }
    *c2='\0';
    if(!parse_s64(spec,&sn) || !parse_s64(c1+1,&num) || !parse_s64(c2+1,&den)){
      usage(); goto done;
    }
    if(sn<INT_MIN || sn>INT_MAX || num<=0 || den<=0){
      fprintf(stderr,"oggmux: bad track spec %d\n",i);
      goto done;
    }
    serials[i]=(int)sn;
    nums[i]=num;
    dens[i]=den;
  }

  mux=ogg_mux_create(nspec,serials,nums,dens,limit);
  if(!mux){ fprintf(stderr,"oggmux: cannot create mux\n"); goto done; }

  if(ogg_sync_init(&oy)!=0){ fprintf(stderr,"oggmux: sync init failed\n"); goto done; }
  have_sync=1;

  for(;;){
    int rl=read_line(stdin,line,sizeof(line));
    if(rl==0) break;                /* physical EOF */
    if(rl<0){
      fprintf(stderr,"oggmux: truncated control line\n");
      status=2;
      goto done;
    }
    if(line[0]=='E' && line[1]=='\0'){
      saw_e=1;
      break;
    }
    if(line[0]=='P' && line[1]==' '){
      char *ps=line+2;
      char *bs=strchr(ps,' ');
      long bytes;
      ogg_int64_t serial;
      ogg_page page;
      ogg_mux_result_t mr;
      if(!bs){ fprintf(stderr,"oggmux: malformed P record\n"); status=2; goto done; }
      *bs='\0';
      if(!parse_s64(ps,&serial) || serial<INT_MIN || serial>INT_MAX){
        fprintf(stderr,"oggmux: malformed P serial\n"); status=2; goto done;
      }
      if(!parse_count(bs+1,&bytes)){
        fprintf(stderr,"oggmux: malformed P byte count\n"); status=2; goto done;
      }
      if(bytes<=0){ fprintf(stderr,"oggmux: empty page record\n"); status=2; goto done; }
      if(!read_page(stdin,&oy,bytes,&page,&page_storage)){
        fprintf(stderr,"oggmux: truncated or invalid page\n");
        status=2;
        goto done;
      }
      if(ogg_page_serialno(&page)!=(int)serial){
        fprintf(stderr,"oggmux: page serial does not match record\n");
        status=2;
        goto done;
      }
      mr=ogg_mux_submit_page(mux,&page);
      if(mr!=OGG_MUX_OK){
        fprintf(stderr,"oggmux: submit failed (%d)\n",(int)mr);
        status=2;
        goto done;
      }
      if(!flush_safe(mux,stdout)){ status=2; goto done; }
    }else if(line[0]=='W' && line[1]==' '){
      char *ws=line+2;
      char *ts=strchr(ws,' ');
      ogg_int64_t serial,ticks;
      ogg_mux_result_t mr;
      if(!ts){ fprintf(stderr,"oggmux: malformed W record\n"); status=2; goto done; }
      *ts='\0';
      if(!parse_s64(ws,&serial) || serial<INT_MIN || serial>INT_MAX ||
         !parse_s64(ts+1,&ticks) || ticks<0){
        fprintf(stderr,"oggmux: malformed W record\n");
        status=2;
        goto done;
      }
      mr=ogg_mux_advance_frontier(mux,(int)serial,ticks);
      if(mr!=OGG_MUX_OK){
        fprintf(stderr,"oggmux: watermark rejected (%d)\n",(int)mr);
        status=2;
        goto done;
      }
      if(!flush_safe(mux,stdout)){ status=2; goto done; }
    }else{
      fprintf(stderr,"oggmux: unknown control record\n");
      status=2;
      goto done;
    }
  }

  if(!saw_e){
    fprintf(stderr,"oggmux: input ended without E record\n");
    status=3;
    goto done;
  }

  /* E must be the last thing in the stream. */
  {
    int c=fgetc(stdin);
    if(c!=EOF){
      fprintf(stderr,"oggmux: data after E record\n");
      status=2;
      goto done;
    }
  }

  if(ogg_mux_end_input(mux)!=OGG_MUX_OK){
    fprintf(stderr,"oggmux: input incomplete (a track has no EOS)\n");
    status=3;
    goto done;
  }
  if(!flush_safe(mux,stdout)){ status=2; goto done; }

  status=0;

done:
  if(have_sync) ogg_sync_clear(&oy);
  if(mux) ogg_mux_destroy(mux);
  free(serials);
  free(nums);
  free(dens);
  free(page_storage);
  return status;
}
