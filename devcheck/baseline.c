#include "shapefil.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 assert(argc==2);char path[2048];snprintf(path,sizeof(path),"%s/capture",argv[1]);
 int parts[]={0,5,10,15};
 double x[]={2,8,8,2,2,20,30,30,20,20,0,10,10,0,0,3,4,4,3,3};
 double y[]={2,2,8,8,2,0,0,10,10,0,0,0,10,10,0,3,3,4,4,3};
 double z[20],m[20];for(int i=0;i<20;i++){z[i]=x[i]+y[i];m[i]=100+i;}
 SHPObject *o=SHPCreateObject(SHPT_POLYGONZ,-1,4,parts,NULL,20,x,y,z,m);assert(o);
 SHPHandle h=SHPCreate(path,SHPT_POLYGONZ);assert(h);
 assert(SHPWriteObject(h,-1,o)==0);SHPDestroyObject(o);SHPClose(h);
 DBFHandle d=DBFCreate(path);assert(d);
 assert(DBFAddField(d,"source",FTString,20,0)==0);
 assert(DBFWriteStringAttribute(d,0,0,"capture-A"));DBFClose(d);
 h=SHPOpen(path,"rb");assert(h);o=SHPReadObject(h,0);assert(o);
 d=DBFOpen(path,"rb");assert(d);
 assert(o->nShapeId==0 && o->nParts==4 && o->nVertices==20);
 assert(o->panPartStart[2]==10 && o->padfX[0]==2 && o->padfX[10]==0);
 assert(o->padfZ[6]==30 && o->padfM[19]==119);
 assert(!strcmp(DBFReadStringAttribute(d,o->nShapeId,0),"capture-A"));
 printf("BASELINE SHPZ raw-ring-order ZM DBF-physical-row OK\n");
 DBFClose(d);SHPDestroyObject(o);SHPClose(h);return 0;
}
