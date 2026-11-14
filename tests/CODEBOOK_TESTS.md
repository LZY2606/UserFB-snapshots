# 码本契约回归测试（tests/codebook_contract.c）

本文件配套 `tests/codebook_contract.c`，说明这套定点 iLBC 回归如何独立地解释
声音差异来自哪里，以及它对两种定向退化的检出情况。测试只新增文件，不修改
任何生产算法、公开接口或既有测试；它不含 `main`，通过
`task_support/check.h` 的 `task_cases` / `task_case_count` 注册，由
`task_support/driver.c` 驱动。

## 运行方式（全程离线）

```sh
bash task_support/run.sh baseline        # 先构建并跑原有冒烟，输出须与 baseline.expected 一致
bash task_support/run.sh target          # 构建库 + 链接本测试，跑 vectors/residual/stream 三组
bash tests/check_codebook_mutants.sh     # 对两份退化分别重建，要求都以退出码 4 并打印 CONTENT_MISMATCH 告终
```

> 沙箱备注：本环境的系统临时目录（macOS `/var/folders/...`）被 seatbelt
> 拦截，脚本里的 `mktemp` 在沙箱内会失败。固定脚本保持原样未改；本地验证时
> 在 `PATH` 前置了一个只把分配重定向到可写 `$TMPDIR` 的 `mktemp` 垫片
> （`.task-build/shim/mktemp`）。正常系统上无需该垫片，脚本直接可跑。

驱动约定：任一 `TASK_CHECK` 失败时该用例返回 1，驱动对失败用例返回 4；
内容差异统一以 `CONTENT_MISMATCH` 前缀打印。构建失败、崩溃或没跑完用例都不
会产生该前缀，因此 `check_codebook_mutants.sh` 同时校验“链接成功 + 出现
CONTENT_MISMATCH + 退出码 4”，把“真的发现内容差异”与“跑不起来”区分开。

## 参照模型的独立性

期望值不允许调用生产的向量、插值、滤波、增益或码本构造函数。本测试把这些
路径在测试文件内按定点规格重新实现了一遍，只读取常量表
（`WebRtcIlbcfix_kAlpha`、`WebRtcIlbcfix_kCbFiltersRev`、
`WebRtcIlbcfix_kGain*`）：

- `ref_augmented`：四点交叠码本。两个乘积各自做有符号 Q15 缩放并**分别截位**，
  再作为 `int16` 相加；不合并成一次舍入。
- `ref_ma_one` / `build_extended`：MA 滤波按有限卷积计算，缓冲两侧用显式零
  延拓，并复刻 `[-134217728, 134215679]` 饱和与 `(o+2048)>>12`。
- `ref_get_cbvec`：按索引算术独立切分四段（未滤波普通段、未滤波增广段、
  滤波普通段、滤波增广段），不调用 `WebRtcIlbcfix_GetCbVec`。
- `ref_gain_dequant` / `ref_gain_chain`：三阶段增益由上一级**已量化**增益的
  绝对值、下限 1638（Q14 的 0.1）递推。
- `ref_cb_construct`：三阶段加权，`int32` 累加后 `(a32+8192)>>14`。
- `ref_build_residual`：整帧残差的自适应部分，用普通时间顺序的有界数组、
  绝对样本坐标重建；反向时间里“记忆 / 码本索引三元组 / 增益三元组”的消费
  顺序写在注释里，不照搬 `DecodeResidual` 的指针与 `memmove` 循环（滑窗在
  参照里按绝对坐标重建，反向路径用独立的反向时间累加器）。

被显式允许复用的生产入口只有三类，且用途受限：

1. `WebRtcIlbcfix_StateConstruct`——仅用于残差组的**标量起始块**，这是任务
   明确允许的复用；其后的起始补足、前向、后向子帧全部走参照模型。
2. LSF 解量化 / 稳定化 / 插值（`SimpleLsfDeQ`、`LsfCheck`、
   `DecoderInterpolateLsp`）——仅 stream 组，在一份独立解码器状态上推进；
   自适应残差仍由参照模型给出。
3. 线格式辅助（`PackBits`/`UnpackBits`/`SwapBytes`/`IndexConvDec`）与公开
   编解码 API——只负责产生/解析合法码流、做索引线格式转换和驱动 `Decode`。

定点数值约定（与 `-fwrapv` 构建一致，代码注释中亦写明）：

