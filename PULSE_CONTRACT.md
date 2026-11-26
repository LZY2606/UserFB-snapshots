# AMR-WB 代数码本脉冲契约测试说明

## 1. 测试组成

固定驱动 `support/target.sh` 编译并运行 `tests/pulse_contract.c`。三组测试分别对应 `support/check.h`：

- `test_inverse`：不调用生产量化器，独立解释 1/2/3 脉冲索引，并检查四轨交织后的 64 采样激励。
- `test_serial`：用公开 VO API 选择 `VOAMRWB_DEFAULT` 参数流格式，从真正输出的 16 位 `BIT_0/BIT_1` 序列按布局取出索引，再与观察到的原始 `code[]` 逐采样比较。
- `test_stream`：从 `E_IF_encode()` 进入完整编码器，轮换 mode 1..4（ACELP 20/36/44/52 bit），比较 PULSE_TRACE 观察到的每个子帧；另外构造确定性的目标向量 `xn`、残差 `cn` 和滤波器响应 `H`，用 `cor_h_x(H,xn,dn)` 生成合法相关向量后调用完整 `ACELP_4t64_fx()`，补齐普通 PCM 不容易稳定走到的轨首、轨尾、重脉冲和三脉冲半区分支。

一次正常运行的实际计数为：

```text
GROUP inverse checks=1174
GROUP serial checks=13754
PULSE_COVERAGE subframes=608 samples=38912 indices=2432 ...
GROUP stream checks=37870
PULSE_COVERAGE subframes=608 mode_subframes=152/152/152/152 samples=38912 indices=2432 tracks=2432 pulses=5168 duplicate_tracks=897 equal2=406 same_order2=245 opposite2=261 head=346 tail=305 pair_low=483 pair_high=429 singleton_same=441 singleton_other=471 callbacks=608
AMRWB_PULSE_OK checks=52798
```

`PULSE_COVERAGE` 中的 `duplicate_tracks`、`equal2`、`opposite2`、`head`、`tail`、`pair_low`、`pair_high` 等都是实际解码和比较后累计的事件数，不是只检查模式名称。

## 2. 独立逆索引的依据

四轨在 64 个采样中按模 4 交织。轨号 `t`、轨内位置 `q`（0..15）对应的绝对采样为：

```text
sample = 4*q + t
```

测试端的 `inverse_1p()`、`inverse_2p()`、`inverse_3p()` 是按索引位布局独立实现的，没有复用 `quant_*` 或从生产 `code[]` 反推期望值。

### 2.1 一脉冲（20 bit 模式，每轨 5 bit）

```text
bit 4   : 符号，1 表示负脉冲
bit 3..0: q
```

### 2.2 两脉冲（36 bit 每轨 9 bit；44 bit 后两轨也是 9 bit）

令：

```text
S = bit 8
H = bits 7..4
L = bits 3..0
```

生产 `quant_2p_2N1(..., N=4)` 不是把最高位当成两个独立符号位。位置大小关系本身参与符号编码：

- `H <= L`：两脉冲同号，`H/L` 是较小/较大位置；二者符号都为 `S`。
- `H > L`：两脉冲异号，`H` 是较大位置且其符号为 `S`，`L` 是较小位置且符号相反。
- `H == L` 时，由于每个绝对位置在一次搜索中只有一个 `sign[i]`，两个脉冲必然同号并叠加。

递归三脉冲中的两脉冲部分使用同一规则，只是 `N=3`：`S` 在 bit 6，`H/L` 各为 3 bit。

### 2.3 三脉冲（52 bit 每轨 13 bit；44 bit 前两轨也是 13 bit）

13 bit 布局为：

```text
bits 12..8: 单脉冲，按 1P/N=4 解释（bit12 是其符号，bits11..8 是 q）
bit 7     : 递归脉冲对所在的半区 h
bits 6..0 : 两脉冲索引，按 2P/N=3 解释
```

递归对解出的两个 0..7 位置要加 `8*h`，恢复成 0..15 的轨内位置。

