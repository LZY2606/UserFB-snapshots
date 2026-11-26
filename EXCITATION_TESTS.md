# Excitation reference tests

## 运行

```sh
bash task_support/run.sh target
bash tests/check_excitation_mutants.sh
```

`task_support/run.sh target` 会用系统 C99 编译器构建现有库、`task_support/driver.c`
和 `tests/excitation_reference.c`。测试文件不定义 `main`，而是通过
`task_support/check.h` 注册：

- `task_cases[]`
- `task_case_count`

运行器依次执行 `pitch`、`innovation`、`frames` 三组中的真实用例。任何样本不一致都会打印：

```text
CONTENT_MISMATCH phase=<阶段> subframe=<子帧> sample=<样本或状态项> expected=<期望值> actual=<实际值>
```

随后失败的 `TASK_CHECK(0)` 使用例返回 1，运行器按约定返回退出码 4。

`tests/check_excitation_mutants.sh` 把 `task_support/`、`src/`、`include/` 和
`tests/` 复制到临时目录，在副本中分别引入两个可编译变体：

1. 将正分数基音分支的基点调整 `-(intPitchDelay+1)` 颠倒为
   `-(intPitchDelay-1)`；
2. 在固定码本解析出末脉冲 `jx` 后执行 `jx ^= 1`，反转末脉冲轨道选择位。

脚本要求两个变体都输出 `CONTENT_MISMATCH` 且 `task_support/run.sh target`
退出 4。编译失败、崩溃或普通非零退出都不算检出。原源码树不会被脚本修改。

## 独立参照的边界

参照实现在 `tests/excitation_reference.c` 内使用自己的数组、字段解析、
`int64_t` 累加和显式定点舍入：

- 偏置右移写成明确的 `floor((x + 2^(q-1)) / 2^q)`；
- 16 位饱和和回绕加法单独建模；
- 基音插值按分数符号选择基点、相位和两侧各 10 个有限卷积抽头；
- 短滞后时直接在同一个按样本递增写入的数组上读取先前结果，因此包含当前子帧的因果自读；
- 固定码本从 13 位 `C`、4 位 `S` 和额外的 `jx` 位独立恢复四个脉冲，先构造
  Q13 基础向量，再按样本顺序使用旧的受限基音增益锐化；
- 帧级测试用自己的 MSB 字段打包生成合法十字节 G.729 语音帧，基音校验位也由
  测试内的独立奇偶函数写出，不调用 encoder 生成期望。

`b30[31]` 是线格式对应的滤波器系数常量，本测试直接读取该表；没有调用生产的
自适应码本插值、固定码本生成或生产舍入宏来计算期望值。生产
`decodeAdaptativeCodeVector` 和 `decodeFixedCodeVector` 只作为被测实际值来源。

帧组中，增益预测和增益码本不属于本次独立运算证明的范围。测试在另一个由
`initBcg729DecoderChannel()` 建立的独立上下文上调用原来的 `decodeGains`，并向它
传入参照合成的固定码本向量。该上下文的 `previousGainPredictionError` 与真实
decoder 状态也会逐次比较。除此之外，两个子帧的自适应激励、固定激励、Q14 合成
舍入和跨帧 80 样本移位都由测试自己的参照状态推导。

固定码本锐化严格使用本子帧增益更新前的 `boundedAdaptativeCodebookGain`；新解出的
自适应增益经 `[3277,13107]` 限幅后只保存给下一子帧。

## 覆盖内容

### pitch

- 第一子帧 P1 的绝对索引解释，覆盖分数 `-1/0/+1`；
- 明确检查 P1=196（`<197` 分支边界）和 P1=197（`>=197` 分支边界）；
- 第二子帧相对第一子帧整数滞后的 `tMin` 窗口，覆盖：
  - 第一滞后 19 时向下限到 20；
  - 第一滞后 25 时正好得到 20；
  - 第一滞后 139 时正好得到 134；
  - 第一滞后 143 时向上限到 134；
- 对每个用例比较全部 40 个自适应激励样本、输出整数滞后和
  `previousIntPitchDelay`；
- 低滞后用例确认卷积确实发生当前子帧因果自读，并且没有读取尚未生成的样本；
- 连续子帧覆盖基音校验错：错误只 concealed 第一子帧，第二子帧仍按 P2 和第一
  子帧输出的整数滞后计算；
- 显式丢帧覆盖两个子帧依次推进旧滞后，并在 143 封顶。

历史缓冲使用确定性的小幅整数模式，不读取外部语音或码流向量。

### innovation

- 独立恢复四个脉冲的轨道位置和正负符号；
- 末脉冲的 `jx=0` 和 `jx=1` 两条轨道都有用例；
- 包含混合符号、全负符号和不同轨道组合；
- 使用 19–29 等真实 G.729 可允许的短滞后验证脉冲向后续样本传播及顺序递归锐化；
- 每次比较完整 40 个 Q13 样本，而不是只检查非零或脉冲数量；
- 用例统计 Q14 锐化乘法余数，实际覆盖半值舍入阈值的两侧。

### frames

- 调用公开的 `bcg729Decoder`，输入为本测试自行打包的正常 10 字节活跃语音帧；
- 共 8 帧，并建立两个 decoder 上下文逐帧交错调用；
- 比较真实 decoder 移位后位于 `excitationVector[74..153]` 的本帧 80 个激励样本；
- 比较 `previousIntPitchDelay`、`boundedAdaptativeCodebookGain` 和增益预测误差状态；
- 每帧检查 PCM 非零样本数和连续非零段，确认解码输出持续有声；
- 增益字段表刻意安排相邻子帧的旧/新增益不同，以暴露增益更新顺序错误。

帧组不要求有损 PCM 等于某个原始语音信号，也不重新实现或独立证明 LSP 解码、
LSP 插值、LPC、`qLSP2LP`、LP 合成或后滤波。SID、RFC 3389 聚合负载、CNG/舒适
噪声和 DTX 路径均不在本测试扩展范围内。

## 旧测试的缺口

`test/src/` 下的模块测试主要是 CSV/文件驱动程序：读取外部输入向量、调用模块、
写出 `.out` 或 `.pcm`，再由测试战役和外部基准结果比较。它们适合固定向量回归，
但存在本测试针对的缺口：

- `decodeAdaptativeCodeVectorTest.c` 的注释说明每个子帧都可从输入文件重新载入
  完整激励历史，因此不会在一个程序状态中独立推导第一子帧最终激励如何成为第二
  子帧的历史；
- 文件基准通常把生产实现输出沉淀为期望值，缺少另一份按字段、宽整数和明确舍入
  写成的运算模型；
- `decodeFixedCodeVectorTest.c` 依赖外部 CSV 行，不能自描述地保证末脉冲两条轨道、
  混合符号、顺序传播和舍入边界都被命中；
- `decoderTest.c` 走公开 decoder 并导出 PCM，但没有直接核对移位后保留的 80 个
  激励、`previousIntPitchDelay` 和受限基音增益；PCM 比对还会把 LPC、后滤波等未在
  本范围内独立证明的模块混入失败定位；
- `task_support/baseline.c` 是编码—解码冒烟和 FNV 哈希检查，能发现大范围行为
  变化，但不能给出激励样本级的期望/实际偏差，也不构成独立参照。

本次新增测试不替换这些旧测试，也不改变生产算法、公开接口或旧测试行为。
