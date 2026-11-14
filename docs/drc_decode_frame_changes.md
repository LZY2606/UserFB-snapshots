# AAC 动态范围（DRC）路径修复与 `drc_decode_frame` 契约

本次变更把 AAC 动态范围处理这条路径修完整：统一浮点与定点构建的增益语义，
按实际帧长（960/1024）应用增益，并让连续的 `EXT_DYNAMIC_RANGE` 元数据按
完整的新描述解释。公开解码 ABI（`neaacdec.h`）不变。

## 问题

1. **定点与浮点行为不一致**：定点分支完全忽略实例控制值 `ctrl1/ctrl2`
   （cut/boost）。关掉 boost（`ctrl2 = 0`）后定点路径仍然提升响度；带小数
   的控制值（如 0.5）在定点下被异常放大。
2. **帧长硬编码为 1024**：旧入口 `drc_decode()` 总是处理 1024 条谱线，
   960 线帧中第 960 线之后的保护区也会被改动。
3. **元数据继承错误**：`dynamic_range_info()` 解析新 payload 时，省略的
   参考响度、分带和排除信息会继承上一帧的内容，而不是回到默认值。

## 接口变化（`libfaad/drc.h`）

```c
int drc_decode_frame(drc_info *drc, real_t *spec, uint16_t frame_len);
```

- 成功返回 0；非法参数（空指针、非 960/1024 的帧长、非法频带）返回非零，
  且频谱保持不变。
- 旧入口 `void drc_decode(drc_info *drc, real_t *spec)` 保留，作为 1024 线
  的兼容包装。
- `specrec.c` 中单声道（SCE）与双声道（CPE）重建都改走带长度的入口，
  传入 `hDecoder->frameLength`。FIL 与音频元素的处理顺序不变。

## 频带规则

- 单带覆盖整帧（忽略 `band_top[0]`）。
- 多带端点仍为 `4*(band_top+1)`，与帧长取较小值，按连续的半开区间
  `[bottom, top)` 应用；最后一个端点之后的谱线不改动。
- 原始端点必须严格递增，否则返回非零且频谱不动——不允许用帧长截断来
  掩盖倒序。
- 排除的声道（`exclude_mask`）照旧在 `specrec.c` 中跳过。

## 增益公式（两种构建一致）

- 压缩：指数 `(-cut*ctl - (80 - ref)) / 24`
- 提升：指数 `(boost*ctl - (80 - ref)) / 24`
- 乘数为 2 的该指数次方。`cut`/`boost` 允许 0..1 的分数；参考响度修正
  `(80 - ref)` 不随控制值缩放。

### 定点实现

- 频谱为 Q14（`REAL_BITS = 14`）。增益分解为整数八度 `n` 与小数部分
  `g`（Q28）：指数先以 Q14 整数域精确计算，再按 `24*16384` 拆分；
  小数部分用 257 项 `2^(k/256)` 表（COEF/Q28 刻度）加线性插值，不混用
  系数刻度，也不把整条频谱转成浮点处理。
- 逐线用 64 位整数完成 `spec * g * 2^n`，对称舍入（负值按远离零方向
  正确舍入），超出 int32 范围时饱和。
- 精度：有限且不饱和的定点结果相对双精度公式误差不超过 2 个 Q14 整数
  单位；浮点分支沿用 `pow(2.0, exp)`，相对误差不超过 2e-5。

## 元数据（`libfaad/syntax.c`）

`dynamic_range_info()` 每解析一个新的 `EXT_DYNAMIC_RANGE` payload 时先恢复
默认值，再按位流覆盖：

- 省略的参考响度恢复 80（`DRC_REF_LEVEL`）；
- 省略的分带恢复整帧单带；
- 省略的排除信息清空（`excluded_chns_present = 0`，掩码清零）；
- `cut`/`boost`（`ctrl1`/`ctrl2`）作为实例控制值保留，不受 payload 影响。

`drc_init()` 使用同样的默认值（`prog_ref_level = DRC_REF_LEVEL` 等）。
没有新扩展的帧沿用已经生效的信息（`present` 与各字段保持不变）。

## 测试（`tests/drc_contract.c`）

测试在 `bash task_support/run.sh target` 下对浮点和定点两种构建都实际执行，
分三组（素材由测试内的 C 代码现场生成）：

- **gain**：用独立的双精度公式核对压缩、提升、分数控制、参考响度、
  多带区间、960 保护区、非法输入以及定点饱和/负值舍入。
- **metadata**：通过真实的位读取路径（`raw_data_block()` → `fill_element()`
  → `dynamic_range_info()`）送入前后不同的扩展，验证默认值恢复、无继承、
  控制值保留以及无新扩展时的沿用。
- **stream**：在 `NeAACDecInit2`/`NeAACDecDecode` 上连续解码生成的
  AAC-LC 流（非零谱），覆盖 960/1024、单声道、左右不同的双声道、排除
  其中一侧，以及无 DRC 的对照。

测试宏 `DRC_CONTRACT_TEST` 下，`specrec.c` 在合成（filter bank）之前调用
只读观察点 `drc_contract_observe_spec()`（由测试实现）；普通构建不包含
该符号，也不新增任何接口。
