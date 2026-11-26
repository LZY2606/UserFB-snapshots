# 数值子表达式共享与临时值计划

这次优化不改变 Kafel 策略语法或公开 API，也不在宿主机上解释规则。
`src/value_plan.c` / `src/value_plan.h` 把同一个比较谓词、同一个 32 位字宽内的
`变量`、`常量`、`&`、`|` 表达式规范化成 DAG，并预先规划 BPF 暂存槽；
`src/codegen.c` 只负责把计划步骤翻译成 classic BPF 指令。

## 值的身份

每个规范化值的身份可以理解为：

```c
struct value_identity {
    int kind;   /* VP_NODE_VAR, VP_NODE_CONST or VP_NODE_OP */

    /* VP_NODE_VAR: 参数下标、来源宽度和高/低字共同决定身份。 */
    int arg;    /* syscall argument index, 0..5 */
    int size;   /* resolver-declared source size, e.g. 4 or 8 */
    int word;   /* VALUE_PLAN_WORD_HIGH or VALUE_PLAN_WORD_LOW */

    uint32_t constant_value; /* VP_NODE_CONST: word-local 32-bit value */

    /* VP_NODE_OP: operation and canonical input identities. */
    int op;     /* EXPR_BIT_AND or EXPR_BIT_OR */
    int left;
    int right;
};
```

因此：

- 参数 `a` 和参数 `b` 不会共享；
- 同一个参数下的 32 位来源和 64 位来源不会混成一个值；
- 高字和低字是不同身份，加载偏移也由 `word` 和目标字节序决定；
- 常量按当前字宽的 32 位值编号；
- `&`、`|` 的两个输入会按语义身份排序后再驻留，所以 `x & y` 与 `y & x`
  得到同一个规范节点。

哈希只用于加速查找；哈希相同仍会逐项比较完整身份，不会仅凭哈希合并。

## 存活范围和槽位

计划从比较左右两侧的根节点开始统计 DAG 边引用数。每消费一次节点值，就减少
一个引用：

- 第一次消费时真实计算该节点；
- 如果之后还有引用，就把 A 中的结果存入一个工作槽；
- 后续消费生成 `BPF_LD | BPF_MEM`；
- 最后一次消费后释放该槽，槽位可被其他值复用。

二元运算的右值还会复制到该运算自己的溢出槽。这个槽在计算左输入期间始终保持
分配，因此左子树即使耗尽某个共享值的引用并复用其规范槽，也不会覆盖当前运算
需要的右值。

完整的寄存器—寄存器比较使用两个固定结果槽：

```c
#define VALUE_PLAN_LEFT_SLOT   14
#define VALUE_PLAN_RIGHT_SLOT  15
#define VALUE_PLAN_WORK_SLOTS  14  /* slots 0..13 */
```

容量判据是：

```c
max_simultaneously_live_work_slots + 2 <= BPF_MEMWORDS
```

也就是完整 A/X 比较最多同时使用 14 个工作槽，再加上左右结果槽 14、15。
常量右值的 K 形式不需要右结果内存槽；当前实现仍使用槽 14 作为左结果槽，
工作槽仍为 0..13。计划器无法满足槽位分配时把 `feasible` 置为 false，
`codegen.c` 随即对该比较使用原来的树式发射。不会扩大 `BPF_MEMWORDS`，
也不会跳过边界检查。

计划只覆盖一个字宽上的一次 `generate_cmp32` 全比较（包含左右两棵表达式）。
64 位比较的高字计划和低字计划分开；`&&`、`||`、`!` 分隔的短路分支也不共享
存活值。因此不会从另一条执行路径或另一个字宽读取尚未写入的槽位。

## 反向发射时的依赖顺序

`value_plan` 的 `steps` 按普通执行顺序保存，例如：

```c
/* Conceptual forward steps for (a & b) | c. */
load_arg_a;
load_arg_b;
and_x;
store_shared_slot;
load_arg_c;
or_x;
```

Kafel codegen 的主缓冲在最终整体反转之前按“反向执行顺序”追加指令。
因此 `generate_value_plan()` 从计划最后一步向第一步遍历并调用
`ADD_INSTR()`。全局 `reverse_instruction_buffer()` 后，BPF 中的实际顺序
仍然是计划的前向顺序，然后才是已经先追加的比较跳转。

引用计数保证依赖值先计算、后消费；父运算的右值溢出槽在左子树完成前不释放，
所以反向接入现有 codegen 不会破坏依赖顺序。

## 测试

`regress/target.c` 提供五组检查：

- `sharing`：六层 repeated fixture、1..6 层、交换操作数、跨左右公共子表达式，
  以及随机生成的相似/不等价表达式；
- `words`：参数下标、来源宽度、高/低字偏移和无符号比较；
- `branches`：`&&`、`||`、`!`、动作顺序和错误架构；
- `storage`：所有暂存下标小于 16，并在可达 BPF CFG 上做 must-write 数据流检查；
- `reuse`：同一个 `kafel_ctxt` 交替编译不同策略和目标架构。

测试用普通 C `uint64_t` 运算作为独立参照，并把边界值及固定种子样本的完整
32 位动作码与 `test/runner/interpreter.c` 的解释结果比较。
