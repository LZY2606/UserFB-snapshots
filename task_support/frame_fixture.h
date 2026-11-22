#ifndef FRAME_FIXTURE_H
#define FRAME_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Synthetic MPEG-1 Layer III, table-1 pair (1,0), long blocks.
 * No downloaded audio or frozen decoder output. Input construction only. */
static void fixture_bits(unsigned char *out, unsigned *pos, unsigned value, unsigned count) {
    while(count) { --count; if((value>>count)&1u) out[*pos/8]|=(unsigned char)(1u<<(7-*pos%8)); ++*pos; }
}
static unsigned fixture_crc_bit(unsigned crc, unsigned bit) {
    unsigned msb=(crc>>15)^bit;
    return ((crc<<1)^((msb&1)?0x8005u:0u))&65535u;
}
/* MPEG-1 Layer III bitrates in kbps, indexed by the header bitrate_index. */
static const unsigned fixture_bitrates[15] =
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
/* Write 'count' bits of 'value' at reservoir-relative bit offset *rbit for a
 * frame whose own main data starts 'main_off' bytes into 'out'. The first
 * 'mdb' reservoir bytes live in the previous frame's tail (out[-mdb]...),
 * the rest starts at out[main_off]. */
static void fixture_reservoir_bits(unsigned char *out, size_t main_off, unsigned mdb,
                                   unsigned *rbit, unsigned value, unsigned count) {
    while(count) {
        size_t rbyte;
        unsigned char *p;
        --count;
        rbyte = *rbit/8;
        p = (rbyte < mdb) ? (out - mdb + rbyte) : (out + main_off + (rbyte - mdb));
        if((value>>count)&1u) p[0] |= (unsigned char)(1u<<(7-*rbit%8));
        ++*rbit;
    }
}
/* Extended frame builder.
 * mode: 0=stereo, 1=joint stereo, 2=dual channel, 3=mono.
 * bitrate_index: 1..14 into fixture_bitrates.
 * main_data_begin: bytes of this frame's main data carried in the previous
 *   frame's tail; the spectral payload is then written into that previous
 *   frame, which must already have been emitted into the same buffer with a
 *   main data area of at least main_data_begin bytes. Use 0 for the first
 *   frame of a segment. */
static size_t fixture_frame_ex(unsigned char *out, unsigned rate_index, unsigned mode,
                               unsigned bitrate_index, unsigned padding, unsigned crc_on,
                               unsigned main_data_begin, unsigned phase) {
    static const unsigned rates[]={44100,48000,32000};
    unsigned channels = (mode==3)?1u:2u;
    size_t size;
    uint32_t head;
    unsigned side,pos,end,gr,ch,rbit;
    if(rate_index>2 || mode>3 || bitrate_index==0 || bitrate_index>14) return 0;
    if(main_data_begin>511) return 0;
    size = 144u*fixture_bitrates[bitrate_index]*1000u/rates[rate_index]+padding;
    memset(out,0,size);
    head = 0xfff00000u|(1u<<19)|(1u<<17)|((crc_on?0u:1u)<<16)|(bitrate_index<<12)|
           (rate_index<<10)|((padding?1u:0u)<<9)|(mode<<6);
    for(int i=0;i<4;i++) out[i]=(unsigned char)(head>>(24-8*i));
    side=4+(crc_on?2:0),pos=side*8;
    fixture_bits(out,&pos,main_data_begin,9);
    fixture_bits(out,&pos,0,channels==1?5:3);
    for(ch=0;ch<channels;ch++)fixture_bits(out,&pos,0,4); /* scfsi */
    for(gr=0;gr<2;gr++)for(ch=0;ch<channels;ch++) {
        fixture_bits(out,&pos,3,12); /* part2_3_length: one pair plus sign */
        fixture_bits(out,&pos,1,9); fixture_bits(out,&pos,230,8);
        fixture_bits(out,&pos,0,4); fixture_bits(out,&pos,0,1);
        fixture_bits(out,&pos,1,5); fixture_bits(out,&pos,0,5); fixture_bits(out,&pos,0,5);
        fixture_bits(out,&pos,0,4); fixture_bits(out,&pos,0,3);
        fixture_bits(out,&pos,0,1); fixture_bits(out,&pos,0,1); fixture_bits(out,&pos,0,1);
    }
    end=side+(channels==1?17:32);
    if(pos!=end*8)return 0;
    /* Spectral payload at reservoir offset 0: inside this frame when
     * main_data_begin==0, in the previous frame's tail otherwise. */
    rbit=0;
    for(gr=0;gr<2;gr++)for(ch=0;ch<channels;ch++) {
        fixture_reservoir_bits(out,end,main_data_begin,&rbit,1,2); /* table 1: 01 => (1,0) */
        fixture_reservoir_bits(out,end,main_data_begin,&rbit,(phase+gr+ch)&1u,1);
    }
    if(crc_on) {
        unsigned crc=65535;
        for(unsigned i=16;i<32;i++)crc=fixture_crc_bit(crc,(head>>(31-i))&1u);
        for(unsigned i=side*8;i<end*8;i++)crc=fixture_crc_bit(crc,(out[i/8]>>(7-i%8))&1u);
        out[4]=(unsigned char)(crc>>8);out[5]=(unsigned char)crc;
    }
    return size;
}
static size_t fixture_frame(unsigned char *out, unsigned rate_index, unsigned channels,
                            unsigned padding, unsigned crc_on, unsigned phase) {
    return fixture_frame_ex(out,rate_index,channels==1?3u:0u,9u,padding,crc_on,0u,phase);
}
#endif
