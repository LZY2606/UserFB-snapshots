//=====================================================================
//
// vsm.h - Variance Shadow Map (offline preview path)
//
// 阴影图先把一、二阶矩做盒式过滤，采样时再由矩计算可见度概率，
// 而不是对硬阴影比较结果做平均。捕获阶段复用 mini3d 的生产
// 梯形/扫描线光栅路径，三角形先在光源齐次空间过六个裁剪面。
//
//=====================================================================
#ifndef VSM_H
#define VSM_H

#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vsm_map vsm_map_t;

// 创建/销毁指定分辨率的阴影图
vsm_map_t *vsm_create(int width, int height);
void vsm_destroy(vsm_map_t *map);

// 清图：深度与两项矩全部复位为 1，每次 capture 前重新开始
void vsm_clear(vsm_map_t *map);

// 把 triangles（vertex_t 三元组数组，共 count 个三角形）经
// object_to_light 变到正交光源齐次空间（w 恒为 1，CVV 为
// x/y∈[-w,w]、z∈[0,w]），六面裁剪后走生产光栅路径写入矩。
// 输入三角形及其它外部设备均不会被改写。
void vsm_capture(vsm_map_t *map, const matrix_t *object_to_light,
	const vertex_t *triangles, size_t count);

// 从原始矩快照开始做 passes 轮 [1,2,1]/4 横纵卷积；
// passes=0 时仅复制原始矩，重复调用结果一致，不会累积模糊
void vsm_filter(vsm_map_t *map, int passes);

// 读取最近深度（未覆盖为 1）
float vsm_depth(const vsm_map_t *map, int x, int y);

// 读取原始矩（moment 取 1 或 2）
float vsm_raw_moment(const vsm_map_t *map, int x, int y, int moment);

// 读取过滤后矩（moment 取 1 或 2）
float vsm_moment(const vsm_map_t *map, int x, int y, int moment);

// 双线性采样过滤后矩并计算可见度概率：
//   接收深度 t 先减 bias；t<=M1 全亮；
//   variance=max(M2-M1^2, min_variance)，
//   p=variance/(variance+(t-M1)^2)，
//   最终 clamp((p-bleed)/(1-bleed),0,1)。
// 光图外、深度域外或无效投影返回 1（全亮）。
float vsm_sample(const vsm_map_t *map, float u, float v, float t,
	float bias, float min_variance, float bleed);

// 设备绑定/解绑：绑定阴影图、当前物体到光源矩阵及阴影参数。
// bias>=0、min_variance>0、bleed∈[0,1)、ambient∈[0,1]。
// 状态按设备保存，两台设备绑定不同阴影图互不干扰。
void device_bind_vsm(device_t *device, vsm_map_t *map,
	const matrix_t *object_to_light, float bias, float min_variance,
	float bleed, float ambient);
void device_unbind_vsm(device_t *device);

#ifdef __cplusplus
}
#endif

#endif /* VSM_H */
