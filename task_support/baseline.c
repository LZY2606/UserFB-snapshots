#define MINI3D_HEADLESS
#include "../mini3d.c"
static void triangle(device_t *d, float depth, color_t color) {
    vertex_t a={{-.75f,-.75f,depth,1},{0,0},{0,0,0},0};
    vertex_t b={{.75f,-.75f,depth,1},{1,0},{0,0,0},0};
    vertex_t c={{0,.75f,depth,1},{.5f,1},{0,0,0},0};
    a.color=b.color=c.color=color;
    device_draw_primitive(d,&a,&b,&c);
}
int main(void) {
    device_t d; IUINT32 tex[4]={0x203040,0x203040,0x203040,0x203040};
    device_init(&d,32,24,NULL); matrix_set_identity(&d.transform.transform);
    d.background=0; device_clear(&d,0); d.render_state=RENDER_STATE_COLOR;
    triangle(&d,.25f,(color_t){1,0,0});
    unsigned color=d.framebuffer[12][16]; float rhw=d.zbuffer[12][16];
    if(color!=0xff0000 || fabsf(rhw-1)>1e-6f) return 1;
    device_clear(&d,0); device_set_texture(&d,tex,8,2,2);
    d.render_state=RENDER_STATE_COLOR|RENDER_STATE_TEXTURE;
    triangle(&d,.25f,(color_t){1,0,0});
    unsigned textured=d.framebuffer[12][16];
    if(textured!=0x203040) return 2;
    printf("MINI3D_BASELINE_OK color=%06x texture=%06x rhw=%.3f\n",color,textured,rhw);
    device_destroy(&d); return 0;
}
