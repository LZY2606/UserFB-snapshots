# PHASE_TESTS — 相位连续性与重叠拼接的数值契约

有声语音合成链在更换 FFT 后端之前，需要一份可执行的数值契约：一次相位跳动
到底来自基频轨迹、LPC 相位采样，还是逆变换后的时间位置，必须能分别定位。
本目录的 `tests/phase_overlap_contract.c` 就是这份契约。生产算法不改；
契约所需的全部期望值都由测试内独立的参考模型计算（自己的复数运算、
直接逆 DFT 求和、分段线性窗定义），不调用生产 FFT 生成期望。

## 运行

```sh
bash task_support/run.sh baseline   # 未改动的基线（不需要本测试）
bash task_support/run.sh target     # 构建并运行本契约 + 基线
bash tests/check_phase_mutations.sh # 退化检出证明（见下文）
```

`target` 模式用 `-DTASK_OBSERVE` 额外构建一份 libcodec2（只增加只读观察点，
见下），普通构建（`baseline` 模式、正常 cmake 构建）完全不含这些代码。

## 观察点（stream 组）

`src/codec2.c` 的 `synthesise_one_frame()` 内、仅在 `TASK_OBSERVE` 宏下挂了
四个只读观察点，由测试文件实现，只记录、不修改任何状态：

| 观察点 | 位置 | 记录内容 |
|---|---|---|
| `task_observe_frame_input` | 相位更新之前 | 模型（Wo/L/voiced/A）、LPC 分析频响 `Aw`、基频时钟 `ex_phase`、帧长、合成窗 `Pn` |
| `task_observe_phase` | 相位更新之后、postfilter 之前 | 含合成相位的模型、更新后的基频时钟 |
| `task_observe_model` | postfilter 之后、`synthesise` 之前 | 后滤波后的真实幅相模型 |
| `task_observe_output` | 增益之后、耳保护与 PCM 裁剪之前 | 浮点输出缓冲 `Sn_[0..n_samp)` |

相位比较在后滤波之前完成（postfilter 会随机化低能量谐波的相位）；重叠参照
消费后滤波之后的幅相模型。观察点按 `CODEC2*` 实例区分，两条实例交错运行时
各自独立记录。

## 参考模型

### 基频时钟与谐波相位（phase 组）

- 频点：谐波 m 的分析频点 `b = (int)(m*Wo/r + 0.5)`，`r = TWO_PI/FFT_ENC`；
  合成频点 `b = (int)(l*Wo*FFT_DEC/TWO_PI + 0.5)`，钳到 `FFT_DEC/2 - 1`。
- 滤波采样：`H[m] = conj(Aw[b])`（测试用自己的复数共轭/乘法）。
- 基频时钟：`ex_phase += Wo*n_samp`，再 `ex_phase -= TWO_PI*floorf(ex_phase/TWO_PI + 0.5)`
  包回一个周期。
- 有声激励：`Ex[m] = cos(m*ex_phase) + i*sin(m*ex_phase)`；
  谐波相位 `phi[m] = atan2(Im(H[m]*Ex[m]), Re(H[m]*Ex[m]) + 1E-12)`。
- 无声分支：随机相位不固定成有声轨迹，只检查值有限、基频时钟仍按同一
  规则推进；全局 `codec2_rand` 不动。

### 合成窗（overlap 组）

窗按分段线性定义独立计算（不是从生产窗口倒推）：

```
w[i] = 0                       i < n_samp/2 - tw 或 i >= 3*n_samp/2 + tw
w[i] = (i-(n_samp/2-tw))/(2tw) 上升段
w[i] = 1                       平台段
w[i] = 1-(i-(3n_samp/2-tw))/(2tw) 下降段
```

生产实现用 float 逐步累加生成同一张窗，与精确值最大偏差约 7.2e-7
（n_samp=80, tw=40）。这一舍入差由逐点容差吸收，测试素材的幅度规模
也据此选取（见误差上限）。