- 16x16 乘积一律在 `int32` 内计算，恒精确（`|a*b| < 2^31`）；
- 有符号右移为算术右移；负数舍入用与生产相同的“先加偏置再移位”表达式；
- 参照中没有负数除法，唯一除法（增广段 `lag = k/2`）作用于严格正的偶数；
- 三阶段加权的 32 位累加依赖与生产相同的有符号回绕语义。

## 三组用例与覆盖

### vectors（6 例，34 项检查）

用普通时间顺序的有界数组描述码本记忆，独立算出并逐样本比对：未滤波普通
向量、四点交叠增广向量（含短起始向量与 40 样本向量、段交界）、滤波向量、
滤波后增广向量，以及三阶段增益递推和最终加权向量。记忆取正负混合值，并
撒入大幅值样本以触及滤波饱和。另含一个**手工构造**样本：令交叠两点都为 1，
证明两次截位得 `0+0=0`、合并一次截位得 `1`，从而把“两次截位 vs 一次截位”
变成可观测差异（见 `vectors.long_augmented`）。

### residual（12 例，48 项检查）

每例自组整帧，标量起始块取自 `StateConstruct`，其余样本全部由参照码本模型
重建后与 `WebRtcIlbcfix_DecodeResidual` 的输出**逐残差样本**比较。覆盖
20ms/30ms、`state_first` 两种位置，以及起始块靠前（startIdx=1）、居中
（20ms=2、30ms=3）、靠后（20ms=3、30ms=5）。居中的 30ms 例同时产生前向与
后向子帧，且断言后向记忆截取确实命中 147 上限（未截取长度 160，截到
`CB_MEML=147`）。

### stream（2 例，1962 项检查）

从确定性的非静音 PCM（弱噪声底 + 中心位置逐帧游走的尖锐突发）经公开
`Encode` 产生两种时长各 96 个连续合法帧，解包并做 `IndexConvDec` 线格式
转换。两个公开解码器实例对同一码流交错解码，第三份独立实例只推进允许的
LSF 路径，自适应残差走参照模型；逐帧比较公开 `Decode` 后的 `prevResidual`，
并核对输出长度、`speechType=ILBC_SPEECH`、复位后重放一致、双实例交错一致。
覆盖计数来自**实际解包出的帧字段**，不是按帧数推断：

| 模式 | 帧数 | 含前向子帧 | 含后向子帧 | 命中147上限 | (startIdx,state_first) 实际覆盖 |
|------|------|-----------|-----------|-------------|----------------------------------|
| 20ms | 96 | 76 | 74 | 0（数学上不可达：有后向子帧时最大快照 120 < 147） | (1,0)=9,(1,1)=13,(2,0)=28,(2,1)=26,(3,0)=18,(3,1)=2 |
| 30ms | 96 | 82 | 83 | 52 | (1,0)=6,(1,1)=7,(2,0)=3,(2,1)=16,(3,0)=19,(3,1)=14,(4,0)=15,(4,1)=2,(5,0)=10,(5,1)=4 |

每个可达 `(startIdx, state_first)` 组合计数都大于 0，并用 `TASK_CHECK`
强制保证；不要求有损 PCM 等于输入，也不把整段哈希当作独立参照。

合计 20 个用例、2044 项内容检查，基线生产代码下全部通过（`TARGET_COMPLETE
cases=20`，退出码 0）。

## 两份退化的第一次差异

`tests/mutants/` 下的补丁分别破坏两处定点细节；两份退化都能编译、链接
（打印 `ILBC_TARGET_LINK_OK`）、跑到内容比较，随后失败并由驱动返回 4。

1. **merged（四点交叠改成一次合并截位，`create_augmented_vec.c`）**
   首个差异：
   `CONTENT_MISMATCH vectors.long_augmented at sample 17 got=6633 want=6632`
   ——合并截位改变了交叠样本的舍入结果，在 `vectors` 组即被逐样本比较拦下。

2. **forward（后向码本记忆改成正向排列，`decode_residual.c`）**
   首个差异：
   `CONTENT_MISMATCH residual.m20.s2.f1 at sample 0 got=3 want=15`
   ——`vectors` 组 6 例全部通过后，在 `residual` 组第一个含后向子帧的用例、
   反向重建的首个样本处暴露记忆方向错误。

两种情形下 `check_codebook_mutants.sh` 都报告
`MUTANT_CONTENT_REJECTED ... exit=4` 并最终打印 `ILBC_MUTANTS_OK variants=2`，
证明差异是在真实内容比较中被发现，而非构建失败、崩溃或中途退出。
