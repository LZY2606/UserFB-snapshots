#include "matio.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char**argv){
 assert(argc==2);size_t dims[]={4,3};
 mat_uint32_t ir[]={0,3,1,2},jc[]={0,2,3,4};double values[]={1,2,3,4};
 mat_sparse_t sparse={4,ir,4,jc,4,4,values};
 matvar_t*v=Mat_VarCreate("s",MAT_C_SPARSE,MAT_T_DOUBLE,2,dims,&sparse,0);assert(v);
 mat_t*m=Mat_CreateVer(argv[1],NULL,MAT_FT_MAT5);assert(m);
 assert(Mat_VarWrite(m,v,MAT_COMPRESSION_NONE)==0);Mat_VarFree(v);assert(Mat_Close(m)==0);
 m=Mat_Open(argv[1],MAT_ACC_RDONLY);assert(m);
 v=Mat_VarReadInfo(m,"s");assert(v&&v->class_type==MAT_C_SPARSE&&v->data==NULL);
 int start[]={0,0},stride[]={1,1},edge[]={2,2};double slab[4];
 assert(Mat_VarReadData(m,v,slab,start,stride,edge)==MATIO_E_OPERATION_NOT_SUPPORTED);
 assert(Mat_VarReadDataAll(m,v)==0);mat_sparse_t*r=(mat_sparse_t*)v->data;
 assert(r&&r->njc==4&&r->jc[3]==4&&r->ir[1]==3);
 assert(((double*)r->data)[2]==3&&v->dims[0]==4&&v->dims[1]==3);
 Mat_VarFree(v);assert(Mat_Close(m)==0);
 puts("BASELINE PASS");return 0;
}
