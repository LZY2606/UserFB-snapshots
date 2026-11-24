# 多流时间线回归（tests/ms_timeline.c）

这套回归直接检查多流解码的输出内容：分包（framing）、声道映射（mapping）、
丢包恢复（recovery）三组，参照物是各自持续存在的普通 `OpusDecoder`，
不借助第二个多流解码器，也不调用多流 encoder、内部 `packet_parse_impl`
或多流 pad/unpad。生产编解码器保持原样。

## 独立封包规则

测试用一个立体声和两个单声道普通 `OpusEncoder` 现场编码确定性素材
（各声道频率、相位、调幅都不同），把每帧的 ToC 字节和负载留在自己的
帧数组里。组包完全按 RFC 6716 自己实现：

- 时长来自测试自带的 ToC 配置表（§3.1 Table 2：SILK 10/20/40/60 ms、
  hybrid 10/20 ms、CELT 2.5/5/10/20 ms），长度来自帧数组，不用
  `opus_packet_get_*` 反推。
- 长度字段按 §3.2.1 自己编码：小于 252 单字节，否则双字节
  （`252+((n-252)&3)`、`(n-252)>>2`）。CBR 立体声帧 330 字节走双字节，
  单声道帧 120/160 字节走单字节，两种宽度都有断言覆盖。
- ToC code 0/1/2/3 都由真实的 20 ms 单帧负载组合：code 1 用 CBR 等长帧，
  code 2 用 VBR 变长帧（显式扫描长度不同的相邻对），code 3 覆盖 CBR 与
  VBR 两种形态以及 padding（7 字节单字节长度、300 字节多字节长度、
  37 字节）。组合前断言各负载 ToC 配置相容（`toc & 0xFC` 一致）、原始包
  确实是单帧（`toc & 3 == 0`）、负载不是只有头的空包。
- 拼多流包时，前两个流按附录 B 改写成 self-delimited（在普通布局的
  帧数据前补一个末帧长度），最后一个流保持普通格式，三段直接拼接。
  参照解码器拿到的是同一批帧的普通格式包，因此两边解码的是同样的
  帧序列、同样的解码器历史。

每次交付检查：多流解码返回的帧数等于按 ToC 表独立算出的样本数、输出区
前后哨兵未被踩、输出先污染再逐样本比对（framing/recovery 精确相等，
mapping float 容差 2e-6、int16 最多 1 个量化单位）。失败时打印组、事件
序号、stream、输出声道和第一个不同的采样（`MS_TIMELINE_DIFF`）。

## 事件时钟

recovery 组把一次回放看成 24 次交付的事件时间线，每条单流参照走同一条
时间线，不每包重建状态：

- 事件 0–7：热身，正常解码。
- 事件 8：包 8 丢失。用包 9 以 `decode_fec=1` 补出包 8，交付坐标是
  `8*960`；随后包 9 再以 `decode_fec=0` 正常解码，坐标 `9*960`。
  两次交付各推进一帧，绝不能把 FEC 和正常解码错误地各推进两包。
- 事件 14/15：连续两包 PLC（`len=0`），事件 16 起恢复正常。
- 每个事件断言交付起始坐标等于 `event*960`，收尾总坐标等于 `24*960`。

冗余必须靠输出证明：另跑一条历史完全相同的平行时间线，在事件 8 用纯
PLC。断言至少一个流的 FEC 结果既不同于同初态 PLC，也不同于把包 9 当
普通包解码——`OPUS_SET_INBAND_FEC` 成功不代表包里真有冗余。CELT-only
（restricted lowdelay）素材单独验证 FEC 请求退回 PLC：与纯 PLC 逐样本
相等，且丢包后继续播放的历史也逐样本相等。整段回放结束后
`OPUS_RESET_STATE` 所有 decoder 重放，和同配置新实例的输出逐样本一致。

## 现有测试的缺口

- `tests/random_config.sh` 驱动的随机编解码测试只核样本数、final range
  和 API 返回值，从不把多流输出和独立参照逐样本比对；分包或映射错了
  只要帧数对就能蒙混过关。
- `tests/test_opus_projection.c` 的矩阵回归拿 multistream decoder 当
  参照，多流路径自身的错误会被参照"继承"，无法暴露。
- `task_support/baseline.c` 只核四路能量非零，`legacy_projection.c` 是
  历史基线，都不涉及 self-delimited 分包、255 静音声道、重复声道或
  FEC/PLC 时间线。
- 没有任何现有测试覆盖：ToC code 0–3 的多流组合、单/双字节自定界长度、
  code 3 padding、映射的重排/复制/静音、`decode_fec` 在多流路径的传递。

## 退化检测

`task_support/mutants.sh` 先确认未修改的套件通过，再分别应用两份退化：
多流路径丢掉 `decode_fec`（recovery 组事件 8 的内容断言检出）、coupled
右声道复制左声道（framing/mapping 组的声道 1 内容断言检出）。两者都必须
以 driver 状态 4 加 `tests/ms_timeline.c` 的 TASK_CHECK 失败检出，构建
失败或崩溃不算。
