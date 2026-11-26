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

## 连续快门运动渲染

新增的 `mini3d.h` 暴露原来的核心类型与函数；`motion.h` / `motion.c` 提供离线连续快门入口：

```c
int motion_render(device_t *device,
                  const motion_triangle *triangles,
                  size_t count,
                  double *rgb);
```

每个 `motion_triangle` 给出同一组三个顶点在快门打开和关闭时的状态。下标相同的端点是同一个顶点，其世界坐标在 `t∈[0,1]` 线性运动；顶点 RGB 不随时间变化，统一读取 `open[k].color`。调用前仍需更新 `device->transform`，并保证合成矩阵是有限的仿射矩阵、`w=1`，所有端点位于原 CVV 内且颜色在 `[0,1]`。

对每个像素中心 `(x+0.5,y+0.5)`，实现先求出所有会改变可见面的连续事件：边行列式过零、投影面积过零/翻转、以及两个面的屏幕重心深度相等。二次几何事件直接求实根，深度差是次数不超过 5 的多项式，用导数分区和二分定位符号交换根；不使用固定时间采样或加密采样猜测短暂显露。事件之间覆盖面、半开共边归属和深度顺序不变，获胜面的屏幕重心 RGB 是二次/二次有理函数，再进行带误差控制的自适应 Simpson 积分。普通解析尺度下，每个颜色通道的积分误差目标低于 `1e-7`。

`rgb` 必须指向长度为 `width*height*3` 的 `double` 数组，按行保存 `[0,1]` 时间积分。成功时结果被复制到该数组，随后逐通道夹取并执行一次 `floor(255*c+0.5)` 量化，写入原有 `device->framebuffer`，函数返回 `1`。`count=0` 生成 `device->background`。空指针、非法设备尺寸或临时内存分配失败时返回 `0`；在全部像素计算完成前不会改写 framebuffer。该接口不读取或写入旧 zbuffer，也不修改输入、矩阵、`render_state`、纹理和 zbuffer，可重入并可交错用于多台设备。普通 `device_draw_primitive` 路径保持不变。

目标测试位于 `tests/motion_target.c`，注册 `events`、`integrals`、`pipeline` 三组用例：

```bash
bash task_support/run.sh target
```

该命令会编译 `motion.c` 和目标测试，并继续运行原来的单帧 baseline。

PPM 示例可用下面命令构建：

```bash
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
    examples/motion.c mini3d.c motion.c -lm -o motion_example
./motion_example
```

它生成 `motion_shutter.ppm`（连续快门积分）和 `motion_single.ppm`（快门打开时刻的普通单帧绘制）。实现、测试和示例只使用 C99 标准库与 libm。

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
