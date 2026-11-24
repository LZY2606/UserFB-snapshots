# Thumb IT 谓词来源分析

本文件由 `tests/thumb_it_context.c` 中注册到 task driver 的真实 C 消费者再生。运行：

```sh
bash task_support/run.sh target
```

`Origin` 组完整批量解码固定样本并逐行写出 [`thumb-it.tsv`](thumb-it.tsv)；`Continuity` 组用同一个消费者结构比较分块结果，并写出 [`thumb-it-disconnect.tsv`](thumb-it-disconnect.tsv)。所有来源字段均取自生产路径返回的 `cs_insn.detail->arm.it`，不是从助记符字符串反推。

固定输入（只作静态解码，不执行）：

```text
字节（小端存储）: 0c bf 01 20 02 21 03 22
起始 PC:         0x1000
```

## 逐指令结论

| 地址 | 原字节 | 反汇编 | cc | update_flags | present/is_it | block_address | mask | slot | first_cc |
|---:|---|---|---:|---:|---|---:|---:|---:|---:|
| `0x1000` | `0c bf` | `ite eq` | `ARM_CC_EQ` (1) | 0 | 1/1 | `0x1000` | `0x0c` | 0/2 | EQ (1) |
| `0x1002` | `01 20` | `moveq r0, #1` | `ARM_CC_EQ` (1) | 0 | 1/0 | `0x1000` | `0x0c` | 1/2 | EQ (1) |
| `0x1004` | `02 21` | `movne r1, #2` | `ARM_CC_NE` (2) | 0 | 1/0 | `0x1000` | `0x0c` | 2/2 | EQ (1) |
| `0x1006` | `03 22` | `movs r2, #3` | `ARM_CC_AL` (15) | 1 | 0/0 | `0x0` | `0x00` | 0/0 | 0 |

机器生成的完整 TSV 为：

```tsv
address	bytes	size	mnemonic	op_str	cc	update_flags	it_present	it_is_it	it_block_address	it_mask	it_slot_index	it_slot_count	it_first_cc
0x1000	0c bf	2	ite	eq	1	0	1	1	0x1000	0x0c	0	2	1
0x1002	01 20	2	moveq	r0, #1	1	0	1	0	0x1000	0x0c	1	2	1
0x1004	02 21	2	movne	r1, #2	2	0	1	0	0x1000	0x0c	2	2	1
0x1006	03 22	2	movs	r2, #3	15	1	0	0	0x0	0x00	0	0	0
```

## 为什么是 EQ、NE，以及为什么不写标志

- `0c bf` 解码为 `ite eq`。原始四位 mask 是 `0x0c`，其末尾零个数为 2，因此总槽数为 `4 - 2 = 2`。`firstcond` 为 EQ，所以 `first_cc = ARM_CC_EQ`；ITE 的 Else 槽产生相反条件 NE。
- `0x1002` 的 `2001` 是 Thumb-1 窄 `MOVS r0, #1` 编码。离开 IT 块时，这条指令有隐式 S 位，应显示 `movs` 并写 APSR；但它位于 `0x1000` IT 的槽 1（Then/EQ），Thumb IT 规则抑制该隐式标志写入。现有译码流程据此把 S/CPSR 操作数改为空谓词寄存器，所以文本是 `moveq`、`cc=EQ`、`update_flags=false`。
- `0x1004` 的 `2102` 同理是窄 `MOVS r1, #2`，位于槽 2（Else/NE）。它显示为 `movne`、`cc=NE`，并且 `update_flags=false`。
- 两个槽用完后，`0x1006` 的 `2203` 已在 IT 块外。它恢复 Thumb-1 的隐式 S 位规则，因此显示 `movs r2, #3`、`cc=AL`、`update_flags=true`，`it` 字段全零。

`block_address` 和 `first_cc` 始终保存最初 IT 的虚拟地址和首条件；即使剩余条件栈在第二个槽后耗尽，也不会把剩余栈误当成另一个 IT 块。

## 两种读取入口的一致性

启用 `CS_OPT_ARM_IT_CONTEXT` 后，上下文只按虚拟 PC 邻接接续，不比较缓冲区指针：

1. 完整批量：一次 `cs_disasm(..., count=0)` 读取四条；
2. 分块交替：`cs_disasm(..., count=1)` 只读 IT，`cs_disasm_iter` 读第一个 MOV，随后 `cs_disasm` 从 `0x1004` 读完剩余两条。

