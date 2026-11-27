# inter_weight_contract — 帧间预测数值契约回归

`tests/inter_weight_contract.cc` 是一套独立的数值回归：用真实的
`seq_parameter_set` / `pic_parameter_set` / `slice_segment_header` /
`de265_image` / `PBMotion` 调用生产函数 `generate_inter_prediction_samples()`
（`libde265/motion.cc`），再用一份按规范独立写出的坐标模型逐样本核对三个
输出平面。参考图像只经过 `RefPicList` → `refIdx` → `base_context::get_image()`
到达预测路径（`TestContext` 是按固定编号提供参考图像的 `base_context` 子类），
不把任何期望数组交给库里的加权函数。

## 独立模型的规则

- 亮度 8 抽头（表 8-10）、色度 4 抽头（表 8-11）系数表显式写出；
- 参考坐标按真实平面尺寸（56x40 亮度、28x20 色度）逐坐标夹取，不依赖库的
  padding；
- 水平滤波后保留规定的中间精度：`>> shift1`（`Min(4, BitDepth-8)`）是**截断**
  （向下取整），负的中间值原样保留，不提前裁到像素范围；
- 末级才按权重、`log2WD = denom + Max(2, 14-BitDepth)`、`offset << WpOffsetBdShift`
  计算并饱和到 `[0, (1<<BitDepth)-1]`；
- 有符号数除以 2 的幂一律定义为向下取整（`shr_floor`/`fdiv`），与二进制补码
  算术右移一致。

模型不调用任何生产插值/加权函数，不缓存生产输出当答案。每个用例还把当前
图像全部平面（含行尾 padding，56/28 不是 16 的倍数，stride 为 64/32）填成
位置相关的哨兵值，块外样本必须原样保留；参考图像在调用前后做字节级快照
比较。高位深平面按样本 stride（`get_image_stride()`）以 `uint16_t` 解释，
不使用字节 stride。

## 用例选取

precision 组（中间精度）：

- `luma_hphase_10bit_truncation`：10 位、P slice、亮度水平相位 1
  （`shift1 = 2`）。参考是线性斜坡 `f(x,y)=2x+5y`，8x8 块在 (16,16)，
  MV (1,0)。**手算样例**：输出 (0,0) 的水平加权和为
  `-106 + 4*108 - 10*110 + 58*112 + 17*114 - 5*116 + 118 = 7198 = 4*1799+2`。
  规定截断得 1799，加半单位再移位会得 1800；末级 `(1799+8)>>4 = 112`
  （舍入版得 113）。该块每个亮度样本都落在同一边界上（`S ≡ 2 (mod 4)`、
  `T+8 ≡ 15 (mod 16)`），截断规则被检查 64 次。色度手算：Cb (8,8) 处
  `S = 5208`，`(1302+8)>>4 = 81`。
- `luma_hv_12bit_intermediates`：12 位、双列表普通平均，负 MV（边缘夹取）
  与正向大 MV 并存，噪声纹理产生负的、不能被 16 整除的水平中间值
  （`shift1 = 4`），用例断言这两类中间值确实出现。
- `weighted_saturation_10bit`：10 位显式加权，`w=127`、负 offset 使末级在
  0 和 1023 两端都饱和；Cb/Cr 用不同权重（Cr 为负）。

lists 组（列表与权重选择）：

- `single_l0_unweighted`：单列表普通，两个块分别经 `refIdx` 0/1 选不同参考。
- `single_l1_explicit_negative_weight`：仅 L1、显式负权重 + 正 offset。
- `bi_plain_average`：同参考同 MV（未加权时生产端的合并捷径在数值上与
  平均完全等价，`(2p+64)>>7 == (p+32)>>6`）以及双不同参考的普通平均。
- `bi_explicit_same_ref`：**手算样例**。同一参考身份、相同 MV (0,0)，但
  L0 权重 1、L1 权重 3（denom 0）。合并捷径会得 `p`，规定的双列表加权得
  `2p`：亮度 (8,8) 处斜坡值为 `2*8+5*8 = 56`，`pred = 56<<6 = 3584`，
  `(1*3584 + 3*3584 + 64) >> 7 = 112`（合并得 56）。Cb (4,4)：
  `(1*2624 + 3*2624 + 64) >> 7 = 82`；Cr (4,4)：
  `(2*2176 + 1*2176 + 64) >> 7 = 51`。
- `bi_explicit_two_refs`：`RefPicList[0]={A,B}`、`RefPicList[1]={B,A}`，
  四个 (列表, refIdx) 组合的权重/offset 全不同，含负权重与正负 offset，
  两个块分别走 refIdx 0 和 1。

planes 组（三平面、色度相位、stride）：

- `yuv420_8bit_chroma_phases`：8 位 4:2:0，亮度四分之一相位映射到色度
  八分之一相位（hv、仅水平、仅垂直），Cb/Cr 权重不同。
- `yuv420_10bit_edge_strides`：10 位，右/下边缘块双向夹取，16 位平面按
  全 stride 校验，字节 stride 与样本 stride 混用会写进哨兵区。
- `yuv420_12bit_fullpel`：12 位整像素：内部直接拷贝、左上角夹取、以及
  MV 完全指出图外的重度边缘复制。

## 两种退化及其第一个差异

在同一组测试上验证过（临时改生产代码、跑通后还原）：

1. **显式加权开着仍合并相同参考和向量**（把 `motion.cc` 里合并捷径的
   `if (!weightedPredFlag)` 去掉）：`lists/bi_explicit_same_ref` 失败，
   第一个差异在亮度 (8,8)——得到 56（按 L0 权重的单列表结果），期望 112
   （双列表独立加权 `2p`）。
2. **高位深亮度水平滤波把截断改成加半个单位再移位**
   （`fallback-motion.cc` 的 qpel 水平滤波 `>>shift1` 变成
   `(+ (1<<(shift1-1))) >> shift1`）：`precision/luma_hphase_10bit_truncation`
   失败，第一个差异在亮度 (16,16)——得到 113（舍入），期望 112（截断）。

## 旧测试为什么覆盖不到

`task_support/baseline.cpp` 只对整段 girlshy.h265 的解码输出算 FNV 哈希：
它证明"那段素材的解码结果没变"，但 (a) 素材里未必出现"同参考同 MV 且双
列表权重不同"的块，加权路径的权重选择错误完全可以不在哈希上显现；
(b) 水平滤波的中间截断误差只有 1 个中间单位，经末级移位后大多数样本被
掩盖，只有落在舍入边界上的样本才不同，普通素材不保证命中；(c) 摘要无法
定位差异坐标，也无法区分"没测"和"测了没变"。本回归按坐标模型逐样本断言，
并把两类退化各自钉在一个手算过的坐标上。

运行：`bash task_support/run.sh target`（编译本文件、逐组核实断言计数、
并联跑现有解码对照）。正常源码必须精确通过，不允许误差。
