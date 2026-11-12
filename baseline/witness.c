#include <stdio.h>
#include <string.h>
#include "http_parser.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"deviation line=%d\n",__LINE__);return 2;}}while(0)
static char body[64];static size_t used;static int done,alive;
static int onbody(http_parser *p,const char *s,size_t n){(void)p;memcpy(body+used,s,n);used+=n;return 0;}
static int complete(http_parser *p){done++;alive=http_should_keep_alive(p);return 0;}
static int one(const char *msg,const char *expected,int eof,int keep){
 http_parser p;http_parser_settings st={0};st.on_body=onbody;st.on_message_complete=complete;
 used=0;done=0;alive=-1;http_parser_init(&p,HTTP_RESPONSE);
 size_t n=strlen(msg);CHECK(http_parser_execute(&p,&st,msg,n)==n&&HTTP_PARSER_ERRNO(&p)==HPE_OK);
 if(eof){CHECK(done==0);CHECK(http_parser_execute(&p,&st,NULL,0)==0);}
 CHECK(done==1&&used==strlen(expected)&&memcmp(body,expected,used)==0&&alive==keep);return 0;
}
int main(void){
 CHECK(one("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;v=1\r\nabc\r\n0\r\nX-End: yes\r\n\r\n","abc",0,1)==0);
 CHECK(one("HTTP/1.0 200 OK\r\n\r\nzz","zz",1,0)==0);
 CHECK(one("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nqrs","qrs",0,0)==0);
 puts("BASELINE_HTTP_FRAMING=chunked,eof,fixed-close");return 0;
}
