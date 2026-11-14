#ifndef EXPORT_SAFETY_FIXTURE_H
#define EXPORT_SAFETY_FIXTURE_H
#include <libpe/pe.h>
#include <stdlib.h>
#include <string.h>
/* Independent little-endian PE bytes, not produced by a PE writer. */
#define FIXTURE_SIZE 4096
static void put16(unsigned char *b, size_t p, uint16_t v) {
    b[p]=(unsigned char)v; b[p+1]=(unsigned char)(v>>8);
}
static void put32(unsigned char *b, size_t p, uint32_t v) {
    put16(b,p,(uint16_t)v); put16(b,p+2,(uint16_t)(v>>16));
}
static void fixture(unsigned char *b, int bits) {
    size_t opt=0x98, prefix=bits==64?112:96;
    size_t sec=opt+prefix+128;
    memset(b,0,FIXTURE_SIZE);
    put16(b,0,0x5a4d); put32(b,0x3c,0x80); put32(b,0x80,0x4550);
    put16(b,0x84,bits==64?0x8664:0x14c); put16(b,0x86,1);
    put16(b,0x94,(uint16_t)(prefix+128)); put16(b,0x96,0x2002);
    put16(b,opt,bits==64?0x20b:0x10b);
    put32(b,opt+16,0x1400);
    if(bits==64) put32(b,opt+24,0x400000); else put32(b,opt+28,0x400000);
    put32(b,opt+32,0x1000); put32(b,opt+36,0x200);
    put32(b,opt+56,0x2000); put32(b,opt+60,0x200);
    put32(b,opt+prefix-4,16);
    put32(b,opt+prefix,0x1000); put32(b,opt+prefix+4,0x100);
    memcpy(b+sec,".edata",6); put32(b,sec+8,0xe00);
    put32(b,sec+12,0x1000); put32(b,sec+16,0xe00);
    put32(b,sec+20,0x200); put32(b,sec+36,0x40000040);
    /* IMAGE_EXPORT_DIRECTORY and three independent parallel tables. */
    put32(b,0x20c,0x1070); put32(b,0x210,7);
    put32(b,0x214,2); put32(b,0x218,2);
    put32(b,0x21c,0x1040); put32(b,0x220,0x1050); put32(b,0x224,0x1060);
    put32(b,0x240,0x1400); put32(b,0x244,0x10a0);
    put32(b,0x250,0x1090); put32(b,0x254,0x1098);
    put16(b,0x260,0); put16(b,0x262,1);
    memcpy(b+0x270,"fixture.dll",12); memcpy(b+0x290,"Alpha",6);
    memcpy(b+0x298,"Beta",5); memcpy(b+0x2a0,"other.Func",11);
}
/* The caller owns b; free the cache before releasing it. No file loader,
 * crypto initialization, or replacement resource/import/hash API is used. */
static pe_err_e fixture_open(pe_ctx_t *ctx, unsigned char *b, size_t size) {
    memset(ctx,0,sizeof(*ctx));
    ctx->map_addr=b; ctx->map_size=size; ctx->map_end=(uintptr_t)b+size;
    return pe_parse(ctx);
}
static void fixture_close(pe_ctx_t *ctx) {
    pe_exports_dealloc(ctx->cached_data.exports);
    free(ctx->pe.sections); free(ctx->pe.directories);
    memset(ctx,0,sizeof(*ctx));
}
#endif
