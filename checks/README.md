# 无窗口合成回归（agg_pixfmt_rgba 自定义合成）

## 运行

```sh
# 常规回归：临时目录构建、运行，PPM 预览写到 --out 指定目录
sh checks/check_compositing.sh --out /tmp/compositing-preview

# 失效检查：先确认原库通过，再对四类单点变异逐个跑同一套回归
sh checks/check_compositing.sh --mutations
```

只依赖现有 C++ 编译器与标准库（默认 `c++ -std=c++14 -O2`，可用 `CXX`/
`CXXFLAGS` 覆盖），不启动任何示例窗口。

## 文件

| 文件 | 作用 |
| --- | --- |
| `compositing_test.cpp` | 主回归，覆盖 blend_pixel / solid span / color span / scanline / renderer_base 裁剪入口 |
| `reference.h` | 独立双精度参照：Porter–Duff 四通道结果、`cover/255` 几何插值、半向上字节量化、多边形与像素方格相交面积 |
| `geometry.h` | 微型几何输入：半像素矩形（明确 cover 常量 128/255/128）与斜边三角形 |
| `check_compositing.sh` | 统一入口（常规 / `--mutations`） |
| `run_mutation_checks.sh` | 隔离失效检查，变异只施加在临时源码副本上 |
| `baseline.cpp` | 既有兼容检查，保持不变 |

## 覆盖范围

- 操作：source-over、source-in、destination-out、source-atop。
- 两个库入口分别覆盖：`comp_op_adaptor_rgba`（直通道源）与
  `comp_op_adaptor_rgba_pre`（预乘源）；目标缓冲始终存预乘值。同一输入
  在两种入口下的可见结果逐字节比较，同时各自对照独立参照。
- 参数矩阵：alpha ∈ {0,1,128,255}，cover ∈ {0,1,127,128,254,255}，
  含透明目标与不对称颜色；每次操作每通道允许 1 字节舍入差。
- 覆盖率是几何覆盖率：参照先算完整 Porter–Duff，再按 `cover/255` 在完整
  结果与原目标之间插值，不预先缩小源 alpha。source-in 的部分覆盖区域
  未覆盖目标必须保留（网格与 sentinel 断言共同保证）。
- 连续合成逐步比较：每一步立刻对照独立参照，累计误差不会被宽松的最终图
  掩盖。
- 内存排列：`order_rgba` 与 `order_bgra` 都检查。
- 步长：正、负 stride 都检查；渲染缓冲带行尾 0xA5 哨兵，缓冲外与未触及
  像素必须保持原值。
- 裁剪：`renderer_base` 裁掉 span 左段后，cover/color 数组必须从正确的源
  位置继续读；裁剪盒外像素不得被写入。
- 几何：半像素矩形只允许明确的 128/255 常量；三角形参照是独立计算的
  多边形–像素方格相交面积（Sutherland–Hodgman 裁剪 + 鞋带公式），不使用
  库自己输出的 covers 当真值。端到端容忍光栅化子像素网格量化（实测最大
  ~2.1 个 cover 字节），同时用库实际发出的 cover 再做一次 1 字节容差的
  严格合成检查，把几何量化与 Porter–Duff 舍入分开。
- 输出：放大 12 倍的颜色与透明度 PPM。

## 失效检查（四类单点变异）

变异脚本对每个变异新建 `include` 副本，锚点行必须且只能匹配一次，否则
明确报 HARNESS ERROR。只有测试以 `FAIL` 数值/哨兵断言失败才算检出；
编译失败、超时、无断言的崩溃都不算。

1. 预乘源在 `comp_op_adaptor_rgba_pre` 中再次乘 alpha；
2. `blender_base::get` 里 cover 被重复使用（cover 再乘 cover）；
3. `blender_rgba_pre` 合成时丢失目标 alpha 项；
4. `renderer_base::blend_solid_hspan` 左裁剪后不推进 cover 起点。

报告输出每个变异的首个失败用例与通道，而不只是退出码。

## 参照发现的现状缺陷与修复（2026-11）

`comp_op_rgba_src_atop`（`agg_pixfmt_rgba.h`）的 B 通道笔误，把
`d.b` 写成了已被更新的 `d.g`：

```cpp
d.b = s.b * d.a + d.g * s1a;   // 修复前
d.b = s.b * d.a + d.b * s1a;   // 修复后，与 W3C/SVG source-atop 一致
```

证据（预乘、cover=128）：

- 输入：dst=(200,100,50,255)，src=(30,200,170,255)，source-atop
- 独立参照期望：(115,150,110,255)
- 库实际输出：(115,150,160,255)，B 通道偏差 50，远超 1 字节容差

修复仅限这一行；其余合成实现、混合模式、颜色空间均未改动。
