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

## 连续快门（运动模糊）

快速运动的三角形按单帧绘制会一闪一闪。`motion_render` 提供一个离线的连续快门入口，按整个曝光期间真正看得见的颜色成像，而不是对几个时刻的画面取平均。

* 头文件：`mini3d.h` 暴露原来的核心类型与函数；`motion.h` 暴露连续快门接口，实现在 `motion.c`。
* 入口：`int motion_render(device_t *device, const motion_triangle *triangles, size_t count, double *rgb)`。
* 输入：每个 `motion_triangle` 含 `open[3]` / `close[3]` 两组 `vertex_t`，两端下标相同的是同一个顶点；世界位置随 `t∈[0,1]` 线性移动，顶点 RGB 在快门期间不变并取 `open` 的 `color`。
* 前提：调用方已更新 `device->transform`，合成矩阵是有限仿射矩阵且保持 `w=1`；所有端点都在原 CVV 内；颜色有限且在 `[0,1]`。坐标约定复用 `transform_apply` / `transform_homogenize`，按 `(x+.5, y+.5)` 看像素，双面、不透明、无纹理。
* 成像规则：每时刻用屏幕重心坐标插值 window-z 与 RGB，z 小的遮住 z 大的；深度恰好相等时输入下标小的优先；无覆盖处取 `device->background` 的 RGB/255。共边按左上角半开规则归属，不重复也不露背景缝。背景不带深度，不读旧 zbuffer。
* 事件切分：边越过像素中心、投影朝向改变（面积过零）、覆盖面之间在该像素的深度交换，都会精确求根切分时间。深度差是重心有理函数，对应最高五次多项式，用 Sturm 序列隔离全部实根，不做固定时间采样、不靠加密采样猜事件。段内颜色用带误差控制的自适应 Simpson 积分，普通解析场景各通道绝对误差不超过 1e-7。
* 输出：`rgb` 为 `width*height*3` 个 double、按行存放的 `[0,1]` 时间积分；随后逐通道夹取，用 `floor(255*c+.5)` 一次量化写入原 framebuffer，成功返回 1。`count=0` 生成背景；指针无效或分配失败返回 0 且不发布部分图像。函数不改输入、矩阵、render_state、纹理和 zbuffer，可连续调用、多设备交错。
* 整段时间投影面积恒为零的面直接忽略；三角形允许在孤立时刻退化或翻转。

### 编译与运行

只依赖 C99 标准库和 libm：

```
# 测试（编译 motion.c 与 tests/motion_target.c，并跑原单帧对照）
bash task_support/run.sh target

# 示例：输出有快门 / 没快门两张 PPM
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. examples/motion.c \
   mini3d.c motion.c -lm -o motion_example
./motion_example
# 生成 motion_instant.ppm（单帧）与 motion_shutter.ppm（连续快门）
```

测试在 `tests/motion_target.c`，分 `events`、`integrals`、`pipeline` 三组，期望值均为独立手算，覆盖短暂穿越、快门内前后交换、非恒定顶点色积分、中途退化、共边网格、时间反转以及设备状态隔离等场景。


## 欢迎捐赠

![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/donation.png)

您的捐助是对该教程的最大肯定，欢迎使用支付宝手扫描上面的二维码，进行捐赠。捐赠款项将用于完善教程文档和图例，以及帮助我写出更多有意思的教程来。



欢迎关注

blog: https://skywind.me/blog

zhihu: https://www.zhihu.com/people/skywind3000 
