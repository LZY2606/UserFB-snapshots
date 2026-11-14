# mini3d

3D软件渲染教程，并没有任何性能优化，主要向人说明如何写一个固定管线的软件渲染器。虽然主体代码只有 700行，但是麻雀虽小，五脏俱全。

【[README in English](README.en.md)】

## 特性

* 单个文件：源代码只有一个 mini3d.c，单个文件实现所有内容，容易阅读。
* 独立编译：没有任何第三方库依赖，没有复杂的工程目录。
* 模型标准：标准 D3D 坐标模型，左手系加 WORLD / VIEW / PROJECTION 三矩阵
* 实现裁剪：简单 CVV 裁剪
* 纹理支持：最大支持 1024 x 1024 的纹理
* 深度缓存：使用深度缓存判断图像前后
* 透视贴图：透视纹理映射以及透视色彩填充
* 边缘计算：精确的多边形边缘覆盖计算
* 实现精简：渲染引擎只有 700行，模块清晰，主干突出。
* 详细注释：主要代码详细注释

## 编译

* mingw: 
		gcc -O3 mini3d.c -o mini3d.exe -lgdi32
* msvc:
		cl -O2 -nologo mini3d.c 
* 已编译版本：
[https://github.com/skywind3000/mini3d/releases](https://github.com/skywind3000/mini3d/releases)

## 演示

纹理填充：RENDER_STATE_TEXTURE
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_1.png)

色彩填充：RENDER_STATE_COLOR
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_0.png)

线框绘制：RENDER_STATE_WIREFRAME
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_2.png)

增加光照和二次线性插值（朋友给 Mini3D 增加的光照效果截图）
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_3.png)

## 方差阴影（离线预览）

在保留原有矩阵、梯形和扫描线渲染链的基础上，新增了一条先过滤矩再采样的
方差阴影（VSM）路径，而不是把硬阴影比较结果做平均。核心接口在 `mini3d.h`，
阴影图由 `vsm.h` / `vsm.c` 管理，全部为 C99 + libm；定义 `MINI3D_HEADLESS`
即可关闭 Win32 窗口层，只编译核心。

* `vsm_create(width,height)` / `vsm_destroy`：创建、销毁阴影图。
* `vsm_clear`：深度与一阶、二阶矩全部复位为 1；`vsm_capture` 内部每次
  也会重新清图，不会改写输入三角形或外部设备。
* `vsm_capture(map, object_to_light, triangles, count)`：`triangles` 是
  `vertex_t` 的三元组数组，矩阵把坐标变到正交光源齐次空间（`w` 恒为 1，
  CVV 为 x/y∈[-w,w]、z∈[0,w]）。三角形先过六个裁剪面，裁出的可见部分
  交给既有的梯形/扫描线生产光栅路径，不另写一套三角形光栅器。每个光图
  像素保留最小 z，并写入
  `M1=z`、`M2=z²+(dz/dx)²/4+(dz/dy)²/4`，
  梯度来自当前面的深度平面（单位为光图像素），不取邻像素有限差分。
* `vsm_filter(map, passes)`：从原始矩快照开始，每轮横向、纵向各做一次
  `[1,2,1]/4` 卷积，两个矩独立过滤，越界 tap 取 `(1,1)`；`passes=0`
  复制原始矩。重复调用相同轮数结果一致，不会在上次结果上继续模糊。
* `vsm_depth` / `vsm_raw_moment` / `vsm_moment`：分别读出最近深度、
  原始矩和过滤后矩。
* `vsm_sample(map,u,v,t,bias,min_variance,bleed)`：先对过滤后矩做双线性
  插值（纹素中心 `((x+.5)/width,(y+.5)/height)`，越界 tap 取 `(1,1)`），
  接收深度先减 bias，`t<=M1` 全亮；否则
  `variance=max(M2-M1²,min_variance)`、
  `p=variance/(variance+(t-M1)²)`，最后
  `clamp((p-bleed)/(1-bleed),0,1)`。光图外、深度域外或无效齐次投影
  一律按全亮处理。参数约束：`bias>=0`、`min_variance>0`、`bleed∈[0,1)`。
* `device_bind_vsm(device, map, object_to_light, bias, min_variance,
  bleed, ambient)` / `device_unbind_vsm`：绑定后，光坐标作为顶点属性
  跨相机裁剪与梯形插值，按主相机 `w` 透视校正后再做光投影（主相机的
  `rhw` 不会被当作光深度）。扫描线在选好纹理或顶点底色之后，用
  `ambient+(1-ambient)*p` 调制浮点底色再量化，纹理/颜色优先级和主相机
  深度比较均保持不变，两台设备绑定不同阴影图时状态互不串扰。

离线示例 `examples/vsm.c` 生成地面加遮挡方块的场景，输出左右对照 PPM
（左：未绑定全亮，右：VSM 阴影）：

	cc -std=c99 -O2 -DMINI3D_HEADLESS -I. examples/vsm.c mini3d.c vsm.c -lm -o vsm_example
	./vsm_example vsm_compare.ppm

任务测试 `tests/vsm_target.c` 覆盖矩捕获（斜平面梯度矩、六面裁剪、
不同深度遮挡者）、矩过滤（独立卷积核对、先比较再过滤的反例）和相机
着色（非等 w 透视、纹理优先级、后方遮挡、解绑与双设备），执行：

	bash task_support/run.sh target

该命令会链接 `mini3d.c`、`vsm.c` 和新测试，并同时重跑原颜色/纹理基线。

## 基础作业

* 增加背面剔除
* 增加简单光照
* 提供更多渲染模式
* 实现二次线性差值的纹理读取

## 进阶作业

* 推导并证明程序中用到的所有几何知识
* 优化顶点计算性能
* 优化 draw_scanline 性能
* 从 BMP/TGA 文件加载纹理
* 载入 BSP 场景并实现漫游

## 原理讲解

- [韦易笑：OpenGL 和 DirectX 是如何在只知道顶点的情况下得出像素位置的？](https://skywind.me/blog/archives/2594)
- [韦易笑：计算机底层是如何访问显卡的？](https://skywind.me/blog/archives/1774)

## 相关项目

- [RenderHelp](https://github.com/skywind3000/RenderHelp)：另外一个支持可编程渲染管线的精简软渲染器实现，详细见 [介绍](https://skywind.me/blog/archives/2589)。

## 欢迎捐赠

![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/donation.png)

您的捐助是对该教程的最大肯定，欢迎使用支付宝手扫描上面的二维码，进行捐赠。捐赠款项将用于完善教程文档和图例，以及帮助我写出更多有意思的教程来。



欢迎关注

blog: https://skywind.me/blog

zhihu: https://www.zhihu.com/people/skywind3000 
