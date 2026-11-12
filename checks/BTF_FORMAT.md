# BTF 输入格式范围

本页是随材料提供的字段速查，算法及压缩规则见用户要求。数据只在本地作为 ELF 元数据使用。

依据 Linux v6.12 官方文档：https://github.com/torvalds/linux/blob/v6.12/Documentation/bpf/btf.rst
CO-RE 布局依据：https://github.com/torvalds/linux/blob/v6.12/Documentation/bpf/llvm_reloc.rst

所有多字节整数使用所属 ELF 的端序，字段宽度不随 ELF32/64 改变。
BTF 头最短 24 字节：u16 magic=0xeb9f、u8 version=1、u8 flags=0，接着五个 u32：hdr_len、type_off、type_len、str_off、str_len。off 相对 hdr_len 结束位置。更长头的未知字节保留。字符串区首字节为零，被使用的每个偏移必须能在区内找到结束零。

每条类型共同前缀是三个 u32：name_off、info、size_or_type。info 的低16位是 vlen，第24至28位是 kind，最高位是 kind_flag，其他位保留为零。紧随前缀的载荷如下。

| kind | 共同第三字段 | 后续载荷 |
|---|---|---|
| 1 INT | 字节大小 | 一个 u32 整数编码，24至27位为编码，16至23位是位偏移，低8位是位数 |
| 2 PTR | 引用的类型ID | 无 |
| 3 ARRAY | 0 | 三个 u32：元素类型ID、索引类型ID、元素数 |
| 4 STRUCT、5 UNION | 字节大小 | vlen 组三个 u32：成员名偏移、类型ID、成员位布局 |
| 6 ENUM | 大小1/2/4/8 | vlen 组 u32 名偏移及 s32 值 |
| 7 FWD | 0 | 无 |
| 8 TYPEDEF、9 VOLATILE、10 CONST、11 RESTRICT | 类型ID | 无 |
| 12 FUNC | FUNC_PROTO类型ID | 无；vlen为linkage 0/1/2 |
| 13 FUNC_PROTO | 返回类型ID | vlen 组两个u32：参数名偏移、参数类型ID；末项0/0表示可变参数 |

除 STRUCT/UNION/ENUM/FWD 外 kind_flag 为零。STRUCT/UNION 设置 kind_flag 时成员位布局的高8位为位域宽，低24位为位偏移，否则整个字段是位偏移。FWD 的 kind_flag 区分 struct/union，ENUM 区分无符号/有符号。INT、PTR、ARRAY、FWD、TYPEDEF、限定符的 vlen 均为零。PTR、ARRAY、限定符、FUNC_PROTO 的名字偏移为零。INT 编码只允许 signed=1、char=2、bool=4，编码的其他保留位为零；位偏移加位数不得越出声明大小。这里检查二进制结构与引用边界，不实现完整的内核类型验证。

BTF.ext 头前8字节与 BTF 同形。接着是 func_info_off/len、line_info_off/len，hdr_len>=32 时再有 core_relo_off/len；这些 off 都相对头结束。hdr_len 为24或至少32，其余中间长度拒绝。各非空区域开头是u32 rec_size，然后重复 u32 sec_name_off、u32 num_info（必须正数）、num_info 条记录。共同前缀之外的记录尾部保留。

| 区域 | 最短rec_size | 前缀中依次排列的u32 |
|---|---|---|
| func_info | 8 | insn_off、type_id |
| line_info | 16 | insn_off、file_name_off、line_off、line_col |
| core_relo | 16 | insn_off、type_id、access_str_off、kind |

这些字符串偏移均指向同一BTF字符串区。ELF格式的insn_off以字节为单位。本任务不验证指令内容或CO-RE表达式，也不解析内核对象、外部base或其他kind。检查范围加法、计数乘法和变长记录之前不要用未验证的大小分配内存。
