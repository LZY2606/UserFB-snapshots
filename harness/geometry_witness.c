#define VOXELIZER_IMPLEMENTATION
#include "voxelizer.h"
#include <stdio.h>
static vx_mesh_t* make(int split,float scale){vx_mesh_t*m=vx_mesh_alloc(split?4:3,split?9:3);float a[4][3]={{3,0,0},{0,3,0},{0,0,3},{1,1,1}};for(int i=0;i<(split?4:3);i++){m->vertices[i].x=a[i][0]*scale;m->vertices[i].y=a[i][1]*scale;m->vertices[i].z=a[i][2]*scale;m->colors[i].r=1;}unsigned ids[9]={0,1,3,1,2,3,2,0,3};for(int i=0;i<(split?9:3);i++)m->indices[i]=split?ids[i]:i;return m;}
static int has(vx_point_cloud_t*p,float x,float y,float z){for(size_t i=0;i<p->nvertices;i++)if(p->vertices[i].x==x&&p->vertices[i].y==y&&p->vertices[i].z==z)return 1;return 0;}
int main(){for(int k=0;k<2;k++){float scale=k?0.00015f:1,precision=k?0:2.5f;vx_mesh_t*a=make(0,scale),*b=make(1,scale);vx_point_cloud_t*p=vx_voxelize_pc(a,1,1,1,precision),*q=vx_voxelize_pc(b,1,1,1,precision);printf("CASE=%s original_voxels=%zu split_voxels=%zu center333_original=%d split=%d\n",k?"small_positive_area":"positive_precision",p->nvertices,q->nvertices,has(p,3,3,3),has(q,3,3,3));vx_point_cloud_free(p);vx_point_cloud_free(q);vx_mesh_free(a);vx_mesh_free(b);}}
