#include "kvazaar.h"
#include <stdio.h>
int main(void){
  const kvz_api*api=kvz_api_get(8);
  kvz_config*c=api->config_alloc();api->config_init(c);
  int r1=api->config_parse(c,"gop","1");
  printf("parse gop=1 -> %d, gop_len=%d\n",r1,c->gop_len);
  int r2=api->config_parse(c,"input-format","P444");
  printf("parse P444 -> %d, chroma_format=%d input_format=%d\n",r2,c->chroma_format,c->input_format);
  api->config_destroy(c);
  return 0;}
