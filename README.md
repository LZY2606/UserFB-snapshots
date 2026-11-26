# h261enc
A useless H.261 encoder with RDO

I do not know how the H.261 video buffer model works.

## 可选的 GOB 预算模式（GOB budget mode）

除原有的固定量化（`H261_RATECTRL_MODE_QUANT`）和整帧 VBR
（`H261_RATECTRL_MODE_VBR`，仍只在整帧结束后更新）之外，新增了按 GOB
结算的链路预算模式。入口在 `src/h261/h261_ratectrl.h`：

```c
h261_gob_budget_config_t cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.bitrate     = 128000;   /* bit/s，整数 1..100000000 */
cfg.fps_num     = 30;       /* 帧率分子，1..1000000 */
cfg.fps_den     = 1;        /* 帧率分母，1..1000000；fps=fps_num/fps_den，比值 1..120 */
cfg.buffer_bits = 16000;    /* 缓冲/预算上限，单位 bit（不是字节），1..1000000000 */
cfg.observe     = on_gob;   /* 可为 NULL */
cfg.opaque      = &mine;    /* 原样传给回调，可为 NULL */

/* 成功返回 1 并把预算历史清零重启；cfg 为 NULL 或数值非法时返回 0，
   且不改变任何现有控制状态。 */
h261_ratectrl_init_gob_budget(&ctx, &cfg);
```

之后照常 `h261_write_picture_header()` + `h261_encode_gobs()` 即可；参考帧
仍按原来的整帧流程在帧结束后更新。再次调用旧的
`h261_init_ratectrl(...)` 会关闭 GOB 预算模式，固定量化/VBR 的码流与以前
逐字节兼容。预算帧率只参与预算计算，不改变 `temporal_reference` 的含义。

完整可编译示例见 `examples/gob_budget_example.c`。

### 结算规则与单位

- QCIF 每帧 3 个 GOB，CIF 每帧 12 个 GOB。按真实编码顺序把所有 GOB 从
  `K=1` 开始编号，第 `K` 个 GOB 完成后的累计预算为精确整数
  `floor(K * bitrate * fps_den / (fps_num * G))`，其中 `G` 是该格式的 GOB
  数（3 或 12）。每个 GOB 的 `quota` 是相邻两次累计预算之差；取整余量以
  整数余数长期保存，不使用浮点。
- 债务 `D` 初值为 0，每组提交后执行 `D += bits - quota`。债务不跨帧清零，
  也不会被裁到 0；因此 `debt_after == coded_total - target_total`。
- `bits`/`quota`/`coded_total`/`target_total`/`debt_after`/`buffer_bits`
  的单位都是 **bit（有效位）**。picture header 的比特计入本帧第一个 GOB，
  其余 GOB 只计自己的 GOB 头和宏块。writer 中跨帧暂留、尚不足一字节的余位
  通过位计数差值自然只计算一次；最终 flush 写出的补齐（padding）位不计入。
  测量只调用 `h261_bitstream_bits()`，不会为了测量提前 flush。

### 候选与选择

每个 GOB 都分别以基础 GQUANT 1..31 真实试编一次，完整经过原有模式搜索、
方差调整、trellis、写码流和重建路径，得到该候选的完整有效位数，以及该 GOB
区域 Y/Cb/Cr 重建相对原输入的逐样本平方误差和 `sse`。不假设量化与成本单调，
也不用残差估算代替真实编码。

所有候选都从同一个已提交状态出发，并在独立的临时位流 writer 与可还原的
工作区中试编；运动搜索缓存、量化继承、位流余位和重建平面都会在候选间恢复，
因此前一个候选不会污染后一个。工作区只保存当前 GOB 的快照、两个最佳结果和
单 GOB 大小的位流暂存，不随视频时长增长，也不保存 31 份整帧。提交时直接
安装中选候选已经生成的字节与状态，不会把它再编一遍；每个 GOB 在真实编码
入口恰好发生 31 次 `h261_encode_gob()` 调用（`trials == 31`）。

- 在满足 `D_before + bits - quota <= buffer_bits` 的候选中，选 `sse` 最小；
  平局依次选 `bits` 更少、基础量化更小者。
- 若没有任何候选放得下（全体超限），则选 `bits` 最小者；平局依次选 `sse`
  更小、量化更小者，并把 `overflow` 置 1。此时仍输出完整、合法的 GOB，不
  丢帧、不截断码流。`overflow == 0` 表示至少有一个候选满足缓冲约束。

### observe 回调与记录借用期

```c
void on_gob(const h261_gob_budget_stats_t *s, void *opaque);
```

每个 GOB 提交后回调一次、且仅一次；试编期间与正式预算推进前不会回调。
`h261_gob_budget_stats_t` 中的字段：

| 字段 | 类型 | 含义/单位 |
| --- | --- | --- |
| `frame_num` | Int32 | 输入 picture 参数里的帧号（原样沿用，不做 `&31`） |
| `gob_number` | Int32 | 从码流 GOB 头取得的 GOB 编号（QCIF 1/3/5，CIF 1..12） |
| `gquant` | Int32 | 中选的基础 GQUANT，1..31 |
| `overflow` | Int32 | 1 = 所有候选都超出 `buffer_bits`；否则 0 |
| `trials` | Int32 | 实际候选编码次数（31） |
| `bits` | Int64 | 本组有效位，bit；本帧第一组含 picture header |
| `quota` | Int64 | 本组预算额度，bit |
| `coded_total` | Int64 | 重启以来累计有效位，bit |
| `target_total` | Int64 | 第 K 组后的累计预算 floor(...)，bit |
| `debt_after` | Int64 | 结算后的债务 D，bit，可为负 |
| `sse` | Int64 | 中选候选 Y/Cb/Cr 的 SSE，样本值平方和 |

`*s` 记录由编码器实例拥有，**只在本次回调期间借用有效**；回调返回后不得再
解引用该指针，需要保留内容请在回调内复制结构体。观察者函数指针与
`opaque` 都保存在该 `h261_context_t` 实例中，两个实例交错使用互不影响。

### 契约测试

`tests/gob_budget_contract.c` 按任务要求通过 `task_cases` /
`task_case_count` 在 `clock`、`selection`、`stream` 三组注册了多个不同名
用例（不自带 `main`）。`bash task_support/run.sh target` 会编译它并并联跑
旧编码基线；测试使用本地平坦/纹理/移动画面实际编码 QCIF/CIF，独立枚举 31
个候选、独立计算 SSE 与排序、独立解析 GOB 编号与 GQUANT，并覆盖同帧内量化
变化、可容纳与全体超限、跨帧余位、落选候选不污染后继 P 帧、两实例交错、
非法配置、长期百万组时钟以及关闭模式下 QUANT/VBR 的字节兼容。
