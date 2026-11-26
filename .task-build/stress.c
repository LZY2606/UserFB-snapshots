#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libmpegts.h"

static uint32_t crc_mpeg(const uint8_t *s, size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=(uint32_t)s[i]<<24;for(int j=0;j<8;j++)c=(c<<1)^((c&0x80000000u)?0x04c11db7u:0);}return c;}
static int pidof(const uint8_t*p){return ((p[1]&31)<<8)|p[2];}
static int pusi(const uint8_t*p){return p[1]&64;}
static int afc(const uint8_t*p){return (p[3]>>4)&3;}
static int ppos(const uint8_t*p){int q=4;if(afc(p)&2)q=5+p[4];return q;}

typedef struct { uint8_t *buf; size_t len,npk; int64_t *clk; } sink;
static int coll(ts_writer_t*w,sink*s,ts_frame_t*f,int n){
    uint8_t*o;int l;int64_t*c;
    if(ts_write_frames(w,f,n,&o,&l,&c)<0)return -1;
    if(l%188)return -2;
    size_t a=l/188;
    s->buf=realloc(s->buf,s->len+l); s->clk=realloc(s->clk,(s->npk+a)*8);
    memcpy(s->buf+s->len,o,l); memcpy(s->clk+s->npk,c,a*8);
    s->len+=l;s->npk+=a; return 0;
}

/* reassemble one pid PES ES blobs + first/last pkt */
typedef struct { uint8_t*es; int len,first,last; int64_t pts; } pes;
static int getpes(const uint8_t*ts,size_t n,int pid,pes**out,int*cnt){
    *out=NULL;*cnt=0; uint8_t*b=malloc(1<<20); int len=0,cap=1<<20,first=-1,last=-1;
    for(size_t k=0;k<n;k++){const uint8_t*p=ts+k*188;
        if(pidof(p)==pid&&pusi(p)&&(afc(p)&1)){
            if(len>9){int hl=9+b[8]; pes*z=realloc(*out,(*cnt+1)*sizeof(pes));*out=z;
                z[*cnt].len=len-hl;z[*cnt].es=malloc(z[*cnt].len);memcpy(z[*cnt].es,b+hl,z[*cnt].len);
                z[*cnt].first=first;z[*cnt].last=last;
                z[*cnt].pts=(((uint64_t)(b[9]&14)<<29)|((uint64_t)b[10]<<22)|((uint64_t)(b[11]&254)<<14)|((uint64_t)b[12]<<7)|(b[13]>>1));
                (*cnt)++;}
            len=0;first=k;
        }
        if(pidof(p)!=pid||!(afc(p)&1))continue;
        int q=ppos(p),av=188-q; if(av<=0)continue;
        if(len+av>cap){cap*=2;b=realloc(b,cap);}
        memcpy(b+len,p+q,av);len+=av;last=k;
    }
    if(len>9){int hl=9+b[8];pes*z=realloc(*out,(*cnt+1)*sizeof(pes));*out=z;
        z[*cnt].len=len-hl;z[*cnt].es=malloc(z[*cnt].len);memcpy(z[*cnt].es,b+hl,z[*cnt].len);
        z[*cnt].first=first;z[*cnt].last=last;(*cnt)++;}
    free(b);return 0;
}

static uint64_t rnd(int64_t x,int m){return (x+m/2)/m;}

