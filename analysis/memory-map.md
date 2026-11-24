# mixed.wasm 内存引用图谱

本表与说明全部来自对 `task_support/fixtures/mixed.wasm` 的**静态**节解码与函数验证
（`DecodeModuleForInspection` + `Module.Validate`）。过程中没有实例化模块、没有分配线性内存、
没有调用导入、没有执行任何函数；页数、静态 offset 与数据段归属都是二进制里的编码事实，
不是运行期访问结果，也不据此判断插件是否安全或是否越界。

逐行数据见同目录 `memory-map.tsv`，下文坐标（行类型与字段）均引用该表。

## 一、三块内存声明（`kind=declaration`）

内存索引空间按“导入前缀 + 本地定义”组合，顺序与二进制一致：

| 内存索引 | 来源 | 地址类型 | min（页） | max（页） | has_max |
| --- | --- | --- | --- | --- | --- |
| 0 | 导入 `env.ram` | i32（32 位内存） | 1 | 2 | true |
| 1 | 本地定义 | i64（memory64，limits flag 0x05） | 4294967296（2^32） | 4294967297（2^32+1） | true |
| 2 | 本地定义 | i64（memory64，limits flag 0x05） | 0 | 2 | true |

- 内存 0 行的 `imported=true`、`import_module=env`、`import_name=ram`、`is64=false`，
  保留原始 limits 页数 1/2；它是组合空间里的第一个内存，因此索引为 0。
- 内存 1、2 行的 `imported=false`、`is64=true`。内存 1 的页数本身就超过 32 位地址空间的
  2^16 页上限，这正是必须按 64 位 limits（ULEB64）原样保存、不能收窄成 `uint32` 的原因。
- 三条记录都显式编码了 max（`has_max=true`）；若二进制未编码 max，表中 `max` 列留空且
  `has_max=false`，不会把运行期上限误写成隐式声明。

## 二、函数与原始 Body（`kind=function_body`）

导入节先导入了函数 `env.f`（类型 0），因此函数索引空间存在一个导入前缀：
模块里唯一的本地 Code 条目属于**函数索引 1**（`function_index=1`），而不是 0。

该行 `body_hex` 是 `Code.Body` 从首个 opcode 起的原始字节，未被验证过程修改：

```
4200 28 42 01 80808080 14 1a 4200 4100 4100 fc 0a 01 00 3f 02 1a 4200 40 02 1a 0b
```

下文每条指令的 `body_offset` 都是相对这段字节首字节（offset 0）的位置，可直接与
`body_hex` 对照。

## 三、四条访存指令（`kind=memory_use`）

所有记录均由生产验证游标在遍历同一 Body 时写出，顺序按函数索引与 Body 偏移排列。

1. **i32.load — `body_offset=2`，内存 1，i64 地址，静态 offset 5368709120（5×2^30）**
   - Body[2]=`0x28` 即 i32.load。紧随的 memarg 第一个 LEB 是 `0x42`：低位对齐值 2，
     且第 6 位置 1，表示后面带显式内存索引；下一字节 `0x01` 即内存 1。
   - offset 统一按 ULEB64 读取（`80 80 80 80 14` → 5×2^30 = 5368709120），超过 4GiB
     也不截断、不会被误当成下一条 opcode。内存 1 是 64 位内存，地址操作数为 i64
     （表中 `address_type=i64`），该 offset 在 64 位地址范围内合法。
   - 注意这是编码里的**静态**偏移；是否发生实际访问、访问是否越界，静态分析不做结论。

2. **memory.copy — `body_offset=17`，目的内存 1、源内存 0**
   - Body[17]=`0xfc` 是 misc 前缀，Body[18]=`0x0a` 是 memory.copy；其后两个完整 ULEB
     内存索引按目的、源顺序为 `0x01, 0x00`，表中 `memory_indices=1,0`。
   - 类型按各内存分别确定：目的地址跟随内存 1 → i64（`copy_dst_type=i64`）；
     源地址跟随内存 0 → i32（`copy_src_type=i32`）；复制长度取两者较窄的地址类型 → i32
     （`copy_length_type=i32`）。该指令不是 memarg 指令，`offset=0`。

3. **memory.size — `body_offset=21`，内存 2**
   - Body[21]=`0x3f`，立即数 `0x02` 是完整 ULEB 内存索引，指向内存 2；size 结果类型
     跟随所选内存，内存 2 为 64 位，故表中 `address_type=i64`。

4. **memory.grow — `body_offset=26`，内存 2**
   - Body[26]=`0x40`，同样以 `0x02` 引用内存 2；grow 的输入（增长页数）与结果（旧页数，
     失败为 -1）都跟随内存 2 的 i64 类型。

## 四、主动数据段（`kind=data_segment`）

- 唯一的数据段（`segment_index=0`）是主动段（`passive=false`），前缀 0x2 显式给出
  目标 `memory_index=1`；偏移表达式为 `i64.const 0x00` 后接 end，与内存 1 的 64 位
  地址类型匹配；初始化字节 `init_hex=112233`。
- 这表示：**如果**模块被实例化，该段会被布置到内存 1 的 i64 常量偏移 0 处。本分析没有
  执行这一布置，也不检查运行期边界；被动段不会绑定内存（本例无被动段）。

## 五、编码如何把声明与指令关联起来

- 组合内存索引空间（导入在前、本地在后）同时用于声明、指令立即数、内存导出检查和主动
  数据段的 `memoryidx`，所以 i32.load/memory.copy/memory.size/memory.grow 里的索引数字
  能无歧义地映射回第一节的某条声明，并由声明的 `is64` 决定该指令要求 i32 还是 i64 地址。
- memarg 对齐立即数的第 6 位把“是否带显式内存索引”编码进同一字节；不带该位时内存索引
  隐式为 0，offset 仍独立按 ULEB64 读取，因此大偏移与多字节内存号都不会破坏游标对齐
  （测试在独立列出的 Body 坐标上核对了这一点）。
- bulk 指令（0xfc 前缀）携带独立的内存索引序列：memory.copy 是目的、源两个索引，
  memory.init/memory.fill 是一个索引；memory.init 的源偏移与长度、memory.fill 的填充值
  仍固定为 i32，地址与长度才跟随目标内存宽度。
- 上述关系全部在既有节解码器和函数验证游标内建立；失败的验证会丢弃已记录的引用，
  重复验证不会重复追加，`Code.Body` 与输入字节在整个过程中保持原样。
