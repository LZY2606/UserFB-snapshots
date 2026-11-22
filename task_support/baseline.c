#define main upstream_indexing_main
#include "../test/indexing.c"
#undef main
#include <stdio.h>
static void track_entry(struct buffer *b,uint64_t number,uint64_t uid,int audio) {
    struct buffer t={0};buffer_put_uint(&t,ID_TRACK_NUMBER,number);buffer_put_uint(&t,ID_TRACK_UID,uid);
    buffer_put_uint(&t,ID_TRACK_TYPE,audio?2:1);buffer_put_string(&t,ID_CODEC_ID,audio?"A_OPUS":"V_VP8");
    buffer_put_master(b,ID_TRACK_ENTRY,&t);buffer_destroy(&t);
}
static void media(struct buffer *c,uint64_t number,unsigned char marker) {
    struct buffer b={0};buffer_put_vint(&b,number);buffer_put_be(&b,0,2);
    buffer_put_byte(&b,0x80);buffer_put_byte(&b,marker);
    buffer_put_master(c,ID_SIMPLE_BLOCK,&b);buffer_destroy(&b);
}
static void member(struct buffer *file,int second) {
    struct buffer seg={0},info={0},tracks={0},cp={0},cluster={0};
    double duration=second?100.0:200.0;uint64_t bits;
    buffer_put_uint(&info,0x2ad7b1,second?2000000:1000000);
    memcpy(&bits,&duration,8);buffer_put_id(&info,0x4489);buffer_put_size(&info,8);buffer_put_be(&info,bits,8);
    buffer_put_master(&seg,0x1549a966,&info);
    if(second){track_entry(&tracks,9,200,1);track_entry(&tracks,77,100,0);}
    else{track_entry(&tracks,42,100,0);track_entry(&tracks,7,200,1);}
    buffer_put_master(&seg,ID_TRACKS,&tracks);
    buffer_put_uint(&cp,ID_TIMECODE,second?5:40);
    if(second)media(&cp,77,0xa2);else{media(&cp,42,0xa1);media(&cp,7,0xb1);}
    buffer_put_master(&cluster,ID_CLUSTER,&cp);buffer_put(&seg,cluster.data,cluster.length);
    put_ebml_header(file);buffer_put_master(file,ID_SEGMENT,&seg);
    buffer_destroy(&seg);buffer_destroy(&info);buffer_destroy(&tracks);buffer_destroy(&cp);buffer_destroy(&cluster);
}
int main(void) {
    struct buffer joined={0},second={0};struct buffer_io io;nestegg *ctx=NULL;nestegg_packet *p=NULL;
    unsigned n=0,tr;uint64_t ts;unsigned char *data;size_t length;int status;
    member(&joined,0);member(&second,1);buffer_put(&joined,second.data,second.length);
    assert(init_from_buffer(&ctx,&joined,&io)==0);
    while((status=nestegg_read_packet(ctx,&p))==1){
        assert(nestegg_packet_tstamp(p,&ts)==0 && ts==40000000);
        assert(nestegg_packet_track(p,&tr)==0 && tr==n);
        assert(nestegg_packet_data(p,0,&data,&length)==0 && length==1);
        assert(data[0]==(n==0?0xa1:0xb1));++n;nestegg_free_packet(p);
    }
    assert(status==0 && n==2);puts("legacy concatenated members: packets=2 final_status=0");nestegg_destroy(ctx);
    assert(init_from_buffer(&ctx,&second,&io)==0);assert(nestegg_read_packet(ctx,&p)==1);
    assert(nestegg_packet_track(p,&tr)==0 && tr==1);
    assert(nestegg_packet_tstamp(p,&ts)==0 && ts==10000000);
    assert(nestegg_packet_data(p,0,&data,&length)==0 && length==1 && data[0]==0xa2);
    puts("second standalone: track=1 local_ns=10000000 payload=a2");
    nestegg_free_packet(p);nestegg_destroy(ctx);buffer_destroy(&joined);buffer_destroy(&second);
    puts("BASELINE_COMPLETE single Segment parser on concatenated WebM");return 0;
}
