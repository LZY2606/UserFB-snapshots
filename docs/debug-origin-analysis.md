# 调试来源分析：samples/cedar32.pe 与 samples/cedar64.pe

本文从两份样本的原始字节出发，说明调试目录在哪里、每条记录的 payload 从
哪里取得、RVA 怎样经节表换算成文件偏移、哪些字节落在 overlay，以及哪些
记录可以归为同一调试身份。文中所有位置都是文件偏移（十六进制），拿十六
进制查看器按所给位置即可逐条核对。分析只读字节，不执行、不装载 PE，也
不访问任何外部 PDB。

两份样本各 0xA00（2560）字节，节布局相同，只有可选头宽度（PE32 / PE32+）
和调试目录内容不同。

## 1. 头部与节表

两个文件的头部骨架一致：

| 位置 | 内容 |
|---|---|
| 0x00 | DOS 头，`4d 5a`（"MZ"） |
| 0x3C | e_lfanew = `80 00 00 00` → PE 签名在 0x80 |
| 0x80 | `50 45 00 00`（"PE\0\0"） |
| 0x84 | COFF 头：Machine（cedar32 为 `4c 01` = I386；cedar64 为 `64 86` = AMD64），NumberOfSections = 2 |
| 0x94 | SizeOfOptionalHeader：cedar32 为 0xE0，cedar64 为 0xF0 |
| 0x98 | 可选头起点。Magic：cedar32 为 `0b 01`（PE32），cedar64 为 `0b 02`（PE32+） |

可选头中与本文相关的字段（相对可选头起点的偏移对两种位宽相同）：

| 字段 | cedar32 | cedar64 |
|---|---|---|
| ImageBase | 0x400000（文件 0xB4，4 字节） | 0x140000000（文件 0xB0，8 字节） |
| SizeOfImage | 0x4000（文件 0xD0） | 同左 |
| SizeOfHeaders | 0x200（文件 0xD4） | 同左 |
| NumberOfRvaAndSizes | 16（文件 0xF4） | 16（文件 0x104） |
| 数据目录起点 | 0xF8 | 0x108 |
| 调试目录（第 6 项，每项 8 字节） | 文件 0x128：RVA `40 10 00 00` = 0x1040，Size `c4 00 00 00` = 0xC4 | 文件 0x138：RVA `60 10 00 00` = 0x1060，Size `70 00 00 00` = 0x70 |

节表起点 = 可选头起点 + SizeOfOptionalHeader：cedar32 在 0x178，cedar64
在 0x188。两个文件的节表内容相同（ cedar64 只是整体后移 0x10）：

| 节 | 表项位置（32/64） | VirtualSize | VirtualAddress | SizeOfRawData | PointerToRawData |
|---|---|---|---|---|---|
| [0] `.rdata` | 0x178 / 0x188 | 0x400 | 0x1000 | 0x400 | 0x200 |
| [1] `.cv` | 0x1A0 / 0x1B0 | 0x200 | 0x3000 | 0x200 | 0x600 |

由此得到文件的区域划分（两份样本相同）：

- **headers**：`[0x0, 0x200)`，到 SizeOfHeaders 为止；
- **section:0**：原始字节 `[0x200, 0x600)`，对应 RVA `[0x1000, 0x1400)`；
- **section:1**：原始字节 `[0x600, 0x800)`，对应 RVA `[0x3000, 0x3200)`；
- **overlay**：`[0x800, 0xA00)`。overlay 起点 = max(SizeOfHeaders,
  各非空节原始范围末端) = max(0x200, 0x600, 0x800) = 0x800。节间若有空洞
  属于 gap 而不是 overlay；这两份样本节首尾相接，没有 gap。

RVA → 文件偏移的换算只对被节覆盖的 RVA 有意义：
`文件偏移 = PointerToRawData + (RVA − VirtualAddress)`，且换算出的范围必
须整段落在该节的 SizeOfRawData 内，不允许跨节拼接。ImageBase 不参与这个
换算——目录项里的 AddressOfRawData 本身就是 RVA，不是 VA。

## 2. 调试目录的位置

调试数据目录给出的是 RVA，要经节表换算：

- **cedar32**：目录 RVA 0x1040 落在节 [0]（RVA 0x1000 起）→ 文件偏移
  0x200 + (0x1040 − 0x1000) = **0x240**。Size 0xC4 = 196 = 7 × 28，即
  **7 条记录**，占 `[0x240, 0x304)`，目录归属节 section:0。
- **cedar64**：目录 RVA 0x1060 → 0x200 + 0x60 = **0x260**。Size 0x70 =
  112 = 4 × 28，即 **4 条记录**，占 `[0x260, 0x2D0)`，目录归属节
  section:0。