static int run_seed(unsigned seed,int percall){
    srand(seed*7919+13);
    int N=8;                 /* max programs fairness run */
    int muxset[]={2000000,4000000,8000000};
    int mux=8000000;
    int NF=5+seed%5;                /* frames/prog */
    int sizes[]={200,400,640,800};
    int slot=(int)((int64_t)188*8*27000000/mux);

    ts_stream_t st[8]; ts_program_t pr[8];
    memset(st,0,sizeof st);memset(pr,0,sizeof pr);
    int vpid[8]; static uint8_t bodies[8][12][1400];
    static int64_t INI[8][12],FIN[8][12]; static int SZ[8][12];
    for(int p=0;p<N;p++){
        vpid[p]=0x100+p*0x100;
        st[p].pid=vpid[p];st[p].stream_format=(p&1)?LIBMPEGTS_VIDEO_AVC:LIBMPEGTS_VIDEO_MPEG2;
        st[p].stream_id=0xe0;
        pr[p].pmt_pid=0x1000+p;pr[p].program_num=p+1;pr[p].pcr_pid=(seed&1)?0x1100+p:vpid[p];
        pr[p].num_streams=1;pr[p].streams=&st[p];
    }
    ts_main_t m={0};m.num_programs=N;m.programs=pr;m.muxrate=mux;m.cbr=1;m.lowlatency=1;
    m.ts_type=TS_TYPE_GENERIC;m.pcr_period=20;m.pat_period=80;
    ts_writer_t*w=ts_create_writer();
    if(ts_setup_transport_stream(w,&m)<0){printf("setup fail seed %u\n",seed);return 1;}
    for(int p=0;p<N;p++)
        if(ts_setup_mpegvideo_stream(w,vpid[p],(p&1)?30:LIBMPEGTS_MPEG2_LEVEL_MAIN,
            (p&1)?AVC_HIGH:LIBMPEGTS_MPEG2_PROFILE_MAIN,2000000,200000,3)<0){printf("vsetup fail\n");return 1;}

    sink s={0};
    /* build frames sorted by final arrival so per-call batching stays monotonic */
    int total=N*NF;
    ts_frame_t *all=calloc(total,sizeof(ts_frame_t));
    uint8_t **keep=calloc(total,sizeof(uint8_t*));
    int idx=0;
    for(int i=0;i<NF;i++)for(int p=0;p<N;p++){
        int sz=sizes[(seed+i+p)%4]; SZ[p][i]=sz;
        for(int j=0;j<sz;j++)bodies[p][i][j]=(uint8_t)(j*131+i*17+p*7+seed);
        INI[p][i]=(int64_t)(i*3600+700+(p*97))*300;
        FIN[p][i]=(int64_t)(i*3600+3600)*300;
        ts_frame_t f={0};f.pid=vpid[p];f.data=bodies[p][i];f.size=sz;
        f.dts=9000+i*3600+p*90;f.pts=f.dts;
        f.cpb_initial_arrival_time=INI[p][i];f.cpb_final_arrival_time=FIN[p][i];
        f.random_access=(i==0);
        all[idx++]=f;
    }
    if(!percall){
        if(coll(w,&s,all,total)){printf("coll fail\n");return 1;}
        if(coll(w,&s,NULL,0))return 1;
    }else{
        /* one monotonic index group (all programs) per call */
        for(int i=0;i<NF;i++){ if(coll(w,&s,&all[i*N],N)){printf("pc coll fail\n");return 1;} }
        if(coll(w,&s,NULL,0))return 1;
    }
    free(all);free(keep);

    /* clock + cc */
    for(size_t k=0;k<s.npk;k++){
        int64_t ex=270000000LL+rnd((int64_t)(k+1)*188*8*27000000LL,mux);
        if(llabs(s.clk[k]-ex)>2){printf("clock seed %u k%zu d%lld\n",seed,k,(long long)(s.clk[k]-ex));return 1;}
    }
    int have[8192]={0},last[8192]={0};
    for(size_t k=0;k<s.npk;k++){const uint8_t*p=s.buf+k*188;int pid=pidof(p);if(pid==0x1fff)continue;
        int a=afc(p),cc=p[3]&15;
        if(a&1){if(have[pid]&&cc!=((last[pid]+1)&15)){printf("CC seed %u pid%x k%zu\n",seed,pid,k);return 1;}last[pid]=cc;have[pid]=1;}
        else if(a==2&&have[pid]&&cc!=last[pid]){printf("ACO seed %u pid%x k%zu cc%d last%d\n",seed,pid,k,cc,last[pid]);return 1;}
    }

    for(int p=0;p<N;p++){
        pes*z;int cnt;getpes(s.buf,s.npk,vpid[p],&z,&cnt);
        if(cnt!=NF){printf("seed %u prog %d pescnt %d/%d\n",seed,p,cnt,NF);return 1;}
        for(int i=0;i<cnt;i++){
            if(z[i].len!=SZ[p][i]){printf("seed %u p%d i%d len %d/%d\n",seed,p,i,z[i].len,SZ[p][i]);return 1;}
            if(memcmp(z[i].es,bodies[p][i],SZ[p][i])){printf("content mismatch seed %u p%d i%d\n",seed,p,i);return 1;}
            int64_t ft=s.clk[z[i].first]-270000000LL, lt=s.clk[z[i].last]-270000000LL;
            if(ft<INI[p][i]-slot){printf("EARLY seed %u mux%d p%d i%d ft%lld ini%lld slot%d\n",seed,mux,p,i,(long long)ft,(long long)INI[p][i],slot);return 1;}
            if(lt>FIN[p][i]+slot){printf("LATE seed %u mux%d p%d i%d lt%lld fin%lld slot%d\n",seed,mux,p,i,(long long)lt,(long long)FIN[p][i],slot);return 1;}
        }
        for(int i=0;i<cnt;i++)free(z[i].es);free(z);
    }
    ts_close_writer(w);free(s.buf);free(s.clk);
    return 0;
}

int main(void){
    for(unsigned seed=0;seed<24;seed++){
        if(run_seed(seed,0)){printf("FAILED batch seed %u\n",seed);return 1;}
        if(run_seed(seed,1)){printf("FAILED percall seed %u\n",seed);return 1;}
    }
    printf("STRESS_OK 48 configs\n");
    return 0;
}
