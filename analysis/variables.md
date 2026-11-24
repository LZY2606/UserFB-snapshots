# 优化 ELF 的局部变量存储恢复（optimized4 / optimized5）

本报告中的每个结论都由库接口 `VariableLocationAnalyzer` 从 ELF 的调试字节恢复，
逐条对应 `analysis/variables.tsv`。源码只用于人工对照；查询程序不读取 `samples/src`，
也不运行样本或调用外部调试器。两份 ELF 来自同一组 C++ 源文件，分别是 DWARF 4
（`.debug_loc`）和 DWARF 5（`.debug_loclists` + `.debug_addr`），链接地址分别是
`0x401000` 与 `0x501000`，函数体逐字节同构。

表中身份列的含义：

- `cu` 是 CU 头在 `.debug_info` 的节内偏移；`function_die` / `variable_die` 是
  `.debug_info` 节内绝对偏移。这两个偏移就是变量身份，同名变量不会合并。
- `scopes` 按祖先顺序列出函数与有自身范围的词法块，格式 `die@begin..end`，
  范围是半开区间。没有自身范围的词法块透明继承父范围。
- `attribute` 是 `DW_AT_location` 的 `form:value`（form 与整数值都十六进制）；
  exprloc 的 value 记为 `-`，因为字节在 `expression` 列给出。
- `entry` 是被选中的旧地址对或 LLE 的节内偏移，`expression` 是表达式第一个
  操作码的节内偏移，`bytes` 是完整表达式字节。inline 表达式的来源固定为
  `.debug_info`，列表表达式的来源固定为 `.debug_loc` / `.debug_loclists`。

## 1. 两份样本的调试编码差别

| 事实 | optimized4.elf | optimized5.elf |
| --- | --- | --- |
| DWARF 版本 | 4 | 5 |
| 位置列表 section | `.debug_loc` | `.debug_loclists` |
| 变量位置的 form | `DW_FORM_sec_offset`（0x17），值就是列表偏移 | `DW_FORM_loclistx`（0x22），值是偏移表下标 |
| 地址索引 | 无 | `DW_LLE_*` 通过 `.debug_addr` 取地址（`DW_AT_addr_base`） |
| CU 根 `loclists_base` | 无 | alpha CU `0xc`，beta CU `0x4a` |
| 列表条目编码 | 地址对 + `uint16` 表达式长度 | LLE 类型字节 + ULEB128 长度 |
| `entry_value` 操作码 | GNU 扩展 `0xf3` | 标准 `DW_OP_entry_value` `0xa3` |
| 函数 `frame_base` | `DW_OP_reg6` 字节 `56` | 同左 |
| 词法块内 `moving` | exprloc `91 74` | exprloc `91 74` |

两种编码恢复出来的**变量事实**一致：`recover_alpha` 的 `moving` 先在后文所述
寄存器/栈位置间移动，`input` 在函数后半段改用 entry value，`stable` 在
`0x401012..0x401039` 是寄存器；两个函数中同名的 `moving` 是互不相同的 DIE。
差异只在编码层：DWARF 4 的列表偏移是直接 section 偏移且表达式是裸地址对，DWARF 5
的列表索引要先经过 `DW_AT_loclists_base` 指向的 offset 表，区间用相对基地址的
`DW_LLE_offset_pair` 表达。库对两者分别解析，绝不把一种格式套到另一份 section 上。

## 2. 位置改变：`recover_alpha` 的 `moving`（DIE 0x52）

函数 `recover_alpha` 的 DIE 是 `0x2a`，范围 `0x2a@0x401000..0x401042`（`high_pc`
是 data4 长度 66，不是地址）。顶层 `moving` 是 DIE `0x52`，`DW_AT_location` 为
`sec_offset 0x39`（DWARF 4）或 `loclistx 1`（DWARF 5）。列表各条目：

