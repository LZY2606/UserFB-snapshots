#define MINI3D_HEADLESS
#include "../mini3d.c"
int main(void){
 device_t d;device_init(&d,32,32,NULL);device_clear(&d,0);
 d.render_state=RENDER_STATE_COLOR;
 vertex_t a={{-.8f,-.8f,2,1},{0,0},{1,0,0},0};
 vertex_t b={{.8f,-.8f,2,1},{0,0},{1,0,0},0};
 vertex_t c={{0,.8f,2,1},{0,0},{1,0,0},0};
 device_draw_primitive(&d,&a,&b,&c);
 int n=0;for(int y=0;y<32;++y)for(int x=0;x<32;++x){
  if(d.zbuffer[y][x]>0){assert(fabsf(d.zbuffer[y][x]-.5f)<1e-5f);++n;}
 }
 assert(n>30);assert(d.framebuffer[16][16]==0xff0000);
 device_destroy(&d);puts("BASE mini3d_color_depth PASS");return 0;
}
