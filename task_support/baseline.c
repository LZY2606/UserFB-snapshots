#include "rdfa.h"
#include "rdfa_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct stats {int count,copy,pattern,payload,event;};
static void collect(rdftriple *t,void *p){
 struct stats *s=p;s->count++;
 if(strcmp(t->predicate,"http://www.w3.org/ns/rdfa#copy")==0)s->copy++;
 if(strcmp(t->object,"http://www.w3.org/ns/rdfa#Pattern")==0)s->pattern++;
 if(strcmp(t->predicate,"https://v.test/name")==0)s->payload++;
 if(strcmp(t->subject,"https://doc.test/event")==0 && strcmp(t->predicate,"https://v.test/name")==0)s->event++;
 rdfa_free_triple(t);
}
int main(void){
 char text[]="<html xmlns=\"http://www.w3.org/1999/xhtml\" prefix=\"rdfa: http://www.w3.org/ns/rdfa# ex: https://v.test/\" version=\"XHTML+RDFa 1.1\"><head><title>Events</title></head><body><div about=\"https://doc.test/event\"><link property=\"rdfa:copy\" href=\"https://doc.test/pattern\"/></div><div about=\"https://doc.test/pattern\" typeof=\"rdfa:Pattern\"><span property=\"ex:name\">Hall</span></div></body></html>";
 struct stats s={0};rdfacontext *c=rdfa_create_context("https://doc.test/page");
 if(!c)return 2;c->callback_data=&s;rdfa_set_default_graph_triple_handler(c,collect);
 int status=rdfa_parse_start(c);
 if(status==RDFA_PARSE_SUCCESS)status=rdfa_parse_chunk(c,text,strlen(text),0);
 if(status==RDFA_PARSE_SUCCESS)status=rdfa_parse_chunk(c,"",0,1);
 rdfa_parse_end(c);rdfa_free_context(c);
 if(status!=RDFA_PARSE_SUCCESS || s.copy!=1 || s.pattern!=1 || s.payload!=1 || s.event!=0){fprintf(stderr,"production baseline mismatch status=%d count=%d copy=%d pattern=%d payload=%d event=%d\n",status,s.count,s.copy,s.pattern,s.payload,s.event);return 3;}
 printf("BASELINE_OK copy=%d pattern=%d pattern_name=%d event_name=%d\n",s.copy,s.pattern,s.payload,s.event);return 0;
}