| PC | TSV 行 | 入口 | 表达式来源 | 字节 | 解释 |
| --- | --- | --- | --- | --- | --- |
| 0x401006 | 行 1 | `.debug_loc@0x39` | `.debug_loc@0x4b` | `7670` | `breg:6:-16` |
| 0x401009 | 行 2 | `.debug_loc@0x4d` | `.debug_loc@0x5f` | `50` | `reg:0` |
| 0x401012 | 行 3 | — | — | — | 空洞，`unavailable` |
| 0x40101b | 行 4 | `.debug_loc@0x60` | `.debug_loc@0x72` | `7670` | `breg:6:-16` |
| 0x40102d | 行 5 | `.debug_loc@0x60` | `.debug_loc@0x72` | `7670` | `breg:6:-16` |
| 0x401042 | 行 6 | — | — | — | 函数末尾（半开区间外），`out_of_scope` |

DWARF 4 的实际地址对是 `0x39: 0x401006..0x401009`、`0x4d: 0x401009..0x401012`、
`0x60: 0x401015..0x401042`。`0x401012..0x401015` 没有任何条目，行 3 的
`unavailable` 是列表空洞，不是“变量不存在”；报告按要求不选条目，`entry` /
`expression` / `bytes` 都为 `-`，但保留候选身份与 `attribute`。

DWARF 5 的同一列表在 `.debug_loclists` 的 offset 表中下标 1（列表首 LLE 在
`0x26`）：`0x26: 0x501006..0x501009`（expr@0x2a `7670`）、
`0x2c: 0x501009..0x501012`（expr@0x30 `50`）、
`0x31: 0x501015..0x501042`（expr@0x35 `7670`）。行 17/18/19/20/21/22 与
DWARF 4 的 1–6 行一一对应，正说明“编码不同、事实相同”。

**为什么是 `breg:6:-16`**：`76 70` 是 `DW_OP_breg6` + SLEB128 `0x70`。SLEB128 的
`0x70` 最高位为 0、符号位为 1，解码为 `-16`。这只是“变量在 rbp 所指帧基址减 16
处”，寄存器编号 6 是位置描述的寄存器，不是运行时值。`50` 是 `DW_OP_reg0`，
表示变量就在 0 号寄存器中。

## 3. 词法范围边界：块内 `moving`（DIE 0x7d / 0x57）与同名变量

`recover_alpha` 内有一个 `DW_TAG_lexical_block`（DWARF 4 DIE `0x70`，DWARF 5
DIE `0x51`），范围都是 `0x40101b..0x40102d`。它下面另有一个**同名**
`DW_TAG_variable` `moving`：DWARF 4 DIE `0x7d`，DWARF 5 DIE `0x57`。它与顶层
`moving`（`0x52` / `0x3f`）是两个独立 DIE，查询按 DIE 身份分别返回，绝不按
名字合并：

- 行 4（PC `0x40101b`）：先返回 DIE `0x52` 的顶层 `moving`（`breg:6:-16`），
  再返回 DIE `0x7d` 的块内 `moving`，后者 `scopes` 为
  `0x2a@0x401000..0x401042;0x70@0x40101b..0x40102d`，`status=described`。
  它的位置属性是 **exprloc**（`attribute=0x18:-`），表达式在
  `.debug_info@0x7f`，字节 `9174`：`DW_OP_fbreg` + SLEB128 `0x74`（= -12）。
  `description=fbreg:-12:frame=56`，`frame=56` 是所属函数 `DW_AT_frame_base`
  的原始字节 `0x56`（`DW_OP_reg6`），只作来源说明，不代为求值。
- 行 5（PC `0x40102d`）：块范围是半开的，`0x40102d` 已在块外，块内 `moving`
  变为 `out_of_scope`；顶层 `moving` 仍由列表覆盖，继续 `described`。
- 行 1–3（PC `0x401006`/`0x401009`/`0x401012`）：块内 `moving` 尚未可见，
  `status=out_of_scope`；报告仍列出这条同名候选，并保留其 `attribute=0x18:-`。

这组行同时证明三件事：词法块边界会改变同名变量的可见性；`out_of_scope`
的候选不会被省略；inline 表达式的来源是 `.debug_info` 而不是位置列表。
DWARF 5 的对应行 17/18/19、20、21、22 给出完全相同的身份与可见性结论，
只是 DIE 偏移换成 `0x3f`/`0x57`、`expression` 换成 `.debug_info@0x59`。

## 4. 两个函数里的同名变量

`recover_alpha` 的 `moving`（`0x52`）与 `recover_beta` 的 `moving`（`0x121`）
从不同 CU 返回，DIE 身份不同，位置事实也不同：