每条记录是 28 字节的 IMAGE_DEBUG_DIRECTORY：Characteristics(4)、
TimeDateStamp(4)、MajorVersion(2)、MinorVersion(2)、Type(4)、
SizeOfData(4)、AddressOfRawData(4)、PointerToRawData(4)。目录是 Size 限
定的数组：SizeOfData 为 0 的零长度标记保留在序列里，不截断后续记录；
重复记录也不合并。

## 3. cedar32.pe 逐条记录

目录 7 条记录，TimeDateStamp 依次为 0x65748321–0x65748327，Major/Minor
均为 1/2。下面每条都给出 28 字节原始记录（可用查看器在条目偏移处核对）
和 payload 来源。

### #0 @0x240 — 零长度标记（relation: none）

原始字节：`00000000 21837465 0100 0200 10000000 00000000 00000000 00000000`

Type = 16，SizeOfData = 0，AddressOfRawData = 0，PointerToRawData = 0。
零长度记录不解引用任何地址，没有候选，但它不挡住后面的 6 条记录（旧实
现在这里就停了，这也是基线里 cedar32 读出 0 条记录的原因）。

### #1 @0x25C — 双坐标同址（relation: same）

原始字节：`00000000 22837465 0100 0200 02000000 2b000000 40300000 40060000`

Type = 2（CodeView），SizeOfData = 0x2B，AddressOfRawData = 0x3040，
PointerToRawData = 0x640。

- RVA 候选：0x3040 落在节 [1]（RVA 0x3000 起）→ 0x600 + 0x40 = **0x640**；
- 文件指针候选：**0x640**。

两个坐标算出同一文件偏移，字节必然相同。payload `[0x640, 0x66B)` 归属
section:1：

```
0x640: 52 53 44 53                                      "RSDS"
0x644: 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10  GUID 原始 16 字节
0x654: 03 00 00 00                                      age = 3
0x658: 43 3a 5c 62 75 69 6c 64 5c 63 65 64 61 72 2e 70 64 62 00
       "C:\build\cedar.pdb" + NUL（19 字节，含 NUL 共 0x2B = 4+16+4+19）
```

GUID 文本按字段端序格式化：前 4 字节按 LE u32、接着两个 LE u16、最后 8
字节顺序写 → `04030201-0605-0807-090a-0b0c0d0e0f10`。

### #2 @0x278 — 仅文件指针，payload 在 overlay（relation: file_only）

原始字节：`00000000 23837465 0100 0200 02000000 2b000000 00000000 20080000`

AddressOfRawData = 0（无 RVA 候选），PointerToRawData = 0x820。0x820 ≥
0x800，落在 overlay，payload `[0x820, 0x84B)`：

```
0x820: "RSDS"  01 02 … 10（同一 GUID）  03 00 00 00（age = 3）
0x838: "D:\cache\cedar.pdb" 00
```

与 #1 同一身份（GUID + age 相同），但路径不同——路径不参与身份。

### #3 @0x294 — 双坐标字节冲突（relation: conflict）

原始字节：`00000000 24837465 0100 0200 02000000 2b000000 a0300000 00090000`

AddressOfRawData = 0x30A0 → 0x600 + 0xA0 = **0x6A0**（section:1）；
PointerToRawData = **0x900**（overlay）。两处各 0x2B 字节，都是 RSDS、同
一 GUID、同一路径 "C:\build\cedar.pdb"，但 age 字段不同：

```
0x6A0 候选（RVA 换算）：age @0x6B4 = 04 00 00 00  → age 4
0x900 候选（文件指针）：age @0x914 = 05 00 00 00  → age 5
```

文件指针与 RVA 指向不同字节，这就是要保留的"文件指针与 RVA 不一致"的
证据。两个候选都展示，不替调用者二选一，本条也不进入确定身份组。

### #4 @0x2B0 — 仅 RVA（relation: rva_only）

原始字节：`00000000 25837465 0100 0200 02000000 1b000000 00310000 00000000`

AddressOfRawData = 0x3100 → 0x600 + 0x100 = **0x700**（section:1）；
PointerToRawData = 0（无文件候选）。payload `[0x700, 0x71B)` 是 NB10：

```
0x700: 4e 42 31 30        "NB10"
0x704: 00 00 00 00        offset = 0
0x708: cd ab 34 12        signature = 0x1234abcd（标量，不是 GUID）
0x70C: 07 00 00 00        age = 7
0x710: "legacy.pdb" 00    （11 字节，含 NUL 共 0x1B = 16+11）
```

### #5 @0x2CC — 异址同字节（relation: equal_distinct）

原始字节：`00000000 26837465 0100 0200 02000000 2b000000 40310000 60090000`

AddressOfRawData = 0x3140 → 0x600 + 0x140 = **0x740**（section:1）；
PointerToRawData = **0x960**（overlay）。两处 0x2B 字节完全相同（RSDS、
GUID `04030201-…`、age 3、"C:\build\cedar.pdb"）：地址不同但字节一致，
与 #3 的冲突形成对照。

