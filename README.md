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

## 渐进辐射度（Radiosity）

在原固定管线之上增加了一个离线的漫反射间接光模块：

* `mini3d.h`：把原 `mini3d.c` 的核心类型（`matrix_t`/`vertex_t`/`device_t` 等）和函数暴露出来，`mini3d.c` 自己也包含它，声明和定义不会漂移；头文件用 `extern "C"` 包裹，可以被 C++ 直接使用。
* `radiosity.h` / `radiosity.c`：C99 接口，实现了半立方体可见性驱动的渐进辐射度。每个面（`radiosity_patch`）是世界空间三角形，绕序决定朝外的法线。`radiosity_create` 深拷贝几何并用 `offset` 把半立方体放到各面面积质心之外，然后遍历五个 90° 视图，复用原来的矩阵、梯形拆分和扫描线链路，把可见性积分成传输矩阵 `F`。之后用“发射”（shooting）方式反复把 `Aᵢ₍Uᵣ+U_g+U_b₎` 最大的面的能量分给其它面，直到能量耗尽。

核心接口：

```c
radiosity_scene *radiosity_create(const radiosity_patch *patches,
                                  size_t count, int resolution, float offset);
void   radiosity_destroy(radiosity_scene *scene);
float  radiosity_factor(const radiosity_scene *scene, size_t i, size_t j);
size_t radiosity_step(radiosity_scene *scene, size_t shots);
int    radiosity_get(const radiosity_scene *scene, size_t i,
                     color_t *B, color_t *U, float *area);
size_t radiosity_last_shooter(const radiosity_scene *scene);
void   radiosity_reset(radiosity_scene *scene);
void   radiosity_draw(const radiosity_scene *scene, device_t *device,
                      float exposure);
```

`radiosity_draw` 把当前累计辐射度 `B × exposure` 当作每个面的恒定顶点色，走原来的三角形绘制管线写进设备，只在最终字节颜色上截断；绘制期间使用场景自己的世界坐标，结束后恢复调用方的 `world`/`render_state` 等状态，不清空调用方已有的颜色和深度缓冲。

### 完全用 C 构造的离线示例

`examples/radiosity_demo.c` 不依赖窗口库或外部素材：一块发光的顶面把能量传给一块地板和一面背墙（两块接收面），中间悬着一块遮挡片投下阴影。程序分别走 1 次和多次发射，各输出一张 PPM：

```bash
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
   examples/radiosity_demo.c radiosity.c mini3d.c -lm -o radiosity_demo
./radiosity_demo       # 生成 radiosity_1bounce.ppm 与 radiosity_nbounce.ppm
```

一次发射时只有直接光加第一跳，房间偏暗、阴影硬；多次发射后接收面自身也参与发射，间接光填满房间、彩色面之间互相串色、阴影变软。

模块自带测试放在 `tests/radiosity_target.c`，通过 `bash task_support/run.sh target` 运行（会编译新模块与测试，跑 visibility / transport / integration 三组，再跑原固定管线对照）。

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
