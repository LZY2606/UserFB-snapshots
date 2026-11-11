# sync-clock 回归测试证据说明

本文件说明 `tests/sync-clock.c` 与 `tests/check-sync-clock.sh` 如何为
libltc 的采样位置同步提供**独立于库自身实现**的参照，以及三类典型失误
在测试输出中的可见影响。

## 1. 独立的边沿参照

### 1.1 期望数据不经过本库产生

测试不调用库 encoder 产生期望波形，也不调用 `ltc_time_to_frame` /
`ltc_frame_to_time` / `ltc_frame_increment` 等转换函数产生期望值：

- 十个 LTC 字节直接按 `src/ltc.h` 中 `struct LTCFrame` 的位布局拼成
  80 个线上比特：8 个 BCD 字段（frame/sec/min/hour 的个位与十位）、
  8 个 user 半字节（bit 4-7、12-15、…、60-63）、bit 59 的 25fps 奇偶位，
  以及 bit 64-79 的同步字 `0011 1111 1111 1101`（线上 MSB 在先）。
- BCD 由测试自带的十进制拆位代码写入（LSB 在先），时间序列
  01:02:03:04 起逐帧递增，进位由测试自己计算。
- 奇偶位在**同步字写入之后**统计全部 80 位（跳过 bit 59）中 1 的个数，
  取偶校验，与 `ltc_frame_set_parity` 的 25fps 规则一致但独立实现。
- biphase-mark 波形由独立构造器生成：每个 bit 起点翻转一次电平，
  bit 值为 1 时在半 bit 处再翻转一次。

库 encoder 的常规编码回读只保留为 `suite_encoder_crosscheck` 中的
次要对照（同一帧的原始字节必须与独立波形解出的字节一致），它替代不了
独立波形本身。

### 1.2 已知边沿采样表

对全局第 b 个 bit（跨帧连续编号，b = 0 … 80*NFRAMES），其起点与
半 bit 边沿按累计有理位置取上整：

    edge[b] = ceil(b * R / (fps*80))
    half[b] = ceil((2b+1) * R / (2*fps*80))

- 48 kHz / 25 fps：bit 宽恰为 24 采样，每帧 80 bit 恰占 1920 采样，
  边沿表与帧边界精确对齐。
- 44.1 kHz / 25 fps：bit 宽 22.05 不是整数。若每 bit 独立截短，录音会
  持续漂移；累计公式把误差保留在分子里，相邻边沿间距在 22 与 23 之间
  交替，整帧宽度恒为 1764 采样。测试还断言边沿表严格单调。

每个完整帧实际占据的音频区间即 `[edge[80k], edge[80(k+1))`，解码器的
`off_start` / `off_end` 直接与该表核对，而不是只比对时码字符串或帧数。

## 2. 捕获区间与方向

序列含 7 个完整帧，保证存在足够多的内部帧。

### 2.1 正向

- 48 kHz：稳态帧（不含首个估计帧）的 `off_start == origin + edge[80k]`、
  `off_end == origin + edge[80(k+1)] - 1`，要求**精确**相等。
- 44.1 kHz：允许一个 bit 宽（ceil(R/2000)=23）以内的估计误差；这是该
  采样率下文档化的精度契约。
- 首个捕获帧缺少前导同步字，估计只受“一帧跨度以内”的宽松约束，
  但帧身份必须正确、**不得丢帧**。

### 2.2 反向与极性

整段录音做样本级反转，再分别叠加极性翻转（四种组合都测）。反向播放时
先遇到的是相邻帧被反转的同步字，`LTCFrameExt` 注释说明库会做
16-bit 的位置补偿；因此：

- 反向序列的首帧（对应原录音最后一帧）属捕获阶段，只做有界估计，
  内部帧不允许丢弃——7 帧输入固定解出 6 个反向帧。
- 内部反向帧 k 占据反转坐标
  `[len-1-edge[80(k+1)]+1, len-1-edge[80k]]`，48 kHz 下与边沿表
  精确吻合，`reverse` 必须非零。
- 极性翻转不改变时码身份；仅首个估计帧的起点可能偏移，内部帧位置不变。

### 2.3 biphase_tics

每帧 80 项 `biphase_tics` 之和必须等于边沿表给出的帧宽
（48 kHz 精确等于 1920；44.1 kHz 与 1764 的差在一个 bit 宽内）。
对非捕获帧，还要求该和等于 `off_end - off_start + 1` 的输出跨度。
这防止只靠时码字符串或总帧数通过测试。

