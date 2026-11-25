# 来源重建分析（source evidence analysis）

本文件的全部结论都来自 `sample_report.c` 通过生产库接口
`backtrace_dwarf_evidence` 生成、保存在 `sample-output/` 的报告，以及对报告
所指 `.debug_info` / `.debug_line` / `.debug_str` 字节区间的回读。报告里的
偏移是 section offset；换算成 ELF 文件偏移的公式是对应 `S` 记录里的 section
起点加上 section offset，例如 capture-A 的 `.debug_info` 起点为 `0x140`，
`.debug_line` 起点为 `0x2d7`。三个对象的 section 起点：

| 对象 | .debug_info | .debug_line | .debug_abbrev | .debug_str |
|---|---|---|---|---|
| capture-A.elf | 0x140 | 0x2d7 | 0x24a | — |
| capture-B.elf | 0x140 | 0x2dc | 0x24f | — |
| optimized-C.elf | 0x1312 | 0x15a0 | 0x125d | 0x1461 |

复现命令：`bash support/run.sh target`。报告在 `sample-output/{A,B,C}.tsv`，
回放与逆序查询产生 `*-replay.tsv` 和 `*.reverse.tsv`。

## 1. capture-A：三层嵌套与逐属性来源

PC `0x400111`（`sample-output/A.tsv` 的 `Q 2`）命中三个帧，输出按最内层到
物理函数排列，depth 分别为 2、1、0：

| depth | 具体 DIE | 命中半开区间 | 名字 |
|---|---|---|---|
| 2 | CU0 `0x64` inlined_subroutine | `[0x400110,0x400118)` | fold |
| 1 | CU0 `0x4f` inlined_subroutine | `[0x400108,0x400128)` | mix |
| 0 | CU0 `0x38` subprogram | `[0x400100,0x400140)` | worker |

区间全部按半开处理：边界 PC `0x400140` 的查询是 `Q 5`，`frame_count=0`，
不写任何假帧；`0x40013f` 仍落在 worker 区间内（`Q 4`，一帧）。

### 1.1 fold 的名字沿三跳引用而来，本地名字不参与

fold 具体 DIE `0x64` 本身没有 `DW_AT_name`，名字的 `R` 记录给出完整三跳：

1. hop0 `abstract_origin`，`ref_addr` 操作数在 info `[0x65,0x69)`，
   字节为 `c5 00 00 00`（ELF 文件偏移 `0x1a5`），从 DIE `0x64`（CU0）
   指到 `0xc5`（CU1，`to_cu=0x90`）。
2. hop1 `specification`，`ref4` 操作数在 info `[0xcb,0xcf)`，值 `0x3f`
   是 CU1 内偏移，`to_die = 0x90 + 0x3f = 0xcf`，仍在 CU1。
3. hop2 `specification`，`ref_addr` 操作数在 info `[0xd0,0xd4)`，
   字节为 `00 01 00 00`，跨 CU 指到 `0x100`（CU2，`to_cu=0xd5`）。

最终 `A name` 的提供者是 CU2 的声明 DIE `0x100`，属性操作数在
info `[0x101,0x106)`（ELF `0x241`），内联字符串字节 `66 6f 6c 64 00`
即 `"fold\0"`，`U` 记录长度 5 含 NUL。注意 CU1 的 `0xc5` 自己带
`DW_AT_name "fold"`（info `[0xc6,0xcb)`），但库的名字偏好是
引用名优先于本地名，因此引擎继续沿 specification 走到 `0x100`，
没有在 `0xc5` 停下——这与 `read_referenced_name` 的行为一致。

### 1.2 本地覆盖与逐属性独立继承

同一个 fold 帧，各属性的提供者不同，引擎不会因为名字已定就停止追踪：

- `decl_file` 在具体 DIE `0x64` 上本地存在：`A decl_file` 指向
  info `[0x69,0x6a)`，原始值 `2`（写着 2 即存在）。文件编号用**提供该属性
  的 CU0** 的行表解释：`T decl_file` 指向 stmt_list `0x0` 的文件表项 2，
  `.debug_line [0x27,0x33)`（ELF `0x2fe`），12 字节，开头是
  `"shared.h\0"` 后接目录索引/时间/大小三个 ULEB；目录索引为 0，目录字段
  写 `-`，相对 CU0 的 comp_dir（info `[0x18,0x28)`，`"/build/a/client"`）
  解析为 `/build/a/client/shared.h`。
