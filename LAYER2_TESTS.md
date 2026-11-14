# Layer II 定点化契约测试（tests/layer2_contract.c）

这套测试为 minimp3 中 MPEG Audio Layer II（`#ifndef MINIMP3_ONLY_MP3` 下的
`L12_*` 分支）的**分组量化**和**尺度因子（SCFSI）复用**提供独立的定点回归。
生产解码算法、公开接口（`mp3dec_decode_frame` 等）和原有 PCM/PSNR 向量测试均未改动；
本文件只新增只读检查，把一次差异定位到具体的**码字（codeword）、子带（subband）
和消费的位数（bit position）**，而不是把同一批 PCM 再解一遍看 PSNR。

## 1. 独立模型，不借用生产实现

测试自带一套与生产代码完全独立的位工具和算术，位于 `tests/layer2_contract.c`：

- `l2_bw_t/l2_bw_put`、`l2_rd_t/l2_rd_get`：MSB-first 的独立位写入器/读取器，
  逐位拼装与逐位读回，不调用生产的 `get_bits`。
- 分组码字的拆分（`code % mod`、`code /= mod`）、去中心（减 `mod/2`）、普通量化
  去中心（减 `2^(ba-1)-1`）、SCFSI 字段到三个尺度区段的映射，全部在模型里独立实现，
  不调用生产的拆组或反量化函数，也不把生产输出缓存下来当期望值。
- 分配表（`l2_bitalloc_code_tab`、`l2_alloc_mpeg1`）作为 ISO 11172-3 的**格式常量**
  在测试中复制一份，仅用于按计划生成合法码字和独立读回。
- 尺度因子期望值用 double 按题目给定公式计算：
  `scf = 2 * pow(2, -index/3.0) / levels`，与生产的浮点结果比较，容差
  `2e-6 * (1 + |expect|)`；码字整数和位消费必须**精确**相等。

被真正调用的生产入口只有受测代码本身：`L12_dequantize_granule`、
`L12_read_scale_info`、`L12_apply_scf_384` 和 `mp3dec_decode_frame`。

### 只读观察点

`minimp3.h` 中仅在定义 `MINIMP3_L2_TEST_OBSERVE` 时，于
`L12_apply_scf_384` 之后、合成滤波器 `mp3d_synth_granule` 之前插入一个
`mp3_l2_observe(igr, sci, grbuf)` 回调，用来记录合成前的 384 个（每声道 576 内的
活动部分）子带值。正常构建（不定义该宏）不产生任何状态、回调或代码体积变化；
baseline 哈希与改动前完全一致。

## 2. 三个检查组

### digits（分组量化，穷举合法码字）

- 对 17/18/19 三种 bit allocation（对应 3/5/9 级、5/7/10 位码字），**遍历所有合法
  三元组**（3³=27、5³=125、9³=729），每个三元组先用显式的位循环生成码字字节，
  再交给真实的 `L12_dequantize_granule`。
- 核对：码字整数能由三个样本 `s0 + s1*mod + s2*mod²` 精确重组；样本先后顺序；
  去中心后的整数值；零位分配子带恒为 0 且不消费位；码字精确占据 width 位及其起止
  位位置；生产端消费位数与模型一致。
- 每组前后夹普通（非分组）量化样本（ba=4 与 ba=3），起始位偏移取 0..7 制造跨字节
  读取。生产位流必须从 bit 0 开始，因此测试把同一组字段从偏移处重新打包成
  bit-0 对齐视图喂给生产函数；偏移缓冲仍由模型独立读回，证明跨字节读取正确。
- 仅覆盖合法码字；非法码字的容错行为不在本契约内定义。

### scfsi（四种 SCFSI 复用模式）

- 用独立位写入器生成四种复用模式（每子带传输 3/2/1/2 个 6 位字段，分别对应
  区段映射 `[f0,f1,f2]`、`[f0,f0,f1]`、`[f0,f0,f0]`、`[f0,f1,f1]`），各段尺度索引
  故意取不同值。
