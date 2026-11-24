# 分段（banked）DWARF 读取与归属分析说明

本目录的实现为 gimli 的只读路径增加了段选择器（segment selector）支持，
并在其上构建了一个分段地址归属分析器。地址始终表示为独立的
`(selector, offset)` 二元组：selector 标识 bank，offset 是段内偏移，二者
不会被拼接成宿主机指针，也不会换算成物理地址。

## 生产读取路径的改动

* `src/read/aranges.rs`
  * `ArangeHeader::segment_size()` 返回元组中选择器的原始宽度（0/1/2/4/8）。
  * 元组按 `(selector, address, length)` 读取，首个元组的对齐以**当前
    contribution 起点**为基准计算，因此同一 section 中相邻 contribution 可以
    使用不同选择器宽度、地址宽度和 DWARF32/64 格式。
  * `ArangeEntry::segment()` 保留选择器；`address()`/`range()` 仍只表示段内
    偏移，旧的 `range.end` 计算与 tombstone 处理保持不变。
  * 只有三字段全零的元组才是空记录；非零 selector 配合零 offset 是合法范围。
  * `ArangeEntry::tuple_offset()` 给出元组在 `.debug_aranges` 中的绝对字节
    偏移，作为可独立回放的证据。

* `src/read/line.rs`
  * `LineProgramHeader::segment_size()` 读取 DWARF5 头中的
    `segment_selector_size`，宽度同样限制为 0/1/2/4/8。
  * 带段的 `DW_LNE_set_address` 按“选择器在前、偏移在后”解码，产生新的
    `LineInstruction::SetSegmentedAddress { segment, address }`；平坦程序仍
    产生原有的 `SetAddress`。
  * `LineRow::segment()` 与 `LineSequence::segment()` 保留段身份。地址推进只
    修改 offset；每个 sequence 初始化 selector 为 0，终止行保留当前
    selector，下一个 sequence 重新从 0 开始且不继承。
  * 旧的 `address()`、`range()`、`start/end`、方法签名与全局 `Encoding`
    均未改变。

* `src/write/line.rs`：转换遇到 `SetSegmentedAddress` 时显式返回
  `ConvertError::UnsupportedLineInstruction`，不会静默丢弃选择器。

* `src/read/segmented.rs`：新的分析 API `segmented_lookup`，复用真实的
  aranges、DIE 属性和 line 读取链，不另写解析器。

## 固定样本 `fixtures/banked-fw` 的结论

工具：`tools/segmented_lookup.rs`（薄驱动，分析逻辑与契约测试共用同一生产
API），输出格式见 `support/REPORT.md`。

### 为什么相同 offset 在不同段归属不同

以 offset `0x1004` 为例：

| query | selector | 归属函数 | 源位置 | 结论来自的字节 |
| --- | --- | --- | --- | --- |
| 0 | `0x21` | `sample_read` | `firmware/sensor.c:4` | aranges 元组 `0x12`（seg=0x21），line seq0 row1 |
| 1 | `0x34` | `bank_service` | `firmware/dispatch.c:8` | aranges 元组 `0x1b`（seg=0x34），line seq1 row0 |

两个地址的段内偏移数值相同，但选择器不同。aranges 用**独立的选择器字段**
把它们区分到不同元组，DIE 上的 `DW_AT_segment`（CU 继承的 `DW_OP_constu
0x21` 与 `bank_service` 显式覆盖的 `DW_OP_constu 0x34`）进一步确认函数的段
身份，line 程序的两个 sequence 也分别固定在 selector 0x21 / 0x34。因此
offset 本身不再是全局唯一键，`(selector, offset)` 才是。

### 只能恢复部分信息的查询

* **有函数、无源码行**：如 query 2（`0x21,0x100c`）仍归属 `sample_read`，
  但 `0x100c` 是 sequence 的终止地址，非终止行的半开区间不覆盖它，于是
  file/line 留空（`-`），函数与 aranges 证据保留。
* **CU 内无函数、可能有行**：如 query 3（`0x21,0x1014`）落在 CU 的 arange
  范围内但超出任何子程序半开范围，保留 CU 与 bank 位置，不虚构函数。
* **无 CU**：如 query 4/6/8/11 没有任何 arange 元组覆盖，cu 到 row 全部为
  `-`；若 `banks.tsv` 能定位字节则仍给出 bank 文件与文件内偏移。
* **未知 selector**：query 10（`0x55`）既无 arange 也无 bank 映射，所有字段
  为 `-`，不伪造任何证据。
* **零 offset 合法范围**：query 9（`0x21,0x0`）由非零选择器 + 零偏移元组
  （`0x24`）命中，不会被误判为空记录。

每条结论都附带 aranges contribution/元组偏移、`.debug_info` 的 CU/DIE/段
属性 DIE 偏移，以及 line contribution/sequence/row 三元组，可脱离分析器直接
对照原始 section 字节重新回放。
