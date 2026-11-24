# 模块数据符号位置分析（Small MSF 2.00 / MSF7）

本报告由离线示例 `src/Examples/ExampleModuleSymbolReport.cpp` 实际读取合成容器生成，
完整逐字段数据见同目录 [`module_symbols.tsv`](module_symbols.tsv)。
示例只在内存中构造 `task_support/analysis_fixture.h` 提供的三份调试容器字节，
经 `PDB::ValidateFile` → `PDB::CreateRawFile` →
`PDB::AnalyzeModuleDataSymbols` 读取，不执行任何映像、不改写输入，
也没有额外构造“改了物理偏移的 MSF7”。

## 1. 三份输入

| 标识 | 序列化器 | 内容 |
| --- | --- | --- |
| `input1_msf7` | `fixture::msf(analysis_streams(false))` | 原 MSF7，512 字节页 |
| `input2_small_msf2` | `fixture::small_msf(analysis_streams(false))` | Small MSF 2.00，1024 字节页，`firstDataPage=40` |
| `input3_small_msf2_alpha60_firstpage80` | `fixture::small_msf(analysis_streams(true), {}, 80)` | Small MSF 2.00，alpha 节内位置改为 `0x60`，数据页从 80 开始分配 |

`analysis_streams(false)` 的流 6 是 `alpha.obj`、流 7 是 `beta.obj`，
各含一条 `S_OBJNAME`、一条作为合法填充的 `S_UNAMESPACE` 和一条 `S_GDATA32`。
变量记录都从流内偏移 **1008 (`0x3F0`)** 开始，记录长 20 字节，
变量名 `state` 从 **1022** 开始，正好跨过 1024 字节页边界。

## 2. 分析结果总表（节号 + 节内偏移不是 RVA）

| 输入 | 格式 | 模块 | 流 | 记录偏移 | 变量 | typeIndex | 节 | 节内偏移 | 物理区间（文件偏移:长度，十进制） |
| --- | --- | --- | ---: | ---: | --- | --- | ---: | ---: | --- |
| input1 | MSF7 | alpha.obj | 6 | 1008 | state | `0x1000` | 1 | `0x40` | `4080:16;4608:4` |
| input1 | MSF7 | beta.obj  | 7 | 1008 | state | `0x1001` | 2 | `0x88` | `7152:16;7680:4` |
| input2 | SmallMSF2 | alpha.obj | 6 | 1008 | state | `0x1000` | 1 | `0x40` | `41968:16;43008:4` |
| input2 | SmallMSF2 | beta.obj  | 7 | 1008 | state | `0x1001` | 2 | `0x88` | `46064:16;47104:4` |
| input3 | SmallMSF2 | alpha.obj | 6 | 1008 | state | `0x1000` | 1 | `0x60` | `82928:16;83968:4` |
| input3 | SmallMSF2 | beta.obj  | 7 | 1008 | state | `0x1001` | 2 | `0x88` | `87024:16;88064:4` |

TSV 另外给出了 `block_size/free_block_map_index/block_count/directory_size`、
十进制/十六进制的 typeIndex 与偏移、区间数量以及十六进制物理区间和 `image_file` 列。
示例同时把三份实际生成的容器字节写到本目录，供物理偏移直接回查文件页：

- [`input1_msf7.pdb`](input1_msf7.pdb)（8704 字节）
- [`input2_small_msf2.pdb`](input2_small_msf2.pdb)（49152 字节）
- [`input3_small_msf2_alpha60_firstpage80.pdb`](input3_small_msf2_alpha60_firstpage80.pdb)（90112 字节）

例如 TSV 中 input2/alpha 的区间 `41968:16;43008:4` 表示记录前 16 字节在
`input2_small_msf2.pdb` 偏移 41968，末 4 字节在偏移 43008。把两段拼回后，
记录头应为 `size=18 kind=0x110d (S_GDATA32)`，尾部 6 字节为 `state\0`。
可用下面的离线 Python 片段独立复核整张 TSV（不依赖本库）：

