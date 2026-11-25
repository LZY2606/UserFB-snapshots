# Deblock 整图数值回归（tests/deblock_contract.c）

这套测试为 h264bsd 的 portable C 去块路径提供独立的整图像素契约，保护后续
优化时容易串在一起的边界强度（bS）、QP/阈值推导和原位更新顺序。生产源码
（`src/`）与原有 I/P 解码能力未改动，不涉及 B 画面，也不新增 SIMD 后端。

## 文件

| 文件 | 角色 |
| --- | --- |
| `tests/deblock_contract.c` | 注册 `strength` / `threshold` / `order` 三组共 14 个具名用例；**不定义 main**，由 `task_support/driver.c` 驱动。固定构建命令只编译这一个 `tests/*.c`，因此它文本包含下面的参照实现。 |
| `tests/deblock_ref.h` / `deblock_ref.c` | **独立参照模块**。只按整幅像素和“自然 4×4 块栅格坐标”描述网格，自行推导 bS、alpha/beta/tC0/QpC 阈值并提交滤波。只依赖 C99 标准库与自有头。 |
| `tests/deblock_adapter.h` | **测试输入适配层**。把自然坐标描述翻译成真实的 `image_t` + `mbStorage_t` 网格（含解码块编号映射、`mbA/mbB`、`refAddr` 身份、哨兵内存），调用生产入口 `h264bsdFilterPicture()`，再与参照逐字节比较 Y/Cb/Cr 和前后哨兵。 |
| `tests/deblock_stream.h` | 公开入口回归：手写 Annex-B 小流（I_PCM + P_Skip/P_L0_16x16、两个不同切分的 slice、启用去块），走 `h264bsdDecode` → `h264bsdNextOutputPicture`，用同一参照链式核对三平面。 |

运行：

```bash
bash task_support/run.sh target     # 编译并执行 14 个用例 + 旧 baseline
bash task_support/baseline.sh       # 只跑旧的 32x32 I_PCM 逐字节对照
```

## 独立性约定

- 测试只调用生产公开整图入口 `h264bsdFilterPicture`（直接网格）和
  `h264bsdDecode` 系列（公开入口用例），**不直接喂 bS 给任何内部滤波函数**，
  也不调用 `GetBoundaryStrengths` / `FilterLuma` 等内部函数。
- 参照**没有**复制生产的展开索引表 `mb4x4Index`、阈值静态数组、滤波宏或任何
  既有输出哈希。参照里的 `REF_ALPHA / REF_BETA / REF_TC0 / REF_QPC` 是按标准
  规范表重新列出的同一组数值（数值必然相同，因为它们是标准常量），来源：
  ITU-T H.264 / ISO/IEC 14496-10 第 8.7.2 节“Deblocking filter process”的
  QP_C 映射表以及 alpha/beta/t′C0 阈值表（不同版次表号约为表 8-15～8-18）。
  每个表上方注释说明了出处。
- **解码块编号 ≠ 自然栅格坐标**。H.264 残差/MV 数组使用解码顺序
  `0,1,4,5,2,3,6,7,...`，自然栅格 `n=by*4+bx` 到解码索引的翻译只写在适配层
  的 `AD_NAT2DEC[16]`。参照本身只用自然坐标，生产继续用它私有的 static
  `mb4x4Index`，两侧不共用展开索引，因此错位不会被“同一张表”掩盖。
- `image_t.width/height` 在本库中以**宏块数**为单位；图像内存布局为
  Y(`nMb*256`) + Cb(`nMb*64`) + Cr(`nMb*64`)，亮度行跨 `W*16`、色度跨 `W*8`。
  适配层按此布局构造真实 `image_t`。
- `refPic` 字段在适配层被刻意写成相同值 7；强度身份只取决于 `refAddr`
  指针是否指向不同的真实存储（`ref_token[]`），与生产
  `mb1->refAddr[...] != mb2->refAddr[...]` 的判断一致。
- 解码器对 `I_PCM` 宏块会执行 `pMb->qpY = 0`（见
  `h264bsd_macroblock_layer.c`），公开流模型如实把 PCM MB 的 QP 设为 0；
  这是被钉住的真实行为，不是放宽误差。

## 覆盖与“选出来的强度必须改变/保留可辨认像素”

- **strength（7 例）**：帧内 cross-MB bS=4 / 内部 bS=3；任一侧残差→bS=2；
  `refAddr` 身份不同（而 `refPic` 相同）→bS=1；MV 分量差“恰好 4 个四分之一
  像素单位”阈值（3 不滤波 / ±4 滤波）；完全 bS=0 时三平面逐字节不变；
  16×16 / 16×8 / 8×16 / 8×8 四种分区分别对齐生产的不同快捷分支；跨 MB 的
  残差/ref/MV 来源单列一例；`sliceId` 与 `disableDeblockingFilterIdc`
  0/1/2 对左/上外边的门控，以及 idc=2 跨 slice 时内部边照常处理。