- `decl_line`、`decl_column` 在 `0x64`、`0xc5`、`0xcf` 上都不存在，于是
  沿同一条三跳链继承到 `0x100`：行 91（info `[0x107,0x108)`）、列 4
  （info `[0x108,0x109)`）。两份 `R` 链与名字链逐跳相同，`A` 终点不同，
  证明每个属性独立走完整 abstract_origin、再 specification 的顺序。

### 1.3 值为 0 也算存在

mix 具体 DIE `0x4f` 带 `DW_AT_decl_column = 0`（`Q 1`，
`A decl_column` 在 info `[0x63,0x64)`，raw `0x0`）。报告保留该 `A`，
F 记录的 decl_column 输出 `0` 而不是 `-`。该 DIE 没有 decl_file/decl_line，
这两个属性经 `0x4f -> 0xbb -> 0xf8` 两跳到 CU2 取得（shared.h、行 73）。

### 1.4 调用位置只读具体 inline

fold/mix 的 call_file/call_line/call_column 的 `A` 全部落在具体 inline DIE
上（如 fold 的 info `[0x76,0x79)`，call_line 21、call_column 7），没有任何
`R` 跳转；物理函数 worker 的三个调用属性都不存在，F 记录对应位置写 `-`。

## 2. 同一抽象函数的两个调用点是不同实例

CU0 里有两个具体 `DW_TAG_inlined_subroutine` 都把 abstract_origin 指向
CU1 的 `0xbb`（抽象 mix）：

- DIE `0x4f`：区间 `[0x400108,0x400128)`，call_line 19、call_column 3
  （info `[0x61,0x63)`）。
- DIE `0x7a`：区间 `[0x400130,0x400138)`，call_line 29、call_column 11
  （info `[0x8c,0x8e)`），且属性顺序不同（low_pc/high_pc 在
  abstract_origin 之前）。

两者名字都解析为 `mix`，但具体 DIE 身份、命中区间、调用坐标全部不同。
PC `0x400109` 选中 `0x4f`（`Q 1`），PC `0x400131` 只选中 `0x7a`
（`Q 3`，两帧）。重复 PC（`Q 6`、`Q 7`）保留为独立查询并得到同样的具体
DIE 身份，证明“同名”不等于“同一来源”：来源身份是具体 DIE 偏移加所属 CU，
而不是名字字符串。

## 3. 跨 CU 引用与文件表归属

capture-A 的引用图跨三个 CU：CU0 `[0x0,0x90)`、CU1 `[0x90,0xd5)`、
CU2 `[0xd5,0x10a)`。每条 `R` 都同时记录 `from_cu` 和 `to_cu`，例如
`0xcf -> 0x100` 一跳 `from_cu=0x90,to_cu=0xd5`。文件编号始终按**提供
decl_file/call_file 的那个 CU** 的 stmt_list 行表解释：mix 的 decl_file
由 CU2 `0xf8` 提供（值 2），`T` 用的是 CU2 的 stmt_list `0x8e`，其文件表
项 2 在 `.debug_line [0xb5,0xc1)`，配合 CU2 comp_dir
（info `[0xe7,0xf4)`，`"/build/a/sdk"`）解析为
`/build/a/sdk/shared.h`；而 fold 的 decl_file 由 CU0 提供，解析为
`/build/a/client/shared.h`。同样的编号 2、同样的基名 `shared.h`，因 CU 与
comp_dir 不同而成为不同来源，报告不会把它们合并。

## 4. capture-B：同构对照对象，身份与字节都不同

capture-B 与 A 结构同构，但指令基址是 `0x500300`、comp_dir 是
`/build/b/...`，DIE 位置整体后移：worker `0x3d`、两个 mix 实例 `0x54` 与
`0x7f`、fold `0x69`。例如 B 的 fold 具体 decl_file 在 info `[0x6e,0x6f)`，
抽象起点引用操作数在 `[0x6a,0x6e)` 指向 `0xca`，与 A 的 `0x64/0x65`
字节区间不同。B 的同名 `shared.h` 经 CU0 comp_dir `/build/b/client`
解析为 `/build/b/client/shared.h`。因此即使名字、行号、嵌套形状完全相同，
两份对象的证据在 CU 身份、DIE 偏移、comp_dir、文件表偏移上都不可互换。

## 5. optimized-C：真实编译器产物

