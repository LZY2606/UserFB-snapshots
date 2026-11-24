# GOLOMB_TESTS.md — ALAC 自适应 Golomb 码流兼容契约

本目录新增了一套**独立逐位模型**驱动的兼容检查，用来在拆除宽字预读优化之前
锁住 ALAC 的 Golomb/游程/符号修正语义以及压缩 SCE/CPE 进 `ALACDecoder::Decode`
的端到端行为。生产代码、已有接口、固定驱动 `task_support/main.c`、固定构建
`task_support/build.sh`、`task_support/target.sh`、变异补丁和旧基线
`task_support/baseline.sh` 均未改动。

## 1. 入口与运行

- 入口：`tests/golomb_contract.cpp`，只实现
  `int task_group(const char *group)`，处理 `codes` / `runs` / `packets`，
  成功返回 `1`；不写 `main`。`main` 与 `task_check`/`TASK_CHECK` 仍由固定的
  `task_support/main.c` + `task_support/check.h` 提供。
- 固定构建（直接编译本入口与全部 codec）：

  ```sh
  bash task_support/build.sh
  bash task_support/target.sh     # 三组 -> 两个变异 -> 旧基线
  .task-build/target codes        # 也可单独运行某一组
  .task-build/target runs
  .task-build/target packets
  bash task_support/baseline.sh   # 512 帧双声道 escape 包逐样本往返，保持不变
  ```

  实测比较计数：`codes checks=2516`、`runs checks=543019`、
  `packets checks=385`，均打印 `DONE <group> checks=...` 与
  `PROCESS_COMPLETE`。

- 固定参数只有 `-Icodec -Itask_support`（没有 `-I.`）。为了让题目要求的字面
  `#include "task_support/check.h"` 从 `tests/golomb_contract.cpp` 解析，增加了
  一个允许的测试头转发垫片 `tests/task_support/check.h`，它只
  `#include "../../task_support/check.h"`；驱动、`TASK_CHECK` 宏、计数与
  `exit(42)` 行为仍然是同一份真实头文件。

- 原编码器材料生成器（C++，**不**参与固定构建）：

  ```sh
  c++ -std=c++11 -O2 -Wno-deprecated-register -Icodec \
      tests/materials/make_material.cpp codec/*.c codec/*.cpp \
      -o .task-build/make_material
  ./.task-build/make_material tests/encoder_compat_material.h
  ```

  它用生产 `ALACEncoder` 压缩确定性的低能量 PCM（断言 `escapeFlag == 0`），
  先做一次精确库内往返自检，再把 16/24-bit 立体声的 cookie、压缩包和期望 PCM
  冻结到 `tests/encoder_compat_material.h`。

## 2. 独立逐位模型（`tests/golomb_model.h`）

模型只用定宽整数算术和自带的 MSB-first 读写器，**不调用**生产的
`dyn_code`、`dyn_get`、`dyn_code_32bit`、`dyn_get_32bit`、`dyn_comp`、
`dyn_decomp`、`jam` 或 `BitBufferRead/Write`。生产 `BitBuffer` 仅用于
`BitBufferInit` 后把数组交给 `dyn_comp`/`dyn_decomp`/`ALACDecoder::Decode`
这些被测入口，以及用 `BitBufferGetPosition` 核对位置。格式常数按
`codec/aglib.h` 的数值在模型内以不与宏冲突的名字重建。

串起来的关键关系：

- **除数是 `m = 2^k - 1`，不是二次幂。**
  `k = min(lg3a(mb >> 9), kb)`，`m = (1 << k) - 1`。
- **截短余数（零余数少一位）。** 令 `div = n / m`、`mod = n - m*div`、
  `de = (mod == 0)`：
  - `de`：`div` 个 `1`，随后 `k` 个 `0`（1 个终止 `0` + 只提交 `k-1` 位），
    长度 `div + k`；
  - 非零余数：`div` 个 `1`、终止 `0`、再写 `k` 位 `mod + 1`，
    长度 `div + 1 + k`。

  解码端先偷看 `k` 位 `v`，记账为 `+k - 1`（只提交 `k-1` 位），仅当
  `v >= 2` 才 `result += v - 1` 并再补消费 1 位；`v < 2`（零余数）结果为
  `pre*m`。这正是 `ag_dec.c` 中
  `tempbits += k; tempbits -= 1; if (v >= 2) tempbits += 1;` 的逐位含义，
  也是 `remainder.patch` 的变异点。
- **`k == 1`** 时 `m == 1` 且没有余数位：码字就是 `n` 个 `1` 加一个终止 `0`，
  解码端走 `if (k != 1)` 的旁路，结果等于前缀计数。
- **九个 `1` 后的直存（escape）。** 当 `div >= 9`（或普通码长度 `> 25`）：
  写/读 9 个 `1`，随后是恰好 `maxBits` 位的原始 `n`。测试覆盖
  `maxBits = 16 / 20 / 24`（SCE/CPE 声道的 `chanBits` 还会有 `17`、`25`
  及 shift-off 后的 `16/17`）。
