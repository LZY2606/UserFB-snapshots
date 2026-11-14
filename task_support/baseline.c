/* Reuse the fixed upstream's public, ISC-licensed test fixture helpers. */
#define main upstream_indexing_main
#include "../test/indexing.c"
#undef main
#include <stdio.h>

static void check_group(void) {
  struct buffer file={0}, seg={0}, info={0}, group={0}, cluster={0}, block={0};
  struct buffer_io io;
  nestegg *ctx=NULL;
  nestegg_packet *pkt=NULL;
  unsigned int count, track;
  uint64_t ts, duration;
  int64_t padding, reference;
  unsigned char *data;
  size_t length;
  unsigned char payload[64];
  size_t i;
  uint64_t track_number=17;
  for(i=0;i<sizeof(payload);i++) payload[i]=(unsigned char)(3*i+1);
  buffer_put_uint(&info,0x2ad7b1,1000000);
  buffer_put_master(&seg,0x1549a966,&info);
  put_tracks(&seg,&track_number,1);
  buffer_put_uint(&cluster,ID_TIMECODE,20);
  buffer_put_vint(&block,17);
  buffer_put_be(&block,0xfffe,2); /* -2 ticks */
  buffer_put_byte(&block,0);
  buffer_put(&block,payload,sizeof(payload));
  buffer_put_master(&group,0xa1,&block);
  buffer_put_uint(&group,0x9b,4);
  buffer_put_uint(&group,0x75a2,200000);
  buffer_put_uint(&group,0xfb,0);
  buffer_put_master(&cluster,0xa0,&group);
  buffer_put_master(&seg,ID_CLUSTER,&cluster);
  put_ebml_header(&file);
  buffer_put_master(&file,ID_SEGMENT,&seg);
  assert(init_from_buffer(&ctx,&file,&io)==0);
  assert(nestegg_read_packet(ctx,&pkt)==1);
  assert(nestegg_packet_track(pkt,&track)==0 && track==0);
  assert(nestegg_packet_count(pkt,&count)==0 && count==1);
  assert(nestegg_packet_tstamp(pkt,&ts)==0 && ts==18000000);
  assert(nestegg_packet_duration(pkt,&duration)==0 && duration==4000000);
  assert(nestegg_packet_discard_padding(pkt,&padding)==0 && padding==200000);
  assert(nestegg_packet_reference_block(pkt,&reference)==0 && reference==0);
  assert(nestegg_packet_has_keyframe(pkt)==NESTEGG_PACKET_HAS_KEYFRAME_FALSE);
  assert(nestegg_packet_data(pkt,0,&data,&length)==0 && length==sizeof(payload));
  assert(memcmp(data,payload,length)==0);
  nestegg_free_packet(pkt); pkt=NULL;
  assert(nestegg_read_packet(ctx,&pkt)==0 && pkt==NULL);
  nestegg_destroy(ctx);
  buffer_destroy(&file); buffer_destroy(&seg); buffer_destroy(&info);
  buffer_destroy(&group); buffer_destroy(&cluster); buffer_destroy(&block);
  puts("BASELINE_GROUP_OK timestamp=18000000 duration=4000000 padding=200000 reference=0 bytes=64");
}
int main(void) {
  test_indexed_access();
  puts("BASELINE_INDEXED_OK tracks=3 lace=256 cue_seek=1");
  check_group();
  return 0;
}
