工具调用：variable_locations samples/queries.tsv。工作目录为源码根；参数文件每行是以制表符分隔的 sample、function、variable、pc，没有表头，pc 是链接后的十六进制地址。逐行处理，保留查询顺序，同一查询内按变量的 .debug_info DIE offset 升序。报告不可读文件/不支持构造时非零退出，stderr 带 section 和偏移；查不到名字用 not_found 正常结果。

输出首行必须是下面的 TSV 表头，之后每个候选变量一行，不能省略 out_of_scope 的同名候选。function 名称只匹配有实体范围的 DW_TAG_subprogram，变量只取该函数下的 DW_TAG_variable/DW_TAG_formal_parameter，忽略 call-site 描述；同名的多个函数实体也分别保留。不跟随 abstract_origin/specification。没有匹配函数/变量时仅一条 not_found。

```
query	sample	pc	cu	function_die	variable_die	scopes	status	description	attribute	entry	expression	bytes
```

以上用\t表示分隔；实际输出应使用TAB。query 从1计数；sample 和pc对应输入；cu为CU头在.debug_info的节内偏移，function_die和variable_die为绝对.debug_info节内偏移；所有偏移/地址带0x。scopes按祖先顺序列出函数及有范围的词法块，写die@begin..end，用分号分隔，不输出抽象引用或call-site；无自身范围的词法块透明。

status为described、unavailable、out_of_scope、not_found。先按scope判定，再选位置，out_of_scope不泄露恰好覆盖该PC但不属于活动词法范围的位置。description为reg:N、breg:N:D、fbreg:D:frame=HEX或opaque（N为十进制寄存器编号，D为有符号十进制偏移，非负时不加+）；其余状态用-。frame是所属函数的原始frame_base字节，仅解释来源，不求值；需要它但缺失或不支持时给诊断。

attribute保留DW_AT_location的数值form:value，form十六进制；整数值带0x，block/exprloc的value用-。无属性用-。entry为.debug_loc@0x偏移或.debug_loclists@0x偏移，指被选择的LLE或旧地址对起点；inline用-。expression为section@0x偏移，指第一个操作码字节。bytes是小写连续十六进制完整表达式。unavailable且命中显式空表达式时保留entry/expression，bytes用-；位置空洞、out_of_scope或not_found不选条目，entry/expression/bytes均用-。not_found的cu/function_die/variable_die/scopes/attribute均用-；其他状态保留候选身份、scope和属性，不能沿用上一查询的结果。

结果里的表达式以及frame_base拥有存储，source offset是section相对偏移。samples/queries.tsv只提供输入，不附输出或预计算映射。support/report_check.cc检查报告覆盖、状态与来源字节的一致性；具体语义需要库测试及analysis/variables.md的独立核对。不要从报告中重建位置列表作为查询缓存。