```python
import csv
for row in csv.DictReader(open("analysis/module_symbols.tsv"), delimiter="\t"):
    data = open("analysis/" + row["image_file"], "rb").read()
    record = b"".join(data[int(o):int(o)+int(n)]
                      for part in row["physical_ranges_dec_offset:length"].split(";")
                      for o, n in [part.split(":")])
    assert len(record) == int(row["record_size"]) == 20
    assert int.from_bytes(record[2:4], "little") == 0x110D
    assert record[14:20] == b"state\x00"
```


**地址口径：** 这些容器没有映像节表，因此“节号 + 节内偏移”只表示 COFF 节相对位置，
不能伪称成 RVA 或运行时地址。`typeIndex` 按记录原样报告，
分析器不解析 TPI、不猜测类型或变量宽度，也不做旧版 TPI/DBI 转换。

## 3. 两份容器中的同名变量

两个模块里都有同名变量 `state`，分析结果不做合并：

- `alpha.obj`：`typeIndex=0x1000`，节 `1`，偏移 `0x40`；
- `beta.obj`：`typeIndex=0x1001`，节 `2`，偏移 `0x88`。

它们由“流号 + 流内记录偏移”唯一区分（这里是 `(6,1008)` 与 `(7,1008)`），
结果结构里同时保留 `streamIndex` 与 `recordOffset`。

同一份逻辑流在 MSF7 与 Small MSF 中的物理证据不同，但恢复出的符号位置完全一致：

- input1 流 6 的逻辑页映射到物理页 `[5,7,9]`（512 字节页）。
  记录 `[1008,1028)` 落在逻辑第 1、2 页：
  `7*512 + (1008-512) = 4080`（16 字节），随后 `9*512 = 4608`（4 字节）。
- input2 流 6 的逻辑页映射到物理页 `[40,42]`（1024 字节页）：
  `40*1024 + 1008 = 41968`（16 字节），随后 `42*1024 = 43008`（4 字节）。

区间始终按流的逻辑顺序给出；跨不连续物理页时拆成多条，不会压成一个连续范围。

## 4. 跨页名称

变量名 6 字节 `state\0` 位于流内 `[1022,1028)`。在 1024 字节 Small 页中：

- `[1022,1024)` 的 `st` 在第 40 页末尾，文件偏移 `40*1024+1022 = 41982`，长度 2；
- `[1024,1028)` 的 `ate\0` 在不连续的第 42 页开头，文件偏移 `42*1024 = 43008`，长度 4。

`DirectMSFStream::ReadAtOffset(..., 1022)` 跨页拼回的 6 字节、
`CoalescedMSFStream` 整流后的 NUL 结尾字符串、以及
`ModuleSymbolStream::FindRecord(S_GDATA32)->data.S_GDATA32.name` 三者一致，
都得到 `state`。分析结果持有自己的 `std::string`，不借用临时 Coalesced 缓冲区。

## 5. 第三份输入的差异

`analysis_streams(true)` 只把 alpha 的节内偏移从 `0x40` 改成 `0x60`，
记录长度和字节布局不变；`small_msf(..., firstDataPage=80)` 把数据页整体后移：

- 流 6 物理页由 `[40,42]` 变为 `[80,82]`，alpha 证据变为 `82928:16;83968:4`，
  恢复位置相应变为节 `1` + `0x60`；
- beta 的载荷完全没变（仍是节 `2` + `0x88`），但物理页由 `[44,46]` 变为 `[84,86]`，
  证据变为 `87024:16;88064:4`。

这正好验证“**物理页换了但载荷相同时，恢复的符号位置不变，文件证据相应改变**”。

`streams` 测试组对每份结果都按 `physical_ranges` 从原始文件字节重新拼出整条 20 字节
记录，与 `CreateMSFStream<DirectMSFStream>` 在记录偏移处读到的字节逐字节比较，
并再与序列化源分流比对，因此 TSV 中的区间不是形式标注，而是可回查文件页的证据。

## 6. 容器 API 的格式与寿命说明