- 核对：真正读取的 6 位字段数量（精确）；每个 12 子带时刻（三个尺度区段）实际使用
  的尺度值；分组与非分组重建后的子带值（容差 2e-6 公式）；码字整数与位消费精确。
- 尺度索引覆盖 0、1、2、62、63（跨三种声道配置汇总）；左右声道从互不相交的索引集合
  取值，保证尺度集合不同。
- 覆盖 joint-stereo 边界**内**左右各有独立码字、边界**外**共用左码字但保留左右两套
  尺度的情况。测试显式建模了生产解析顺序中的一个事实：joint 边界外右槽位在读取
  SCFSI/尺度时仍持有左 bit allocation 的“陈旧”副本（清零发生在其后），因此共用码字
  仍带一套独立的右尺度——断言右尺度既不等于左尺度，也不允许只靠“左右能量非零”蒙混。

### frames（完整帧 / 公开接口）

- 用明确计划生成能被 `mp3dec_decode_frame` 接受的连续 MPEG-1 Layer II 帧
  （192 kbps、44.1 kHz、626 字节/帧；每段配一个相同头的跟随帧以满足帧同步匹配），
  含 mono、普通 stereo、joint-stereo，且每种都同时带分组与普通量化并覆盖全部
  SCFSI 模式。
- 独立读回帧头、bit allocation 及其余字段，算出三个尺度区段的子带期望；通过观察点
  记录生产端 `L12_apply_scf_384` 之后、合成之前的值并逐项比较。
- 公开返回值检查：每帧 1152 样本、消费字节数、`channels/hz/layer/bitrate_kbps`
  格式信息、PCM 非零，以及输出缓冲区前后保护区不被写穿。另有一个连续三帧 mono
  流验证逐帧消费恰好等于流长度。
- 合成滤波器不另写参照；Layer III 的短窗、bit reservoir 和后端选择不在本次范围。

## 3. 与原 PCM/PSNR 测试的区别与“首差”定位

| 维度 | 原 minimp3_test / baseline | 本契约 |
| --- | --- | --- |
| 观测量 | 最终 16 位 PCM，再算 PSNR / 哈希 | 码字整数、子带整数/浮点值、消费位数 |
| 失败定位 | 只有一个聚合误差，无法指到字段 | 消息直接给出 `g/ch/sb/t`（粒度、声道、子带、时刻）或码字位区间 |
| 参考来源 | 解码同一输入 | 独立模型按格式规范重新推导 |
| 整数/位精确性 | PSNR 容忍小偏差 | 码字与位消费必须精确相等，尺度才用浮点容差 |

例如拆组除数被改坏（`task_support/mutations/radix.patch`）时，digits 组在第一个
三元组即以 `digits order/value ba=17 ...` 报出**首差**；SCFSI 复用多读一个尺度
（`reuse.patch`）时，scfsi 组以 `scale-info bit consumption ... got=236 want=212`
报出首差，二者均以 `CONTENT_MISMATCH` 和退出码 42 结束——两份变异源码都能正常编译、
能运行到内容比较阶段，崩溃或构建错误不算检出。

## 4. 如何运行

```sh
bash task_support/baseline.sh     # 原 Layer III / Layer II 各三帧真实解码（保持不变）
bash task_support/build.sh        # 固定入口编译 tests/layer2_contract.c
.task-build/target digits         # 422880 个非零断言
.task-build/target scfsi          # SCFSI 四种模式 + joint 边界
.task-build/target frames         # 完整帧公开接口契约
bash task_support/target.sh       # 三组 + 两份变异 + 联跑 baseline
```

> 注：`target.sh` 用裸 `mktemp -d` 建变异临时目录。在限制写入系统临时目录的沙箱里
> 该命令会因 `Operation not permitted` 失败（环境限制，非测试问题）；变异检出逻辑
> 可在可写目录下等价执行，两份补丁均被确认以退出码 42 + `CONTENT_MISMATCH` 检出。

## 5. 范围与材料

所有新代码、材料生成与示例均为 C，不下载音频、不安装包、不引入新依赖；测试输入全部
在内存中按 ISO 字段计划合成。保留的容错行为（非法码字、超界读取等）沿用生产现状，
本契约不重新定义。
