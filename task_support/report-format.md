示例参数是一个或多个样本路径，按传入顺序处理；报告写 stdout，错误说明写 stderr，损坏输入返回非零。输出 UTF-8、LF、TAB 分列，无标题行。除 sample 使用输入路径的 basename 外，所有文本都按下列编码避免转义歧义。整数以小写 0x 十六进制表示，序号使用十进制；hex 为小写连续十六进制，无字节用 -。原始路径只输出 NUL 之前的字节的 hex。诊断不是成功报告。

每个目录项先输出一行 R，紧接其零到两个候选 C，最后输出所有身份组成员 G。各行的字段为：

```
R<TAB>sample<TAB>ordinal<TAB>entry_file_offset<TAB>directory_section_index<TAB>entry_28_bytes_hex<TAB>relation
C<TAB>sample<TAB>ordinal<TAB>source<TAB>payload_file_offset<TAB>owner<TAB>payload_hex<TAB>format<TAB>identity_key<TAB>path_hex
G<TAB>identity_key<TAB>sample<TAB>ordinal
```

section_index 是原节表的零基编号。source 为 file 或 rva；两者都有时 file 在前，即使同址也各输出一行，以便追踪两个头字段。owner 为 section:N、overlay、headers 或 gap。候选范围必须整体属于一种归属，不支持跨归属读取。relation 为 none、file_only、rva_only、same、equal_distinct、conflict，分别对应零长、仅文件指针、仅RVA、同址、异址同字节、异字节。零长项不输出 C。

format 为 RSDS、NB10 或 opaque。RSDS identity_key 为 RSDS:小写GUID:八位小写age十六进制，GUID采用8-4-4-4-12字段顺序，不加花括号；NB10 为 NB10:八位offset:八位signature:八位age，后三段均为小写十六进制。不透明候选的 identity_key、path_hex 为 -。这些展示键不能替代库中保留的原始字段。

G 只包含已识别且无来源冲突的记录；同址或异址同字节的一条记录只有一个 G。按 identity_key 字节序，再按输入文件次序、目录序号排序，每个成员一行，重复记录仍分别列出。冲突候选只出现在 C，不进入 G。输入文件 basename 必须非空且互不重复，重复时明确返回非零，避免成员身份混淆。

task_support/report_check.cpp 只检查行结构、输入归属、记录与候选配对，以及报告里的原始字节在原文件中确实存在，不解析 PE、不提供身份和布局答案。新回归需要独立验证实际来源、身份、关系和归组；这个结构检查不能替代内容断言。