44 bit 模式的轨长度是 `[3,3,2,2]`，索引宽度是 `[13,13,9,9]`；不能按四条等长三脉冲轨处理。36 bit 为四条两脉冲轨，52 bit 为四条三脉冲轨，20 bit 为四条一脉冲轨。

### 2.4 累加和幅度

观察点记录生产代码实际使用的单脉冲幅度：

```text
A = 512 >> h_shift
```

目标模式中为 512 或 256。重建时使用：

```c
excitation[4*q + track] += sign * A;
```

这里必须是 `+=`：同一位置的同号多脉冲会形成 `±2A` 或 `±3A`，不能把数组当成位置集合。测试还检查 `sum(abs(code)) / A` 等于该子帧按索引解出的脉冲总数。

## 3. PULSE_TRACE 观察点

新增内部测试头：

```text
tests/pulse_trace.h
```

`amrwbenc/src/c4t64fx.c` 只在定义 `PULSE_TRACE` 时包含该头并调用 `pulse_trace_callback()`。调用点位于 `ACELP_4t64_fx()` 末尾：

- `code[64]` 已按最终 `codvec[]/sign[]` 建好；
- `_index[0..3]` 已按 20/36/44/52 bit 布局生成；
- 函数尚未返回给 `coder()`；
- 因而在 `voAMRWBEnc.c` 后续的 `Preemph(code, ...)`、`Pit_shrp(code, ...)` 和高码率音高增强之前。

事件记录：

```text
nbbits、ser_size、pulse_amplitude、code[64]、index[4]、magic
```

未定义 `PULSE_TRACE` 的普通构建中没有该 include、回调调用或额外事件结构，也没有新增堆分配；算法和公开 API 未改。`support/baseline.sh` 仍输出：

```text
AMRWB_BASE frames=12 bytes=405 hash=946cf075
```

## 4. VisualOn 默认参数流布局

`wrapper.c` 给 `E_IF_*` 选择的是 RFC3267 字节格式；为了检查题目要求的 16 位 `BIT_0/BIT_1` 参数流，`test_serial` 直接使用同一个公开 VO API，并设置：

```c
frame_type = VOAMRWB_DEFAULT; /* 0 */
```

`PackBits()` 在 `bitstreamformat == 0` 时向 `unsigned short outputStream[]` 写入：

```text
word 0: 0x6b21              TX_FRAME_TYPE 同步字
word 1: 0                   TX_SPEECH
word 2: mode                1..4
word 3..: 每个参数位一个 16 位字
         BIT_0 = -127       （unsigned short 为 0xFF81）
         BIT_1 =  127       （0x007F）
```

测试自己的逐位读取器直接读取这些输出字，按 MSB-first 拼回参数值；没有扫描字节或搜索匹配整数。

mode 1..4 的参数位布局如下。ISF 对这些模式均为 46 bit，宽度 `8,8,6,7,7,5,5`；帧首另有 1 bit VAD 标志。

| codec mode | ACELP | 基音延迟（4 个子帧） | LT 选择 | 每子帧码本索引宽度 | 增益 | 总参数位 |
|---|---:|---|---:|---|---:|---:|
| 1 / 8.85k | 20 bit | 8,5,8,5 | 无 | 5,5,5,5 | 6 bit | 177 |
| 2 / 12.65k | 36 bit | 9,6,9,6 | 每子帧 1 bit | 9,9,9,9 | 7 bit | 253 |
| 3 / 14.25k | 44 bit | 9,6,9,6 | 每子帧 1 bit | 13,13,9,9 | 7 bit | 285 |
| 4 / 15.85k | 52 bit | 9,6,9,6 | 每子帧 1 bit | 13,13,13,13 | 7 bit | 317 |

因此码本索引在读取器中的位置是：

```text
1 bit VAD + 46 bit ISF
+ 当前子帧之前各子帧的 [基音位 + LT选择位 + 全部码本索引位 + 增益位]
+ 当前子帧的 [基音位 + LT选择位]
```

随后依次读取该子帧的四个码本索引。测试计算出的总位数与 `PackBits()` 的 `nb_of_bits[mode]` 以及实际输出字数共同核对。

