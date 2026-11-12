#include "readstat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static char expected[258]; static int count;
static ssize_t out(const void *p,size_t n,void *v){return fwrite(p,1,n,v);}
static int variable(int i,readstat_variable_t *v,const char *l,void *c){return i==1?READSTAT_HANDLER_SKIP_VARIABLE:READSTAT_HANDLER_OK;}
static int value(int row,readstat_variable_t *v,readstat_value_t x,void *c){
 int i=readstat_variable_get_index(v);
 if(i==0){if(strcmp(readstat_string_value(x),expected)){puts("DIFF string-segment");return READSTAT_HANDLER_ABORT;}}
 else if(i==2){if(readstat_double_value(x)!=7+row){puts("DIFF numeric-token");return READSTAT_HANDLER_ABORT;}}
 else {puts("DIFF skipped-column");return READSTAT_HANDLER_ABORT;}
 count++;return READSTAT_HANDLER_OK;
}
int main(void){
 char path[]="/tmp/readstat-XXXXXX";int fd=mkstemp(path);assert(fd>=0);FILE *f=fdopen(fd,"wb");assert(f);
 for(int i=0;i<257;i++)expected[i]='a'+i%23;
 readstat_writer_t *w=readstat_writer_init();assert(w);readstat_set_data_writer(w,out);
 readstat_variable_t *s=readstat_add_variable(w,"text",READSTAT_TYPE_STRING,257);
 readstat_variable_t *skip=readstat_add_variable(w,"unused",READSTAT_TYPE_DOUBLE,0);
 readstat_variable_t *n=readstat_add_variable(w,"number",READSTAT_TYPE_DOUBLE,0);
 assert(!readstat_writer_set_compression(w,READSTAT_COMPRESS_ROWS));
 assert(!readstat_begin_writing_sav(w,f,2));
 for(int r=0;r<2;r++){assert(!readstat_begin_row(w));assert(!readstat_insert_string_value(w,s,expected));
 assert(!readstat_insert_double_value(w,skip,99));assert(!readstat_insert_double_value(w,n,7+r));assert(!readstat_end_row(w));}
 assert(!readstat_end_writing(w));readstat_writer_free(w);fclose(f);
 readstat_parser_t *p=readstat_parser_init();readstat_set_variable_handler(p,variable);readstat_set_value_handler(p,value);
 readstat_error_t rc=readstat_parse_sav(p,path,NULL);readstat_parser_free(p);unlink(path);
 if(rc || count!=4){printf("DIFF parse=%d cells=%d\n",rc,count);return 1;}
 puts("READSTAT_SAV_OK rows=2 selected-cells=4 long-string=257");return 0;
}