### #6 @0x2E8 — 未知 CodeView 签名（relation: file_only）

原始字节：`00000000 27837465 0100 0200 02000000 08000000 00000000 c0090000`

PointerToRawData = 0x9C0（overlay），SizeOfData = 8。payload
`[0x9C0, 0x9C8)` = `5a 5a 5a 5a 01 02 03 04`（"ZZZZ"…）。Type 是 2 但签
名不是 RSDS/NB10，保留为 opaque，不做解释。

## 4. cedar64.pe 逐条记录

目录 4 条记录 @0x260，TimeDateStamp 0x65748321–0x65748324。PE32+ 只改
变了可选头宽度（ImageBase 8 字节、数据目录起点 0x108），换算规则不变。

### #0 @0x260 — 仅文件指针，NB10 在 overlay（relation: file_only）

原始字节：`00000000 21837465 0100 0200 02000000 1f000000 00000000 20080000`

PointerToRawData = 0x820（overlay），SizeOfData = 0x1F。payload
`[0x820, 0x83F)`：

```
0x820: "NB10"  offset = 0  signature = 0x1234abcd  age = 7
0x830: "alt\legacy.pdb" 00（15 字节，含 NUL 共 0x1F = 16+15）
```

### #1 @0x27C — 双坐标同址（relation: same）

原始字节：`00000000 22837465 0100 0200 02000000 29000000 40300000 40060000`

AddressOfRawData = 0x3040 → **0x640**，PointerToRawData = **0x640**。
payload `[0x640, 0x669)`：RSDS，GUID 原始字节 `01 02 … 10`，age = 3，
路径 "remote\cedar.pdb"（17 字节含 NUL，共 0x29）。

### #2 @0x298 — 双坐标同址，GUID 不同（relation: same）

原始字节：`00000000 23837465 0100 0200 02000000 2b000000 a0300000 a0060000`

AddressOfRawData = 0x30A0 → **0x6A0**，PointerToRawData = **0x6A0**。
payload `[0x6A0, 0x6CB)`：RSDS，age = 3，路径 "C:\build\cedar.pdb"，但
GUID 第一个字节是 `99` 而不是 `01`（文件 0x6A4 处），文本为
`04030299-0605-0807-090a-0b0c0d0e0f10`。

### #3 @0x2B4 — 零长度标记（relation: none）

原始字节：`00000000 24837465 0100 0200 10000000 00000000 00000000 00000000`

Type = 16，三个坐标字段全 0，目录到此结束（Size 也只够 4 条）。

## 5. 跨文件身份归组

身份键 = 格式种类 + 全部身份字段（RSDS：GUID + age；NB10：offset +
signature + age），路径不参与。只有已识别且无来源冲突的记录进入确定身
份组：

| 身份 | 成员（文件 #目录序号） | 各自路径 |
|---|---|---|
| `RSDS:04030201-0605-0807-090a-0b0c0d0e0f10:00000003` | cedar32 #1、#2、#5；cedar64 #1 | C:\build\cedar.pdb / D:\cache\cedar.pdb / C:\build\cedar.pdb / remote\cedar.pdb |
| `RSDS:04030299-0605-0807-090a-0b0c0d0e0f10:00000003` | cedar64 #2 | C:\build\cedar.pdb |
| `NB10:00000000:1234abcd:00000007` | cedar32 #4；cedar64 #0 | legacy.pdb / alt\legacy.pdb |

两组对照关系值得注意：

- **同路径不同身份**：cedar64 #2 与 cedar32 #1 的路径都是
  "C:\build\cedar.pdb"，但 GUID 差第一个字节（0x6A4 处 `99` 对 `01`），
  是不同调试文件身份。
- **同身份不同路径**：cedar32 #2 把同一 GUID+age 写成了
  "D:\cache\cedar.pdb"；cedar64 #0 与 cedar32 #4 的 NB10 三元组相同而路
  径不同。路径只是构建机器上的线索，不是身份。

不进组的记录：cedar32 #3（双来源字节冲突，age 4 与 age 5 两个候选都展
示，不选定）、cedar32 #6（opaque）、两个文件里的零长度标记（无
payload）。

## 6. 核对方法

- `bash task_support/run.sh target` 会用 `examples/debug_origin` 对两份
  样本生成 TSV 报告（R = 目录项及原始 28 字节，C = 候选字节，G = 身份
  组成员），并由 `task_support/report_check.cpp` 校验报告中的原始字节
  确实存在于原文件所引位置。
- 手工核对：按第 2 节的目录偏移（cedar32 为 0x240、cedar64 为 0x260）
  读 28 字节记录；按第 3、4 节的候选偏移读 payload；GUID、age、路径与
  第 5 节的分组一一对应。报告中每条 R/C 行的字节都能在上述文件偏移处
  逐字节对上。
