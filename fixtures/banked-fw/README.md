# 合成 bank 固件调试输入

这是手工编码的小型只读固件/调试样本，不是某款硬件编译器的输出。MIT OR Apache-2.0，许可随源码根保留。两个 bank 的合成四字节指令格式为 `[0x10, bank-tag, immediate, 0xff]`，其余字节是填充；这些字节只用来定位文件，不执行，也不要求反汇编。

`banks.tsv` 只描述 selector 对应的原始文件、段内基址和长度，文件偏移为 offset-base。`queries.tsv` 是无答案的查询列表。`firmware/` 是调试文件表所引用的源码文本，只作内容参照。

`debug_abbrev`、`debug_info`、`debug_line`、`debug_aranges` 分别是同名 DWARF section 的原始字节，不带 ELF 容器。所有字节按小端编码。info 为 DWARF5/DWARF32，地址宽度4，函数 high_pc 为长度常量；segment 是 exprloc，限定为一个 DW_OP_constu。子 DIE 没有 segment 属性时继承最近父节点。line 为 DWARF5，两个 contribution 分别使用1和2字节 selector；每个 sequence 内 selector 不变。aranges 为 version 2，先 DWARF32/1字节 selector，再 DWARF64/2字节 selector，地址宽度均为4。每个 contribution 单独对齐元组，不能按 section 起点对齐。

需要从实际调试字节恢复函数和行归属，不能把 bank 清单当符号表。原始样本不含查询预期表、生成器或分析结果。工具读取新目录/新查询时也应工作，不依赖这些文件的固定长度、CU顺序、名字或查询顺序。

参考规范：[DWARF5](https://dwarfstd.org/doc/DWARF5.pdf)，2.12 的段属性继承、6.2 的行程序及7.21的地址范围表。本样本把非零 selector width 的 set_address 操作数明确约定为 selector 在前、offset 在后。