- **有符号映射与 zmode 修正。** 编码
  `n = 2*|del| - (del < 0) - zmode`；解码
  `ndecode = n + zmode`，以 `ndecode` 最低位作符号：偶数得正
  `(ndecode+1)>>1`，奇数得负。游程后的第一个非零样本 `zmode == 1`，奇偶整体
  平移一位；漏掉 `+ zmode` 会让 `+t -> -t`、`-a -> +(a-1)`。这是
  `zmode.patch` 的变异点。
- **均值推进与钳位。**
  `mb' = pb*(n+zmode) + mb - ((pb*mb) >> 9)`；若 `n > 65535` 则
  `mb = 65535`（此后 `k = 7`）。
- **zmode/零游程。** 每个样本后若 `(mb << 2) < 512` 且后面还有样本，则武装
  zmode，按
  `kr = lead(mb) - 24 + ((mb + 16) >> 6)`、
  `mz = ((1<<kr)-1) & wb` 用 16 位 `dyn_get/dyn_code` 变体写一个游程长度，
  随后 `mb = 0`。游程长度到 `65535` 时 `zmode` 清零，因此紧随
  `65535` 游程的非零值恢复**普通**映射（`65534` 则仍带 zmode）。
- **游程超样本数。** 解码先读游程长度，再做
  `c + nrun <= numSamples` 检查，失败返回 `kALAC_ParamError (-50)`，
  且补零循环在检查之后，故不会改写声明区间之后的输出哨兵。

## 3. Padding 与物理 EOF 约定

每个交给生产入口的缓冲都由 `tests/golomb_image.h` 包成：

```
[ 前硬哨兵 16B 0x5A ][ 逻辑流 streamBytes ][ 可读预读 padding 64B 0xA5 ]
[ 后硬哨兵 32B 0xDB ]
```

- AG 宽字读取 `read32bit()` 一次读 4 字节，`getstreambits()` 跨界时还会读
  第 5 个字节；`kReadPadding = 64` 提供这些**可读**字节。
- `BitBufferInit(&bb, im.p, logicalBytes)` 的 `byteSize`/`end` 仍只命名
  **逻辑长度**，没有用 padding 扩大物理 EOF；`dyn_decomp` 仍以
  `byteSize*8` 为 `maxPos`、以 `cur <= end` 收尾。内存安全/EOF 约定与旧接口
  完全一致，本次不改生产代码。
- 除了 16/32 字节硬哨兵，还核对有效位窗口之前 `startBit` 个高位与之后 32 个
  位哨兵（生产 jam 的读-改-写会保留掩码外位；本测试用到的 ≤24 位直存都落在
  单个 32 位宽字内）。
- 全部三组通过 `-fsanitize=address` 构建运行，无越界报告；UBSan 在
  `tests/` 新增代码中零报告（仅生产代码里既有的非对齐 cookie 结构转换、
  有符号移位等历史报告，属于要保留的既有行为）。

## 4. 三组分别走到的分支

### `codes`
对每个明确有符号残差计划同时做四个方向的交叉核对，且都**逐个残差、逐位
有效位串、实际消费位数**比较：

1. 模型位串 vs 生产 `dyn_comp` 位串/位数；
2. 生产 `dyn_comp` 输出 -> 生产 `dyn_decomp` 残差；
3. 生产 `dyn_comp` 输出 -> 独立模型解析器残差/位数；
4. 独立模型位串 -> 生产 `dyn_decomp` 残差/位数。

命中：`k=1`（用 `mb=256` 使 `mb>>9=0` 得 `k=1` 且 `mb>=128` 不武装 zmode）；
`k=3,m=7` 计划里的零余数（`n=7,14,...`）与非零余数（`n=8,13,...`）；
普通短码与 `n=63/70` 的九一 escape 在同一流内切换；16/20/24 位直存
（`n=0xBEEF / 0xABCDE / 0xABCDEF`，放在对齐最差的 `startBit=7/6/5`）；
富计划在起始 `bitIndex = 0..7` 各跑一遍。结束前用覆盖率布尔断言这些分支
确实被命中。

### `runs`
- 先用 `mb0=5000,pb=40` 的 80 个 `±1` 让均值真实衰减到 `<128` 并产生游程，
  再串接 `0/1/5/33/300` 长度与正负跟随值，模型与生产逐样本/位数一致。
- 确定性低均值块（`mb=10,pb=0`，均值恒定为 10）逐一覆盖游程长度
  `0,1,2,7,40,257,4096,65534,65535`，每种都接正、负目标；核对目标样本的
  **符号映射**、整块结束 `BitBufferGetPosition`，并对 `L>0` 额外解码
  “anchor + L 个零”的前缀，断言其消费位数恰好等于目标主码字的
  `bitStart`（下一个符号的精确位位置）。