| 查询 | 行 | 函数 / 变量 DIE | PC | entry / expression | bytes | description |
| --- | --- | --- | --- | --- | --- | --- |
| alpha `recover_alpha` | 1 | 0x2a / 0x52 | 0x401006 | `.debug_loc@0x39` / `.debug_loc@0x4b` | `7670` | `breg:6:-16` |
| beta `recover_beta` | 10 | 0xf9 / 0x121 | 0x401058 | `.debug_loc@0xe0` / `.debug_loc@0xf2` | `767c` | `breg:6:-4` |

`767c` 是 `DW_OP_breg6` + SLEB128 `0x7c`（= -4），和 alpha 的 -16 不同，因为
两个函数各自的栈帧布局不同。beta 的列表（首项 `0x60` / DWARF 5 下标 1）还给出
一个**零长度**地址对 `0x40105b..0x40105b`：行 11 在 `0x40105b` 是
`unavailable`，说明零长度范围不命中，且列表不会跨空洞合并。beta 的第二个有效
区间从 `0x40105e` 开始（行 12，`.debug_loc@0x107` / `.debug_loclists@0x6b`），
同样恢复为 `breg:6:-4`。

## 5. 寄存器与不可解释表达式

- **寄存器**：`stable`（DIE `0x61` / `0x48`）在 `0x401012` 起是列表区间
  `0x401012..0x401039`，行 9 / 25 选中的表达式是 `53` = `DW_OP_reg3`，报告
  `reg:3`。这只说明变量驻留在 3 号寄存器，不声称该寄存器此刻的值。
- **不可解释（opaque）**：`input` 在 `0x40101e` 的表达式是
  `f301559f`（DWARF 4）或 `a301559f`（DWARF 5）。两者都是 entry value：
  `DW_OP_entry_value`/GNU 扩展 + 长度 1 + `DW_OP_reg5`（rdi）+
  `DW_OP_stack_value`。它描述“入参在入口时的值”，需要调用时的上下文才能求值，
  因此报告 `opaque` 并保留完整十六进制字节。**opaque 仍是 `described`**，
  与 `unavailable` 不同：前者有明确表达式，后者根本没有可用的位置描述。
  `input` 在 `0x40102d`（行 8 / 24）落在两个区间之间的空洞，才是 `unavailable`。
- **fbreg**：块内 `moving` 的 `9174` 只给出“帧基址 -12”，不替调用方计算地址；
  `frame=56` 只是所属函数 `DW_AT_frame_base` 的原始字节。
- **算术 / piece / stack_value 等**：本样本未出现单独的这类表达式；一旦出现，
  分析层保留完整字节并标 `opaque`，不会退化成空成功。

## 6. 同名缺失与查询顺序

行 14/30 查询 `recover_alpha` 的 `missing`，行 15/31 查询函数 `missing`。二者
都只返回一条 `not_found`，`cu`/`function_die`/`variable_die`/`scopes`/
`attribute`/`entry`/`expression`/`bytes` 全部为 `-`，不会沿用上一条查询的身份。
行 16/32 把查询 2/18 原样重复一遍，结果逐字节相同，说明查询无缓存、顺序无关。

## 7. 不支持的构造

样本里没有 `DW_AT_ranges`、split DWARF、内联子程序，也没有
`abstract_origin`/`specification` 继承，因此这些路径没有伪造结果。分析层在
查询路径上遇到这些结构会返回带 section/偏移的诊断并以非零状态退出，而不是给出
空成功：`DW_AT_ranges`、split/skeleton unit、`DW_TAG_inlined_subroutine`、
`abstract_origin`/`specification`，以及无法识别的 `DW_AT_location` form 或
`.debug_loclists` LLE 都在拒绝之列。缺少 `.debug_loc` / `.debug_loclists` /
`.debug_addr` 等必需 section 同样诊断。

## 8. 与 TSV 的对应关系

上面引用的行号即 `analysis/variables.tsv` 的 `query` 列（从 1 计数）。该文件由
`tools/variable_locations` 逐条读取 `samples/queries.tsv` 生成；行内表达式字节
的 section 偏移可在对应 ELF 中直接核对。`support/report_check.cc` 另外校验了
`bytes` 与 ELF section 原始字节、`entry`/`expression` 来源一致性，以及所有状态与
身份字段的格式。