### 直接逆 DFT 与自然时间轴重叠（overlap 组）

- 参照逆变换是直接求和：`sw[n] = Σ_l 2*A[l]*cos(phi[l] + 2π*b_l*n/FFT_DEC)`，
  因子 2 来自实信号的共轭（负频率）镜像；当前 KISS FFT 逆变换增益为 1
  （`FFTI_FACTOR = 1.0`），参照同样不除 N。
- 重叠在自然（绝对）时间轴上进行：每一帧的加窗片段保存到绝对位置
  `T+i`（窗口位置 i 对应合成时间 `n = i-(n_samp-1)`），连续帧的贡献在
  绝对轴上合并；`shift=1` 时 `T += n_samp`，且连接位置 `T+n_samp-1`
  从零开始（上一帧尾部的最后一个样本不被携带），`shift=0` 时在同一
  绝对位置叠加。不照抄生产代码里的数组搬移循环。
- 参考历史（基频时钟、绝对时间轴累加器）跨帧保存，绝不从生产的尾部
  重新初始化。

### stream 组

同一套相位/时间模型接到真实的 `codec2_encode`/`codec2_decode`
（3200 模式，连续帧）。编码器、postfilter、LPC 参数解码沿用生产实现；
至少两条历史不同的实例逐帧交错运行，各自的参考历史独立。除观察点比较外，
还核对公开解码输出的样本数（160/帧）与非静音内容（输出能量门限）。

## 误差上限

- 相位：单位圆距离 `|exp(ja)-exp(jb)| <= 2e-5`。
- 合成缓冲：逐点 `|got - want| <= 2e-3 + 2e-5*|want|`。
- 合成窗（对分段线性定义）：`|got - want| <= 1e-4`。

全部为逐点比较，不用相关系数或总能量代替。测试素材的幅度规模选得使
生产窗的 float 累加舍入（≤7.2e-7）与 KISS FFT 的 float 舍入（实测
≤5e-4 相对量级）远低于上述下限。

## 用例清单

- **phase**
  - `sample_bins`：最近频点取共轭，非实数滤波响应，多组 Wo/L。
  - `voiced_track`：24 帧 ×（8k/16k 两种帧长），时钟跨过 ±π、基频逐帧
    变化、谐波数不同，逐谐波数值相等。
  - `unvoiced_clock`：无声帧相位有限、时钟仍推进。
- **overlap**
  - `window_def`：8k/16k 两种配置的 `make_synthesis_window` 全部输出。
  - `shift_modes_8k` / `shift_modes_16k`：`shift=0` 叠加与 `shift=1`
    推进，相邻帧换基频、换幅度；每帧检查全部 `2*n_samp` 个有效输出与
    保留尾部。
  - `silence_tail`：零幅度帧把尾部排空，末帧输出为 0。
  - `top_bin`：末个谐波贴近并钳到最高可用频点（`FFT_DEC/2-1`）。
  - `junction`：连续 8 帧，每帧显式检查连接位置 `n_samp-1`。
- **stream**
  - `dual_instance`：两条实例交错，各自的相位/窗口参考历史独立。
  - `voiced_unvoiced`：有声→无声过渡，无声分支在真实解码器上执行。

## 退化检出证明

`tests/check_phase_mutations.sh` 用临时源码副本对同一组测试跑两种仍可
运行的退化：

1. `clock_short`：基频时钟每帧少推进一个样本
   （`ex_phase += Wo*(n_samp-1)`，src/phase.c）。
2. `overlap_head_early`：重叠头部从逆变换里的前一个样本开始取
   （`sw_[FFT_DEC - n_samp + i]`，src/sine.c）。

判定检出的唯一标准是：契约运行器打印第一个 `CONTENT_MISMATCH` 并以
退出码 4 结束。编译失败或进程异常（任何其他退出码）都不算检出。
两种退化都必须被检出，脚本才返回 0。