- **threshold（4 例）**：相邻 MB 使用不同 QP 并跨过 `QpC` 非线性区，组合
  正负 `chromaQpIndexOffset` 与相互独立的 `filterOffsetA/B`，两个方向
  （2×1 左边、1×2 顶边）共约 1200 组配置逐字节比较，专门钉住“色度必须先
  分别裁剪+映射两侧 QP，再做带舍入平均”；alpha/beta 通过与拦截纹理；
  tc 裁剪、强滤波两侧 small/p2/q2 条件与近邻更新用手算硬常量钉死
  （`x2..x5 = 103,107,109,112`；强滤波 `x13..x18 = 121,122,122,124,125,125`；
  small 不成立时 `x14..x17 = 112,116,123,126`）；以及公开解码三平面回归。
- **order（3 例）**：交叉边纹理让同一 4 行带内的水平滤波读到竖直滤波刚写回的
  像素；3×3 MB 栅格 + 不同 QP/偏移钉住宏块栅格提交次序；前后哨兵与
  idc=1/2 禁用边附近像素检查。参照严格按“MB 栅格次序、每 4 行先竖后横、
  就地读写”提交，不会把两个方向都从原始快照算完再拼接。
- 像素容差为 **0**：整个 Y/Cb/Cr 逐字节相等才算通过。

除具名用例外，开发期还做过 60000 组随机网格差分（四种分区、随机
QP/offset/slice/idc/系数/MV/ref 身份与六类纹理）以及 ASan/UBSan 运行，
参照与生产全部一致且无越界；这些临时 fuzz 程序未留在提交的测试目录中。

## 正常运行结果（本修订）

`bash task_support/run.sh target` 退出码 0：

```
GROUP strength  cases=7  checks=107  COMPLETE
GROUP threshold cases=4  checks=1258 COMPLETE
GROUP order     cases=3  checks=44   COMPLETE
TARGET_COMPLETE cases=14
H264BSD_BASE_OK frames=2 pixels_checked=3072 concealed=0
```

（`task_support/driver.c` 要求每组至少 1 例且 check 数 > 0；这里每组至少
2 个不同名用例。）

## 两份独立退化的“第一个差异”

下列两份退化都能**编译通过、不崩溃、非空测试**，并被内容断言判失败
（driver/`run.sh target` 退出码 4）。行号对应当前修订；若后续编辑本文件，
以坐标/数值这个稳定的“第一差异”为准。

### 退化 A：色度外边先平均两侧亮度 QP 再映射

在 `h264bsd_deblocking.c::GetChromaEdgeThresholds` 的 TOP/LEFT 外边框里，
把“邻侧先 `h264bsdQpC[clip(qpTmp+off)]` 映射、再与已映射的当前侧
`qp` 做 `(qp+qpTmp+1)>>1`”替换成“先对原始 `mb->qpY` 与
`mb->mbB/mbA->qpY` 做 `(qpY+qpTmp+1)>>1`，再对平均值做一次 `QpC` 映射”。

`bash task_support/run.sh target` 的第一个失败：

```
FAIL tests/deblock_contract.c:422 ok_
  CHROMA_CFG orient=0 qp=(30,37) chromaOff=2 offA=6 offB=-2
  FIRST_DIFF plane=Cb x=7 y=5 lib=130 ref=131
```

即 `threshold/chroma_qp_map_order` 用例在第 1 个扫描配置（2×1、左外边）
就发现 Cb 像素相差 1。只改 TOP 一处时，会在 `orient=1`（1×2、顶外边）
被抓到，首个配置为 `qp=(30,37) chromaOff=2 offA=6 offB=4`，
`FIRST_DIFF plane=Cb x=2 y=7 lib=125 ref=126`。

### 退化 B：亮度每 4 行把水平边挪到竖直边之前

在 `h264bsd_deblocking.c::FilterLuma` 的
`for (vblock = 4; vblock--;)` 循环体中，把整段水平边
（`tmp[0..3].top` 的 `FilterHorLuma(Edge)` if/else）移动到四条
`FilterVerLumaEdge` 调用之前。

`bash task_support/run.sh target` 的第一个失败来自第一个用例：

```
FAIL tests/deblock_contract.c:145 ok_
  FIRST_DIFF plane=Y x=4 y=2 lib=105 ref=107
```

即 `strength/intra_edges` 在 Y(4,2) 首先不一致（套件整体退出 4）。
即使只单独运行 order 组，三个用例也都能独立抓到同一退化，各自首个差异为：

```
order/cross_edges_v_then_h  FIRST_DIFF plane=Y x=8  y=2 lib=111 ref=109
order/mb_raster_commit      FIRST_DIFF plane=Y x=42 y=2 lib=128 ref=127
order/sentinels_disabled    FIRST_DIFF plane=Y x=3  y=2 lib=101 ref=99
```

验证完成后两份退化均已还原，`src/` 与 `task_support/` 保持无修改。