- `n=70000` 命中均值钳位：`meanAfter==65535` 且下一个码字 `k==7`。
- 块重置：脏块之后重新 `set_ag_params(mb0,...)` 的结果与全新参数逐样本相同；
  每个游程用例本身也都从新的 `mb0`/`zmode=0` 开始。包级每个声道元素在
  `ALACDecoder::Decode` 内也都会用 `mConfig.mb` 重新 `set_ag_params`。
- 有限输入超样本：`{3}` + 20 个零的流按 `numSamples=8` 解码，断言返回
  `kALAC_ParamError`、`out[0]==3`、其后到守卫区间全部保持
  `0x77777777`、硬哨兵不变，且模型给出同样的参数错误。

### `packets`
全部用 `gp::Writer` 自带位写入器（不用 `BitBufferWrite`）构造合法压缩包，
cookie 也由测试按 24 字节 `ALACSpecificConfig` 布局自行生成（`pb=0,mb=10,
kb=14`，预测阶数 `numActive=0`、`denShift=9`，立体声 `mixBits=mixRes=0`），
因此 `unpc_block` 只复制残差、`unmix16/24` 在 `mixRes==0` 时只做交错，期望
PCM 可直接由残差与 shift 低字节算出。覆盖：

- SCE16 单声道；带游程/游程后正负值的 SCE16（以 `numChannels=2` 缺口调用，
  使其真正读到 `ID_END`，核对读完并字节对齐后的位置 `== pktBytes*8`）；
- CPE16 立体声（含游程后值，逐样本交错 PCM）；
- SCE24 / CPE24 无 shift 的 24-bit 三字节小端 PCM；
- SCE24 与 CPE24 的 1 字节 shift-off：按解码端顺序在系数头之后写入
  shift 区（CPE 为 U/V 交错低字节），AG 的 `chanBits` 分别用 16/17，
  期望样本 `((res<<8)|low) & 0xFFFFFF`；
- partial frame：头置 partial 并写 32 位 `P=13`，以 `BLOCK=64` 调用，核对
  `outNumSamples==13` 与前 13 帧 PCM；
- 同一包内相邻元素：`SCE+SCE+ID_END`（3 声道请求，核对两条单声道 PCM 与
  `ID_END` 位置）以及 `SCE+CPE`（3 声道，布局 `[sce,u,v]`）；
- 连续 5 个不同长度 `5,11,23,37,9` 的 CPE16 包，逐包核对 PCM、帧数与
  `71 + bitsL + bitsR` 的结束位位置；
- 每个压缩包都断言绝对位 22 的 `escapeFlag == 0`，不用 escape 包冒充；
- `P11` 解码冻结的生产 `ALACEncoder` 压缩包
  `tests/encoder_compat_material.h`（16/24 各一），逐字节比较源 PCM，作为
  额外兼容对照；独立构造材料才是主体，库内往返没有替代独立材料。

> 备注：`ID_END` 分支直接 `goto Exit`，会跳过 `NoMoreChannels` 的补零循环，
> 因此“请求声道数多于元素”时缺失声道不会被补零（保持调用方填充值）；测试
> 只对元素实际声明的声道做 PCM 断言，并在 SCE+双声道缺口用例中把该既有行为
> （第二声道保留 canary）固定下来。

## 5. 变异“首差”

用 `task_support/mutations/*.patch` 实测（每个变异都能编译成功，且由同一套
`TASK_CHECK` 内容断言终止，进程精确返回 `42`，不是编译错误/崩溃/无用例）：

- **`remainder.patch`**：`codec/ag_dec.c:256` `tempbits -= 1` 改成
  `tempbits -= 0`，截短余数每次多消费 1 位。首个 `codes` 场景
  （`k=3,m=7,startBit=0`）第一个残差仍正确，第二个符号起错位，首差为

  ```
  CONTENT_MISMATCH tests/golomb_contract.cpp:148 codes: dyn_decomp per-residual
  ```

  进程退出码 `42`；即使绕过逐残差断言，消费位数比较也必然失败。
- **`zmode.patch`**：`codec/ag_dec.c:314` `ndecode = n + zmode` 改成
  `ndecode = n`。`codes` 组因刻意保持高均值、不武装 zmode 而继续全绿；
  `runs` 组第一个用例（游程长度 `0`、目标 `+5`）即失败：`+5` 在
  `zmode=1` 下 `n=9`，正确 `ndecode=10 -> +5`，变异后 `ndecode=9 -> -5`。
  首差为

  ```
  CONTENT_MISMATCH tests/golomb_contract.cpp:259 runs: sample mapping after run
  ```

  进程退出码 `42`。`packets` 组若单独在变异构建上运行，也会在
  `tests/golomb_contract.cpp:508 packets: SCE16 PCM` 从公开 PCM 输出抓到它，
  证明游程后非零值确实到达公开输出。

`task_support/target.sh` 的判定是变异副本上 `run_groups` 返回 `42` 且日志含
`^CONTENT_MISMATCH `。已用与其完全相同的 tar 复制、`git apply`、
`task_support/build.sh` 和判定循环验证两个补丁均输出
`DETECTED task_support/mutations/<name>.patch exit=42`。