测试的共享消费者复制地址、字节、助记符、操作数、`cc`、`update_flags` 和全部 `it` 字段，并逐字段比较两份清单；不是只比较返回条数或调用成功。把后续字节复制到另一块缓冲区后，只要 PC 仍为 `0x1002`，来源仍是原 IT 的 `block_address=0x1000`。新 handle 把完整片段基址改为 `0x2000` 时，`block_address` 随输入 PC 变为 `0x2000`，槽位和 EQ/NE 关系不变。

槽位按成功解出的指令推进。测试还用 `0c bf 4f f0 00 01 02 21` 覆盖“16 位 IT + 32 位槽 + 16 位槽”：空输入以及只提供 32 位指令前两个字节时，`cs_disasm_iter` 均失败，且 `code/size/address` 游标不变；在同一 PC `0x4002` 补齐为 `4f f0 00 01` 后，仍得到槽 1、EQ、不写标志。skipdata 真正产生 `.byte` 数据项时会清空 IT 关联；数据不占槽，下一条指令按块外 `AL`/`movs` 处理。

## PC 断开后旧来源消失

实际测试先在 `0x1000` 解出 IT，然后把同样的 `02 21 03 22` 放到不相邻的 PC `0x1800` 批量解码。生成的 `analysis/thumb-it-disconnect.tsv` 内容为：

```tsv
address	bytes	size	mnemonic	op_str	cc	update_flags	it_present	it_is_it	it_block_address	it_mask	it_slot_index	it_slot_count	it_first_cc
0x1800	02 21	2	movs	r1, #2	15	1	0	0	0x0	0x00	0	0	0
0x1802	03 22	2	movs	r2, #3	15	1	0	0	0x0	0x00	0	0	0
```

如果 PC 连续到 `0x1004`，第一行应是属于 `0x1000` 槽 2 的 `movne`、NE、不写标志；PC 断开到 `0x1800` 后，上下文从空 IT 状态开始，同一字节恢复为块外 `movs`、AL、写标志，且所有来源字段为零。

## 最小 C 调用示例

```c
#include <capstone/capstone.h>
#include <stdio.h>

int analyze(const uint8_t *code, size_t size, uint64_t pc) {
    csh handle;
    cs_insn *insn;
    size_t count, i;

    if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &handle) != CS_ERR_OK)
        return 1;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    cs_option(handle, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON);

    count = cs_disasm(handle, code, size, pc, 0, &insn);
    for (i = 0; i < count; ++i) {
        cs_arm *arm = &insn[i].detail->arm;
        printf("0x%llx %s %s cc=%u flags=%u it=%d/%d block=0x%llx slot=%u/%u\n",
            (unsigned long long)insn[i].address,
            insn[i].mnemonic, insn[i].op_str,
            arm->cc, arm->update_flags,
            arm->it.present, arm->it.is_it,
            (unsigned long long)arm->it.block_address,
            arm->it.slot_index, arm->it.slot_count);
    }

    if (count)
        cs_free(insn, count);
    cs_close(&handle);
    return 0;
}
```

## 兼容范围

- `CS_OPT_ARM_IT_CONTEXT` 追加在 `cs_opt_type` 末尾，既有选项编号不变；它接受 `CS_OPT_OFF`（默认）和 `CS_OPT_ON`。
- 默认关闭时保留旧行为：`cs_disasm` 入口仍清空本轮 ARM IT 状态，`cs_disasm_iter` 仍保留 handle 内既有状态；新 `cs_arm.it` 字段为零。
- 开启后仅改变 ARM/Thumb 的跨入口 IT 接续和来源提交；没有新增第二套指令解码器，条件、`update_flags` 和文本仍由原 ARM 译码/打印流程产生。
- 上下文保存在各自 `csh` handle 中；小端和 `CS_MODE_BIG_ENDIAN` Thumb 均按同一 PC 规则工作。
- 成功切换 ARM/Thumb 模式，或改变该开关的值，会清空条件栈、PC 邻接游标和 IT 来源；重复设置相同 ON/OFF/模式值不会打断连续流。
- `cs_arm.it` 追加在 `cs_arm` 末尾。源码 API 向后兼容，但公共结构体大小因此变化，已编译的二进制/绑定应使用更新后的头文件重新编译。
- 该机制只恢复已有指令的 IT 控制关系和槽位来源，不计算执行后的寄存器值，也不扩展符号执行或控制流图。
