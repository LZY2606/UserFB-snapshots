#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/metadata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FRAMES 2304
struct state { FLAC__byte *data; size_t used, cap, pos; unsigned frames, cues, reads, writes; int bad, digest; };
static FLAC__int32 sample(unsigned i,unsigned c) { return (FLAC__int32)((i*113u+c*997u)%60001u)-30000; }
static FLAC__StreamEncoderWriteStatus put(const FLAC__StreamEncoder *e,const FLAC__byte *b,size_t n,uint32_t samples,uint32_t frame,void *u) {
  struct state *s=u; size_t end=s->pos+n;(void)e;(void)samples;(void)frame;
  if(end<s->pos) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  if(end>s->cap) {size_t cap=end+4096;void *p=realloc(s->data,cap);if(!p)return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;s->data=p;s->cap=cap;}
  memcpy(s->data+s->pos,b,n);s->pos=end;if(end>s->used)s->used=end;return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
static FLAC__StreamEncoderSeekStatus seekout(const FLAC__StreamEncoder *e,FLAC__uint64 pos,void *u) {
  struct state *s=u;(void)e;if(pos>s->used)return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;s->pos=(size_t)pos;return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}
static FLAC__StreamEncoderTellStatus tellout(const FLAC__StreamEncoder *e,FLAC__uint64 *pos,void *u) {
  (void)e;*pos=((struct state*)u)->pos;return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderReadStatus get(const FLAC__StreamDecoder *d,FLAC__byte *b,size_t *n,void *u) {
  struct state *s=u;(void)d;++s->reads;if(!*n)return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  if(s->pos==s->used){*n=0;return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;}
  *n=1;*b=s->data[s->pos++];return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderWriteStatus pcm(const FLAC__StreamDecoder *d,const FLAC__Frame *f,const FLAC__int32 *const b[],void *u) {
  struct state *s=u;unsigned i,c;(void)d;++s->writes;
  if(f->header.channels!=2||f->header.bits_per_sample!=16||s->frames+f->header.blocksize>FRAMES)s->bad=1;
  if(s->bad)return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  for(i=0;i<f->header.blocksize;++i)for(c=0;c<2;++c)if(b[c][i]!=sample(s->frames+i,c))s->bad=1;
  s->frames+=f->header.blocksize;return s->bad?FLAC__STREAM_DECODER_WRITE_STATUS_ABORT:FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void meta(const FLAC__StreamDecoder *d,const FLAC__StreamMetadata *m,void *u) {
  struct state *s=u;(void)d;
  if(m->type==FLAC__METADATA_TYPE_STREAMINFO){unsigned i;for(i=0;i<16;++i)s->digest|=m->data.stream_info.md5sum[i];if(m->data.stream_info.total_samples!=FRAMES)s->bad=1;}
  if(m->type==FLAC__METADATA_TYPE_CUESHEET){const FLAC__StreamMetadata_CueSheet *c=&m->data.cue_sheet;s->cues=c->num_tracks-1;if(c->num_tracks!=4||c->tracks[1].offset!=713||c->tracks[1].indices[1].offset!=47||c->tracks[3].offset!=FRAMES)s->bad=1;}
}
static void error(const FLAC__StreamDecoder *d,FLAC__StreamDecoderErrorStatus e,void *u) {(void)d;(void)e;((struct state*)u)->bad=1;}
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  struct state s={0};FLAC__StreamEncoder *e=FLAC__stream_encoder_new();FLAC__StreamDecoder *d=FLAC__stream_decoder_new();
  FLAC__StreamMetadata *cue=FLAC__metadata_object_new(FLAC__METADATA_TYPE_CUESHEET);FLAC__StreamMetadata *md[1];
  FLAC__int32 input[FRAMES*2];const FLAC__uint64 off[4]={0,713,1491,FRAMES};const FLAC__uint64 pregap[3]={29,47,101};unsigned i,c;
  REQUIRE(e&&d&&cue);REQUIRE(FLAC__metadata_object_cuesheet_resize_tracks(cue,4));
  for(i=0;i<4;++i){cue->data.cue_sheet.tracks[i].number=i==3?170:i+1;cue->data.cue_sheet.tracks[i].offset=off[i];if(i<3){REQUIRE(FLAC__metadata_object_cuesheet_track_resize_indices(cue,i,2));cue->data.cue_sheet.tracks[i].indices[0].number=0;cue->data.cue_sheet.tracks[i].indices[0].offset=0;cue->data.cue_sheet.tracks[i].indices[1].number=1;cue->data.cue_sheet.tracks[i].indices[1].offset=pregap[i];}}
  REQUIRE(FLAC__metadata_object_cuesheet_is_legal(cue,0,NULL));md[0]=cue;
  for(i=0;i<FRAMES;++i)for(c=0;c<2;++c)input[i*2+c]=sample(i,c);
  REQUIRE(FLAC__stream_encoder_set_channels(e,2));REQUIRE(FLAC__stream_encoder_set_bits_per_sample(e,16));REQUIRE(FLAC__stream_encoder_set_sample_rate(e,44100));REQUIRE(FLAC__stream_encoder_set_blocksize(e,576));REQUIRE(FLAC__stream_encoder_set_total_samples_estimate(e,FRAMES));REQUIRE(FLAC__stream_encoder_set_metadata(e,md,1));
  REQUIRE(FLAC__stream_encoder_init_stream(e,put,seekout,tellout,NULL,&s)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
  REQUIRE(FLAC__stream_encoder_process_interleaved(e,input,FRAMES));REQUIRE(FLAC__stream_encoder_finish(e));FLAC__stream_encoder_delete(e);FLAC__metadata_object_delete(cue);
  s.pos=0;REQUIRE(FLAC__stream_decoder_set_md5_checking(d,1));REQUIRE(FLAC__stream_decoder_set_metadata_respond(d,FLAC__METADATA_TYPE_CUESHEET));
  REQUIRE(FLAC__stream_decoder_init_stream(d,get,NULL,NULL,NULL,NULL,pcm,meta,error,&s)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
  REQUIRE(FLAC__stream_decoder_process_until_end_of_stream(d));REQUIRE(FLAC__stream_decoder_finish(d));FLAC__stream_decoder_delete(d);
  REQUIRE(!s.bad&&s.frames==FRAMES&&s.cues==3&&s.digest&&s.reads>s.used&&s.writes==4);
  printf("FLAC_SOURCE_ROUNDTRIP_OK samples=%u tracks=%u md5=verified decoder_frames=%u one_byte_reads=%u\n",s.frames,s.cues,s.writes,s.reads);
  free(s.data);return 0;
}
