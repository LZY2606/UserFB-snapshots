#include "kvazaar.h"
#include <stdio.h>
int main(void){
  const kvz_api*api=kvz_api_get(8);
  const char*bad[][2]={{"input-format","P444"},{"input-bitdepth","10"},{"gop","1"},{"ref","3"},{"bipred","1"}};
  for(int t=0;t<5;++t){
    kvz_config*c=api->config_alloc();api->config_init(c);
    const char*o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"weighted-pred","1"}};
    for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)api->config_parse(c,o[i][0],o[i][1]);
    api->config_parse(c,bad[t][0],bad[t][1]);
    c->width=32;c->height=32;c->enable_logging_output=0;
    kvz_encoder*e=api->encoder_open(c);
    printf("%s=%s -> %s\n",bad[t][0],bad[t][1],e?"ACCEPTED(BAD)":"rejected(ok)");
    if(e)api->encoder_close(e);
    api->config_destroy(c);
  }
  return 0;}