```cpp
namespace PDB {
    enum class ContainerFormat { MSF7, SmallMSF2 };

    struct ContainerInfo {
        ContainerFormat format;
        uint32_t blockSize;
        uint32_t freeBlockMapIndex;
        uint32_t blockCount;
        uint32_t directorySize;
    };
}
```

- `RawFile::GetContainerInfo()` 以只读值返回上述五项归一头部信息；
  两种格式都可用，`ExamplePDBSize` 已改为消费它，不再无条件解引用 SuperBlock。
- `RawFile::GetSuperBlock()` 对 MSF7 保持原行为（指向输入偏移 0），
  对 Small MSF 2.00 返回 `nullptr`。
- Small 目录中的 16 位页号按**无符号**归一到 `uint32_t`
  （高位页号 `0x8000` 不会变成负数偏移）；旧内存指针槽被忽略。
  `0xffffffff` 仍是 nil 标记，nil 与 empty 都不占页号。
- 支持 Small 页大小 1024/2048/4096 及各自合法 FPM 位置
  `(1,9)`、`(1,5)`、`(1,2)`；MSF7 原有校验规则保持不变。
- 归一目录元数据归每个 `RawFile` 所有。移动构造、跨格式移动赋值、自移动后目的对象
  均保持正确；源对象先析构不会带走目的对象的目录。MSF7 的页表指针指向随移动转移的
  Coalesced 目录内存，Small 的归一 `uint32_t` 页表存放在随移动转移的 `std::vector`
  中，堆地址不变，因此移动前从源对象创建的 `DirectMSFStream` 在目的对象与原始输入
  都存活时继续可用。
- 原始文件字节始终由调用者保活，API 不延长输入缓冲寿命。
- `AnalyzeModuleDataSymbols` 返回的名称与数值自持有，RawFile/临时流析构后仍可使用；
  `physicalRanges` 只是数值偏移，若要重新拼接证据字节，仍需调用者保留原始容器缓冲。

## 7. 完整离线命令

在仓库根目录执行：

```bash
# 构建库并运行 directory/streams/ownership 三组与旧式符号基线
bash task_support/run.sh target

# 仅重新生成 analysis/module_symbols.tsv
bash task_support/make_report.sh

# 或者手动执行 make_report.sh 内的完整命令：
bash task_support/build.sh
c++ -std=c++17 -O2 -I. -Isrc -Itask_support \
  src/Examples/ExampleModuleSymbolReport.cpp \
  .task-build/libraw_pdb.a -o .task-build/module_symbol_report
mkdir -p analysis
./.task-build/module_symbol_report analysis/module_symbols.tsv
```

也可用 CMake（C++17）构建并运行独立目标 `ModuleSymbolReport`：

```bash
cmake -S . -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/cmake --target ModuleSymbolReport -j
.task-build/cmake/src/Examples/ModuleSymbolReport analysis/module_symbols.tsv
```

> 说明：仓库既有的 `Examples` 聚合目标在 macOS 上有一处与本次改动无关的
> `fopen_s` 编译问题（`ExampleTypes.cpp`），因此离线命令沿用
> `task_support/build.sh`，它只编译库与可移植的两个示例源文件。

## 8. 测试覆盖

`tests/small_msf.cpp` 通过 `task_cases/task_case_count` 注册：

- **directory**：两种格式的 `ContainerInfo`、三种 Small 页大小与各自 FPM
  （含非法 FPM 拒绝）、非法页大小与截断头拒绝、260 流/三页分散目录、
  nil/empty 之后的正常流与零长度流创建、`0x8000` 高位页号。
- **streams**：三份输入的逐字段断言、Direct 跨页读、Coalesced 整流与非对齐子窗口、
  `ModuleSymbolStream` 枚举/查找、按物理区间拼回记录比对、物理页变化而载荷不变。
- **ownership**：Small/MSF7 移动构造、双向跨格式移动赋值、自移动、移动前流存活、
  三份映像 64 轮交错读取不串目录、分析结果在临时 RawFile/缓冲释放后仍可用。
