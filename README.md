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

## 半立方体渐进辐射度（离线漫反射间接光）

在不引入窗口库和外部素材的前提下，工程新增了纯 C99 的离线辐射度模块，
复用 mini3d 原有的矩阵、梯形拆分和扫描线：

* `mini3d.h`：把原核心的数学、变换、梯形/扫描线和 `device_t` 类型与函数
  暴露出来，头文件可被 C++ 直接包含。
* `radiosity.h` / `radiosity.c`：C99 接口（头文件带 `extern "C"` 守护）。
  `radiosity_scene` 为不透明类型，`radiosity_create` 深拷贝三角面并建立
  半立方体传输矩阵，几何创建后不可变，也不使用任何全局场景状态。
* 每个源面在面积质心沿法线偏移后放置半立方体，顶面加四个 90 度侧面，
  侧面只积分朝法线的上半部；五个视图共用同一套离散权重归一化，并使用
  独立清除的图元 ID 目标和投影深度目标，不借用主相机缓冲。
* 源面不参与自遮挡，其余面双面遮挡；最近命中若为背面则只吸收能量，
  只有接收面正面得到传输。近面为 `offset/4`，跨越近面或侧界的三角形
  经齐次裁剪后保留可见部分。
* `radiosity_step` 按面积加权挑选未发射能量最大的面逐步射击，
  面积比 `Ai/Aj` 只在射击时换算，浮点辐射度全程不夹取。
* `radiosity_draw` 把当前辐射度乘以曝光作为恒定顶点色，仍走原来的
  三角形绘制管线，只在最终字节颜色上截断，并在结束后恢复调用方的
  变换和渲染状态、不清空调用方已有的颜色与深度。

### 纯 C 示例：一次反射与多次反射 PPM

`examples/radiosity_example.c` 完全用 C 构造一个小场景：一块发光顶面、
地板和后墙两块接收面、一块遮挡片，分别输出一次反射和迭代到收敛的
PPM 图像：

```
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
   examples/radiosity_example.c mini3d.c radiosity.c -lm \
   -o radiosity_example
./radiosity_example .
# 生成 radiosity_one.ppm（一次反射）和 radiosity_multi.ppm（多次反射）
```

### 测试

`tests/radiosity_target.c` 注册 visibility、transport、integration 三组
用例，分别用独立逐像素射线/三角形参照核对五个面的 ID 积分、用解析面积
和读出的 F 矩阵独立递推传输过程、以及校验分次推进/reset/双场景和原绘制
管线对照：

```
bash task_support/run.sh target
```

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