### 2.4 输入入口、分块与 64 位偏移

同一段正向/反向录音分别经五个公开入口喂入：
`ltc_decoder_write`（u8）、`_write_u16`、`_write_s16`、`_write_float`、
`_write_double`。电平选用各格式可精确表达的双电平（如 ±1.0、
±32512、0x0100/0xff00），量化后恰为内部 u8 的 1 与 255，量化差异不会
被误判为库错误。

每次测试分别整段送入，以及按 1、317、1023、1024、1025 采样分块送入
（1024 正好等于 PCM 转换模板的内部缓冲，专门覆盖跨转换块的 posinfo
累加）。所有调用的 posinfo 按真实输入偏移递增，并把整段起点平移到
2^33（8589934592）以上，验证 64 位偏移在每条入口路径上都得以保留。

## 3. 队列消费

- 容量为 3 的环形队列配合及时读取处理 7 帧：写指针至少绕过两圈，
  断言每帧身份连续、位置为 `origin + 1920*k`，且 7 帧全部读出，
  不产生未定义溢出。
- 在恰好送入半帧（960 采样）后调用 `ltc_decoder_queue_flush`：此时队列
  本应为空；flush 只清已完成输出，正在解析的 bit 状态必须与后续音频
  接续，最终仍解出全部 7 帧且首帧起点等于 origin。
- 先送入两帧（队列中确有两个完成帧）再 flush：旧输出被清空、读取返回
  0，而后续帧 2..6 无缝到达，身份与位置连续，无丢失或重复。
- 连续两次 flush 同样不得触碰解析状态。

## 4. 三类失误的可见影响（变异测试）

`check-sync-clock.sh --mutations` 在三个独立源码副本中各引入一处可编译、
但行为退化的改动；原库先全通过，随后每个变体必须以普通断言失败
（退出码 1 且打印 `ASSERT_FAIL`）被检出。编译失败、崩溃或超时都不算
成功检出，改动也不会写回 `src/`。

1. **PCM 转换不再按 1024 采样分块累加 posinfo**
   （`ltc.c` 模板中 `posinfo + copyStart` 退化为 `posinfo`）：
   四个非 u8 入口在跨转换块后位置逐块错位，48 kHz 的
   `matrix.steady48k.exact_start/exact_end`、
   `matrix.origin_above_2pow33`、`matrix.capture_estimate`、
   `matrix.tics48k.sum_span` 以及 44.1kHz 对应断言失效。u8 路径不受影响，
   因而该变体只在对应入口上暴露，定位明确。

2. **反向输出漏掉 16-bit 同步字位置补偿**
   （`decoder.c` 反向分支的两处 `- 16 * snd_to_biphase_period` 被删）：
   反向内部帧整体平移约 384 采样（16×24），
   `matrix.steady48k.exact_start/exact_end` 与
   `matrix.steady44k.within_bitwidth` 失败；正向帧与时码身份仍然正确，
   说明这是纯反向同步位置缺陷。

3. **`queue_flush` 额外重置正在解析的 bit 计数**
   （`ltc_decoder_queue_flush` 中加入 `d->bit_cnt = 0`）：
   半帧处 flush 后解析器丢失比特对齐，帧边界整体后移一帧并少解一帧，
   `queue.flush_mid.frame_count`、`queue.flush_mid.first_identity`、
   `queue.flush_mid.first_position`、`queue.flush_mid.continuity`、
   `queue.flush_mid.edge_aligned` 与 `queue.flush_repeat.*` 被触发；
   而“先完成两帧再 flush”的用例仍通过，恰好区分“清队列”与“重置解析
   状态”两种语义。

## 5. 运行方式

    bash tools/baseline.sh
    bash tests/check-sync-clock.sh
    bash tests/check-sync-clock.sh --mutations

脚本在临时目录中编译原库与同一份测试，任何断言失败均以非零状态退出，
并以四个短标记汇总：`LTC_SYNC_ORIGINAL_PASS`、
`LTC_SYNC_OFFSET_MUTATION_CAUGHT`、
`LTC_SYNC_REVERSE_MUTATION_CAUGHT`、
`LTC_SYNC_FLUSH_MUTATION_CAUGHT`，同时打印每个变体实际失败的断言名。
