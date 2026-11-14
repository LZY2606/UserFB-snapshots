# SILK NLSF 参数解码契约回归

`tests/nlsf_contract.c` 为 Opus 中 SILK 的 NLSF（归一化线谱频率）参数解码链
建立一组可执行的契约检查。它不改动任何现有编解码行为或公开接口；唯一的
生产侧改动是在 `silk/decode_parameters.c` 里加了三个只读观察点，仅在
`-DTASK_NLSF_TEST` 构建下编译，默认回调为 `NULL`，普通构建的 ABI 与内存
分配完全不变。

构建与运行：

```sh
bash task_support/run.sh baseline   # 原有多流 + padding 基线
bash task_support/run.sh target     # 基线 + 本回归（vectors/history/packets）
bash task_support/mutants.sh        # 变异体检测
```

## 观察点（silk/task_nlsf_test.h）

`silk_task_nlsf_hook` 是一个全局回调指针（默认 `NULL`），在
`silk_decode_parameters()` 内三个位置被只读调用：

| 事件 | 时机 | 可观察内容 |
| --- | --- | --- |
| `TASK_NLSF_EVENT_NLSF` | 当前帧 NLSF 解码完成、插值系数已按 reset 状态钳制、插值与历史更新之前 | 码书索引、上一帧已提交的 `prevNLSF_Q15`、当前 NLSF、`lossCnt`、`first_frame_after_reset` |
| `TASK_NLSF_EVENT_LPC` | 插值与 NLSF2A 完成、`prevNLSF_Q15` 已更新、丢包后带宽扩展（BWE）之前 | 两组 `PredCoef_Q12`（BWE 前）、保存给下一帧的线谱 |
| `TASK_NLSF_EVENT_LPC_FINAL` | BWE 块之后（无丢包时与前一组相同） | 两组最终 `PredCoef_Q12` |

指针只在回调期间有效，测试不得写入。真实帧解码与受控调用
（直接调用 `silk_decode_parameters()`）走同一组观察点。

## vectors 组：码书路径的独立宽整数模型

独立模型按格式重新定义，不调用 `silk_NLSF_unpack`/`silk_NLSF_decode`，
不用生产定点宏，也不复制其反量化展开实现；码书表、delta-min 表作为
格式常量直接引用：

1. **预测器选择**：`ec_sel` 的每个打包字节覆盖两维——bit 0 选偶数维、
   bit 4 选奇数维的预测器行，`pred_Q8[dim + row*(order-1)]`。
2. **残差反量化**：从最后一维反向展开。预测值
   `pred = (int16)acc * pred_Q8 >> 8`；残差 `idx*1024`（Q10），非零残差
   向远离零方向偏移 102（Q10 的 0.1）；累加
   `acc = pred + (cur * step_Q16 >> 16)`。
3. **重建**：`res_Q10*16384 / wght_Q9`（向零截断除法）加回一级中心
   `CB1<<7`，钳制到 `[0, 32767]`。

模型中三类运算各自明确定义：负数右移为向下取整（算术）移位；乘法缩放
先取 64 位乘积再移位；除法为 C99 向零截断。

- `order10_exact` / `order16_exact`：扫描全部一级条目 × 12 种残差模式，
  只保留模型输出本身已满足全部 delta-min 间距（含两端边界）的路径，
  与生产 `silk_NLSF_decode` 逐项**精确**比较 Q15；输出报告实际比较的
  路径数与最小间距余量（如 `min_margin=44`）。
- `order10_stabilized` / `order16_stabilized`：取合法但模型输出违反间距
  的索引路径（未稳定化的向量不能作为最终答案），检查生产解码输出的
  边界间距恢复、值域有限（`[0,32767]`）、再次稳定化幂等。

## history 组：插值、LPC 参照与历史纪律

受控参数入口：手工构造 `silk_decoder_state`（码书、阶数、索引、已提交
历史、reset/loss 标志），直接调用 `silk_decode_parameters()`，经观察点
记录前后两组 LPC 与保存值。

- **插值模型**：`nlsf0 = prev + ((coef*(cur-prev)) >> 2)`（向下取整移位），
  由前一帧已提交线谱与当前线谱自行计算，不读生产的插值结果。
- **LPC 参照**：余弦用量化余弦表分段线性插值；偶数根、奇数根分别直接
  相乘出 `p(z)=∏(1-c z⁻¹+z⁻²)`、`q(z)`，再由
  `A(z)=((1+z⁻¹)p(z)+(1-z⁻¹)q(z))/2` 得到首一多项式，取负即为
  白化系数（双精度计算，不复制生产的排列与定点递推）。良态向量
  （`|a| < 7.9`，证明未触发 LPC 再拟合）下每个系数绝对误差 ≤ 8 个
  Q12 单位，实际最大误差打印在 `INFO` 行（目前为 1）。
- 两种阶数（10/16）都覆盖非零插值（coef 2 与 3）。
- **reset 纪律**：`first_frame_after_reset=1` 时插值系数被钳为 4，两组
  LPC 相同（只用当前向量）；下一帧恢复用已提交历史插值。
- **丢包纪律**：`lossCnt>0` 时两组 LPC 都精确等于 BWE 模型
  （chirp 63570/65536 逐次舍入）作用于 BWE 前系数的结果；扩展后的系数
  不得存入线谱历史——保存给下一帧的 `prevNLSF_Q15` 必须逐项等于当前
  NLSF。

## packets 组：真实码流端到端核对

公开 encoder（`OPUS_APPLICATION_RESTRICTED_SILK` + 强制 NB/WB 带宽）
对连续低幅类语音信号（慢颤音谐波簇，峰值约 1200）各编码 16 个 20 ms
包，分别真正经过窄带（内部 8 kHz、10 阶）与宽带（16 kHz、16 阶）SILK
解码。第 7 包丢弃触发一次 PLC 后恢复，第 12 包前 `OPUS_RESET_STATE`
触发一次 reset 后重新预热。每个真实解码帧都按 vectors/history 两组的
独立模型核对：码书索引→NLSF（精确）、历史链（上一帧提交值逐项等于
本帧观察到的历史）、插值→两组 LPC（≤8 Q12）、丢包帧 BWE（精确）、
保存值。同一个库解码两次不作为参照。每个包核对 PCM 样本数（960）、
全部有限、能量非零；至少一个非首帧确实启用插值（`interp_seen`）。

出现首个差异时按 `MISMATCH <项> packet=<包> order=<阶数> dim=<维度>
expected=<期望> actual=<实际>` 报告。

## 变异体

`task_support/mutants.sh` 在隔离副本中注入两处退化并期望被内容比较拒绝
（退出码 4，非构建失败/崩溃/零断言）：

- `residual_bias`：去掉非零残差的 ±0.1 零点偏置 → vectors 组精确比较
  拒绝（`check_exact`）。
- `early_history`：插值前把旧线谱过早覆盖成当前线谱 → history 组受控
  插值检查拒绝（`check_close_q12("lpc0", ...)`）。
