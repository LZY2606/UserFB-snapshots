#include "libdeflate.h"
#include "dictionary_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 struct libdeflate_compressor*c=libdeflate_alloc_compressor(6);
 struct libdeflate_decompressor*d=libdeflate_alloc_decompressor();
 unsigned char compressed[1000],out[400];size_t n,got=0,used=0;const char*text="offline records offline records";
 assert(c&&d);n=libdeflate_zlib_compress(c,text,strlen(text),compressed,sizeof(compressed));assert(n>0);
 assert(libdeflate_zlib_decompress_ex(d,compressed,n,out,sizeof(out),&used,&got)==LIBDEFLATE_SUCCESS);
 assert(got==strlen(text)&&used==n&&!memcmp(out,text,got));
 assert(libdeflate_zlib_decompress_ex(d,dictionary_zlib,sizeof(dictionary_zlib),out,sizeof(out),&used,&got)==LIBDEFLATE_BAD_DATA);
 assert(libdeflate_deflate_decompress_ex(d,dictionary_zlib+6,sizeof(dictionary_zlib)-10,out,sizeof(out),&used,&got)==LIBDEFLATE_BAD_DATA);
 libdeflate_free_compressor(c);libdeflate_free_decompressor(d);
 puts("LIBDEFLATE_BASELINE_OK ordinary-roundtrip=1 preset-dictionary=unsupported");return 0;
}
