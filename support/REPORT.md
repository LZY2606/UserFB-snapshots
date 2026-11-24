# 分段归属报告协议

新增工具 `tools/segmented_lookup.rs` 用两个位置参数：样本目录、查询 TSV 文件。例如编译后运行 `lookup fixtures/banked-fw fixtures/banked-fw/queries.tsv`。stdout 仅为 UTF-8 TSV，诊断走 stderr。正常查询（包括没有归属）退出0；无法读取文件、不支持的表达式等输入问题退出非零。

首行固定如下，字段之间是一个制表符：

```text
query selector offset cu cu_name function die segment_die file line aranges tuple line_program sequence row bank bank_offset
```

这里用空格展示字段名，实际输出用 TAB。数值允许十进制或 `0x` 十六进制，无值统一用 `-`，不能留空。

| 字段 | 含义 |
|---|---|
| query | 查询输入的0基序号；重复查询也各自输出 |
| selector / offset | 输入的二元地址，彼此独立 |
| cu / cu_name | 命中的 CU header 在 debug_info 的绝对 section 偏移、CU 名 |
| function / die | 命中的子程序名字、DIE 在 debug_info 的绝对 section 偏移 |
| segment_die | 提供该函数有效 DW_AT_segment 的 DIE 的绝对 section 偏移；是函数自身或最近祖先。平坦且无段属性时为 `-` |
| file / line | 源文件目录和文件名组合的相对路径、行号；没有有效非终止行则都为 `-` |
| aranges / tuple | 命中的 aranges contribution 起点及该元组起点，都是 debug_aranges 内绝对字节偏移 |
| line_program / sequence / row | line contribution 起点的 section 字节偏移、该 contribution 中0基 sequence 序号及该 sequence 中0基 row 序号；这个三元组可回到原始行程序独立重放 |
| bank / bank_offset | 原始 bank 文件名、字节偏移，由 banks.tsv 的 selector 和半开文件范围映射；即使没有调试归属仍可存在 |

匹配用半开区间。先按 selector+offset 取得 aranges 的全部 CU 候选，然后在各 CU 里找有效段身份与范围同时命中的全部 subprogram。不合并同名函数。每个函数候选关联同 CU、同 selector 的 line sequence：若多条合法 sequence 命中，全部保留；sequence 内选择覆盖查询 offset 的非 end_sequence 行，区间右端是下一行的地址。没有函数仍可给出已知 CU 的行位置；没有源行时不能沿用上一个 sequence 的末行。

每个 query 至少一行：没有 CU 时，cu 到 row 全部为 `-`；有 CU 但没有函数时 function、die、segment_die 为 `-`，其余已知证据保留。排序先 query，再 CU、DIE、sequence、row 的数值顺序，无值排后；重复相同的 CU/函数/sequence 身份不输出两遍。bank 映射不作为调试范围存在的证据。

输入范围：样本说明中的未压缩四节、单层 CU/subprogram DIE、直接 string、low_pc/address 与 high_pc/data4、stmt_list/sec_offset。段属性为单条 DW_OP_constu 或空表达式；空表达式显式选择平坦空间，无属性向父节点继承，直到 CU。其他表达式明确报不支持，不把它算成0；不做寄存器、内存、内联、split DWARF、重定位或通用表达式求值。

为检验输入驱动行为，新 Rust 测试应构造另一组合法小样本，改变 selector、函数名、源行、contribution顺序和offset；不能只反复读取固定查询或比较两次生产输出。预期值独立写明，测试不得从待测分析器反推。