## 5. 确定性覆盖

`test_inverse` 包含：

- 全部 32 个一脉冲码字；
- 全部 512 个两脉冲码字的结构解码；
- q=0/q=15 的轨首、轨尾；
- `H==L` 的同位置重脉冲；
- `H>L` 的异号且位置顺序相反编码；
- 13 bit 三脉冲的三种递归分解分支，以及高/低半区不对称位置。

`test_stream` 和 `test_serial` 使用内容随帧号变化的 PCM，连续帧轮换 mode 1..4。构造套件按 4 × 96 个确定性种子生成 `xn/cn/H`，经 `cor_h_x()` 得到合法 `dn` 后执行完整 `ACELP_4t64_fx()` 搜索，而不是直接给 `quant_*` 喂位置。正常运行中实际观察并比较了 608 个子帧、38912 个采样。

## 6. 两个变异分别由什么实际数据发现

固定 `support/target.sh` 按 `inverse -> serial -> stream` 运行；提供的 `unit_check()` 在第一次失败时输出 `ASSERT` 并以 42 退出。因此下面的“首次失败组”是固定驱动中的实际发现位置。

### 6.1 `sign2`：丢掉两脉冲索引最高符号位

变异：

```c
- return (index);
+ return (index & 255);
```

固定驱动中由 **serial 组**首次发现：

```text
ASSERT serial frame=1 subframe=0 mode=2 sample=0 bits=36
idx=[16,159,14,64] amplitude=512 actual=512 expected=-512
```

这是第 2 个默认格式串行帧（frame 从 0 开始），mode 2 / ACELP 36 bit，第 1 个子帧。逐位读取器从真正的 `BIT_0/BIT_1` 输出中解析出轨 0 索引 `16 (0x010)`。

按两脉冲逆索引：

```text
S=0, H=1, L=0, H>L
=> 大位置 q=1 为正，小位置 q=0 为负
=> sample 0 应是 -512
```

但 PULSE_TRACE 记录的原始 `code[0]` 是 `+512`。变异前该索引应为 `272 (0x110)`：`S=1,H=1,L=0`，即 q=1 为负、q=0 为正，正好与 `code[0]=+512` 一致。`& 255` 删掉的正是承载符号关系的 bit 8。

如果单独运行 stream 组，它也独立以 42 发现该变异，例如：

```text
ASSERT E_IF frame=1 subframe=0 mode=2 sample=26 bits=36
idx=[135,218,104,93] amplitude=512 actual=-512 expected=512
```

### 6.2 `track44`：交换 44 bit 模式最后两轨索引

变异把 44 bit 后两条两脉冲轨写成 `_index[track ^ 1]`。固定驱动中同样由 **serial 组**首次发现：

```text
ASSERT serial frame=2 subframe=0 mode=3 sample=6 bits=44
idx=[8039,435,225,253] amplitude=512 actual=0 expected=-512
```

这是 frame 2、mode 3 / ACELP 44 bit、subframe 0。串行流解析出的后两轨索引为：

```text
track2 = 225 (0x0E1, 9 bit)
track3 = 253 (0x0FD, 9 bit)
```

`225` 的两脉冲解释为 `S=0,H=14,L=1,H>L`：q=1 为负、q=14 为正。若按 track 2 交织，q=1 对应绝对采样：

```text
4*1 + 2 = 6
```

所以独立重建期望 `code[6] == -512`，但实际 `code[6] == 0`。原因是变异把原本按 track 3 搜索得到的索引放到了 track 2；同一个 q=1 脉冲实际位于 track 3 的 sample 7，而不是 sample 6。

单独运行 stream 组也会以 42 发现同一交换，实际 E_IF 数据为：

```text
ASSERT E_IF frame=2 subframe=0 mode=3 sample=34 bits=44
idx=[3614,8054,226,130] amplitude=512 actual=512 expected=0
```

固定驱动在 serial 组第一次 `ASSERT` 后按设计退出，因此不会继续打印 stream 组；上面的 stream 行来自只调用 `test_stream()` 的临时驱动，用于确认流组自身也有足够的检测能力。
