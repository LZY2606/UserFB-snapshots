//=====================================================================
//
// csm.h - Cascaded Shadow Maps (三级级联阴影) for Mini3D
//
// 调用方提供：透视相机（view 矩阵 + fov/aspect + near/far）、方向光、
// 三组同尺寸方形深度图、划分参数 lambda，以及全部遮挡物三角形
// （世界空间，point_t 每三个为一个三角形）。
//
// 分界：split_i = lambda * near*(far/near)^(i/3)
//              + (1-lambda) * (near + (far-near)*i/3), i = 1,2,3
//
// 每级用方形正交投影包住对应视锥段的角点，投影中心按世界空间纹素
// 大小吸附，光深范围包含能投到接收区的离屏遮挡物。矩阵与覆盖范围
// 均可从 csm_t 直接读出。
//
//=====================================================================
#ifndef CSM_H
#define CSM_H

#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSM_CASCADES 3

typedef struct {
	int size;                               // 深度图边长（方形）
	float lambda;                           // 划分参数 [0,1]
	float znear, zfar;                      // 相机近远平面
	float splits[CSM_CASCADES + 1];         // 视深分界：[0]=near .. [3]=far
	matrix_t light_view;                    // 光观察矩阵（三级共用）
	matrix_t light_proj[CSM_CASCADES];      // 各级方形正交投影
	matrix_t light_vp[CSM_CASCADES];        // light_view * light_proj
	float ortho_center[CSM_CASCADES][2];    // 吸附后的光空间 xy 中心
	float ortho_center_raw[CSM_CASCADES][2];// 吸附前的光空间 xy 中心
	float ortho_extent[CSM_CASCADES];       // 方形边长（世界单位）
	float depth_min[CSM_CASCADES];          // 各级光深范围（世界单位）
	float depth_max[CSM_CASCADES];
	float *depth[CSM_CASCADES];             // 深度图，size*size，行优先
	int depth_owner[CSM_CASCADES];          // 是否由 csm 内部分配
	float ambient;                          // 环境光 [0,1]
	float bias;                             // 世界深度 bias，非负
} csm_t;

// 初始化并内部分配三张 size x size 深度图
void csm_init(csm_t *csm, int size);
void csm_destroy(csm_t *csm);
// 改用调用方提供的深度图（size*size 浮点），释放内部缓冲
void csm_set_depth_map(csm_t *csm, int index, float *map);

// 依据相机/方向光/遮挡物更新划分、光矩阵与覆盖范围。
// occluders 为完整场景的世界空间三角形（每 3 点一个三角形），
// 不是只有主相机可见的面；光深范围会把能投到接收区的离屏遮挡物算进去。
void csm_update(csm_t *csm, const matrix_t *cam_view, float fovy, float aspect,
	float zn, float zf, const vector_t *light_dir, float lambda,
	const point_t *occluders, int tri_count);

// 光源 pass：清空深度图（远值），再把遮挡物画进各级深度图。
// 三角形按齐次平面裁出可绘部分，保留更靠近光源的片元。
void csm_clear(csm_t *csm);
void csm_render_triangle(csm_t *csm, const point_t *a, const point_t *b,
	const point_t *c);
void csm_render(csm_t *csm, const point_t *tris, int tri_count);

// 主 pass 取样：world 为透视校正后的世界坐标，view_z 为正视深度。
// 返回可见度 [0,1]；深度图外与最后一级以外均视为无遮挡。
float csm_visibility(const csm_t *csm, const point_t *world, float view_z);

// 绑定/解绑到渲染设备：ambient ∈ [0,1]，bias 为非负世界深度。
void csm_bind(csm_t *csm, device_t *device, float ambient, float bias);
void csm_unbind(device_t *device);

#ifdef __cplusplus
}
#endif

#endif /* CSM_H */
