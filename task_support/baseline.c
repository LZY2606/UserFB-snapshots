#include <mxf/mxf.h>
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline failed %d: %s\n",__LINE__,#x); return 1; } } while (0)
int test_create_and_write(const char *);
int test_read(const char *);
int main(void) {
    const char *path=".task-build/essence.mxf";
    MXFFile *f=NULL; mxfKey key; uint8_t llen; uint64_t len,total=0;
    unsigned parts=0,packets=0; uint8_t bytes[64];
    REQUIRE(test_create_and_write(path)); REQUIRE(test_read(path));
    REQUIRE(mxf_disk_file_open_read(path,&f));
    while (mxf_read_kl(f,&key,&llen,&len)) {
        if (mxf_is_partition_pack(&key)) {
            MXFPartition *p=NULL;
            REQUIRE(mxf_read_partition(f,&key,len,&p));
            REQUIRE(p!=NULL); ++parts; mxf_free_partition(&p);
        } else if (mxf_is_gc_essence_element(&key)) {
            uint64_t remain=len; ++packets; total+=len;
            while (remain) {
                unsigned j,step=remain>sizeof(bytes)?sizeof(bytes):(unsigned)remain;
                REQUIRE(mxf_file_read(f,bytes,step)==step);
                for(j=0;j<step;j++) REQUIRE(bytes[j]==0);
                remain-=step;
            }
        } else {
            REQUIRE(mxf_skip(f,len));
        }
    }
    REQUIRE(parts==4 && packets==3 && total==2304);
    mxf_file_close(&f);
    printf("BASELINE MXF partitions=%u audio_klv=%u pcm_bytes=%llu readback=exact\n",parts,packets,(unsigned long long)total);
    return 0;
}