`optimized-C.elf` 是 clang `-O2 -g -gdwarf-4` 的自然产物（单个 CU
`[0,0x14f)`），证据形态与手工对照对象互补：

- **CU 内 ref4**：fold 实例 DIE `0xd9` 的 abstract_origin 操作数在
  info `[0xda,0xde)`，字节 `2a 00 00 00`，`ref4` 目标为
  `CU 起点 0 + 0x2a = 0x2a`（抽象 fold）。`R` 的 from/to CU 都是 0。
- **strp 名字**：抽象 fold 的 name 是 `DW_FORM_strp`，`A` 的 raw 是
  `.debug_str` 偏移 `0xb`，`U` 回读 ELF `0x1461+0xb=0x146c` 处
  `"fold\0"`（5 字节）；worker 的 strp 偏移 0 指向 `"worker\0"`。
- **带目录索引的文件表**：文件项 1 在 `.debug_line [0x25,0x34)`
  （15 字节，`"optimized.c\0"` 加 `01 00 00`），目录索引 1 指向
  `[0x1c,0x24)` 的 `"private\0"`（8 字节）；comp_dir 是 strp 偏移
  `0x10`，回读 `"/sample/build\0"`，拼接为
  `/sample/build/private/optimized.c`。
- **缺失属性写 unknown**：抽象 fold `0x2a` 没有 decl_column，具体实例也没
  有，F 记录 decl_column 为 `-`，且没有对应 `A`；不用行表或其他层补值。
- **抽象函数复用**：mix 抽象 DIE `0x59` 被两个具体实例 `0xb3`
  （区间 `[0x600100,0x600117)`，call 12:16）和 `0x100`
  （区间 `[0x600117,0x60012e)`，call 14:10）复用；各自内嵌的 fold 实例
  `0xd9`、`0x126` 也同样复用抽象 fold `0x2a`。PC `0x600100` 与
  `0x600117` 因此得到同名但不同具体 DIE 的两条三层链。
- 半开边界：`0x60012e` 仍在 worker `[0x600100,0x60012f)` 内（一帧），
  `0x60012f` 为 `frame_count=0`。

## 6. 逆序重查与同进程多对象的缓存隔离

`support/report_main.c` 在同一进程内按 A、B、A 的顺序分析，并把 pcs 逆序
再查一遍：

- A 的两次报告字节级一致（`compare(a,c)` 通过，产出 `A-replay.tsv`）。
- 逆序 pcs 的每个查询块去掉 query 序号后与正序对应块完全相同
  （`*.reverse.tsv` 校验通过），说明选择只依赖 PC 与 DWARF 数据，不依赖
  查询顺序。
- 第二轮以 C、B、C 交替分析（`C.tsv`、`B-again.tsv`、`C-replay.tsv`）。
  证据缓存挂在每个 `dwarf_data`（每个对象一份）上，键是 PC；`evidence_test`
  里 `test_cross_object_isolation` 断言 A、B 返回不同的证据记录、同 PC 重查
  返回同一缓存指针、穿插查询 B 后 A 的最内层 DIE 仍是 `0x64`，证明缓存不会
  串用对象或 CU。
- `evidence_test` 共 72 条断言（`EVIDENCE_TESTS_RETURNED checks=72`），
  覆盖 ref4/ref_addr 操作数字节回读、三跳 specification 链、文件/目录表项
  区间、strp 偏移与字符串回读、零值属性保留、空命中与跨对象隔离，均通过。

## 7. 生命周期与生产路径约束

- 所有证据结构由 `backtrace_alloc` 在 `backtrace_state` 上分配，借用
  `backtrace_dwarf_add` 传入的 section 缓冲；state 在本库中不释放，驱动也
  不释放这些缓冲，证据指针在进程内长期有效，调用方不释放。
- 证据复用库的生产读取路径：`read_attribute`、`read_uleb128`、
  `lookup_abbrev`、`find_unit`、`read_initial_length` 等，字节区间在解析
  当下围绕实际读取位置记录，不做按值反搜，也不引入第二套 DWARF 解析器或
  addr2line。PC 到 CU、物理函数到内联链的选择与 `dwarf_lookup_pc` 共用
  `dwarf_prepare_unit_for_pc` 与同一套 bsearch/半开区间平局规则，旧的
  `backtrace_pcinfo`、MOREDATA 与提前停止回调行为保持不变。
