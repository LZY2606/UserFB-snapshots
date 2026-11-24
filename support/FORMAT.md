这是报告的交换格式；UTF-8、LF、tab 分隔，无标题行。十进制或带 0x 的十六进制均可。字符串中空白、%、tab、换行按 UTF-8 字节写 %HH；未知值统一写 `-`。名字/路径不得含未转义空白。PC 是指令地址，不减一。库的结构化证据接口由实现者设计，下面只是 sample_report 的序列化协议。

|记录|顺序字段（记录名之后）|
|---|---|
|S|section_name elf_file_offset size|
|Q|query_index pc frame_count|
|F|query_index depth concrete_cu concrete_die low_pc high_pc name decl_path decl_line decl_column call_path call_line call_column|
|A|query_index depth key provider_cu provider_die attr_offset attr_length form raw_value|
|R|query_index depth key hop reference_attribute from_cu from_die attr_offset attr_length form to_cu to_die|
|T|query_index depth key provider_cu stmt_list file_index file_entry_offset file_entry_length directory_entry_offset directory_entry_length compdir_attr_offset compdir_attr_length resolved_path|
|U|query_index depth key section_name string_offset string_length decoded_string|

每份报告先写 S，至少包含 .debug_info、.debug_abbrev、.debug_line。所有 CU、DIE、A/R 属性区间和 compdir 区间都是 .debug_info 的 section offset；stmt_list、文件表及目录表区间都是 .debug_line 的 section offset。换成 ELF 文件偏移的公式是 S 中的 section 起点加 section offset。区间都表示 `[offset, offset+length)`，长度不含 abbrev code，必须覆盖属性操作数的完整编码；string 长度包含结尾 NUL。form 使用 DW_FORM 的数值。raw_value 为原编码解释出的值；string 用上面的 %HH 编码。

query_index 从0开始，按 pcs 文本逐行排序，重复PC也保留。无命中写 Q 且 frame_count=0，不写假帧。每个Q后立即写本次全部记录。F 按最内层到物理函数，depth为物理0、直接inline1、依此增加；low/high是该帧命中的实际地址区间，不是原 high_pc 操作数。声明和调用坐标相互独立，物理函数调用位置未知。F、A、R、T 的 q/depth 必须对应。

每个已知的 name、decl_file、decl_line、decl_column、low_pc、high_pc、call_file、call_line、call_column 都有 A；原始属性不存在则没有 A。名字沿库原有选择规则解析，但其 A/R 仍应指向实际提供名字的路径。low/high 和调用属性只来自具体实例。对于 name/decl_* 的继承，R 逐跳写实际选择的引用路径，hop从0递增；reference_attribute为abstract_origin或specification，A给出终点。R 的 form 为 ref4 时 to_die=所属CU起点+操作数，为 ref_addr 时 to_die=操作数。to_cu必须是该 DIE 真实所属CU。属性值为0的A不能丢弃。

decl_file 和 call_file 分别有 T，key 为 decl_file/call_file。T 的CU是对应A的provider_cu。file_entry区间从文件名首字节覆盖到目录索引、时间及大小三项ULEB结尾。directory_entry区间包含目录字符串及NUL；目录索引0时两个目录字段都写 `-`，路径以该CU的comp_dir为基准。compdir字段指向CU根的DW_AT_comp_dir完整属性操作数。T的file_index是原始值，不能用解析后路径推断身份。无define_file。

字符串有inline string和strp两种。A的strp raw_value为.debug_str偏移，另用U给出字符串原字节区间和解码值；inline string的A raw_value为字符串，也用U定位同一.debug_info字节。name与两个T使用的comp_dir各有U，key分别为name、decl_comp_dir、call_comp_dir。长度包括NUL。T无对应文件属性时没有T及其U。目录索引非零时，目录字符串先按line header规则处理，相对目录再拼comp_dir。

样本限定 DWARF32 version4、address_size=8、little-endian。来源链所用 forms 为 addr、data1、data4、string、strp、sec_offset、ref4、ref_addr、flag_present；high_pc为data4相对长度。编译器样本中的变量等无关DIE还有exprloc等上游本已支持的形式，按生产逻辑跳过，不做位置求值。引用图无环，所选具体函数无范围歧义，源文件表采用DWARF4从1开始的索引。无关 declaration DIE 不产生物理帧。目标实现只需此范围；保留上游对其他已有数据的行为。

support 的报告驱动只检查结构完整性、查询次序、基本字节区间和重查一致性，不是来源正确性的证明。evidence_test.c 和 analysis.md 必须独立比较具体结果、引用操作数、属性及文件表原字节，不以结构检查通过替代分析。
