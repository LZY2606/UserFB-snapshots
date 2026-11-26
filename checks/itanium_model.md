# Itanium substitution model checks

这些检查只位于测试和测试运行器中：

- `itanium_model_test.go`：独立的小型 C++ 类型模型、Itanium 子集编码器、结构投影和确定性/fuzz 入口。
- `checks/substitution_mutations.sh`：在两个独立临时副本中验证候选登记顺序和 substitution 索引的变异会被同一套语义断言杀死。
- `checks/run_symbols.sh target`：总入口。

运行：

```bash
bash checks/run_symbols.sh target
```

成功时最后一行是：

```text
ITANIUM_SUBSTITUTIONS_OK cases=6 mutations=2
```

`cases=6` 指 `prefix_chain`、`qualified_type`、`templates`、`declarators`、
`candidate_order`、`reuse` 六组；每组固定种子生成 36 个模型，共 216 个可重放案例。

## 检查方式

测试不使用外部 `c++filt` 或 C++ 编译器作为参照，也不把 demangler 的 AST 类型当作
生成器模型。`imModel`/`imType` 是测试内自建的结构模型，并自行打印合法 C++ 声明。

每个模型生成两个完整的普通非模板函数符号：

1. refs 编码：能复用候选时输出 `S..._`；
2. plain 编码：不使用 substitution，所有结构重新展开。

两种编码都检查：

- `ToAST` 后的结构投影是否等于模型的结构投影；
- `ASTToString` 的完整输出是否等于模型独立打印的 demangled 形式；
- `ToString` 的完整输出是否相同。

结构投影保留：

- `Q(scope::name)` 限定层级；
- `Tpl(name;args=[...])` 模板及参数；
- `CV`、`Ptr`、`Lref`、`Rref`、`Array` 包装顺序；
- `Fn(ret=...,args=[...])` 的返回类型和参数表。

普通 mangled 函数名本身不编码顶层函数返回类型，所以顶层投影的返回位置是 `-`；
函数指针中的 `Fn(ret=...)` 会完整检查返回类型，因此“返回 int、参数是 T”和
“返回 T、参数是 int”不会混淆。模型的 `cppDeclaration()` 另外给顶层函数补上返回类型，
用于错误报告和人工核对。

## 使用的 ABI 子集

编码器只实现本任务需要的 Itanium C++ ABI 子集：

- 普通函数符号：`_Z` + `<source-name>` + 参数类型序列；
- source name：`<decimal-length><identifier>`；
- builtin：`i`、`b`、`c`、`d`、`l` 等；
- 三级及以上限定类名：`N1A1B1CE`、`N2ns6detail5CodecE`；
- 类模板类型参数和整数参数：
  - `Box<int>` 编码为 `3BoxIiE`；
  - `Counter<7>` 编码为 `7CounterILi7EE`；
- 模板可以是限定名的最后一层，也可以是中间前缀，例如 `Outer<int>::Box`；
- `K`/`V` cv 限定；同时 const volatile 时按测试选定顺序编码为 `VK`，使库的
  qualifier 前插逻辑得到 `const volatile`；
- `P` 指针、`R` 左值引用、`O` 右值引用；
- 定长数组：`A<decimal-dimension>_<element-type>`；
- 函数类型和函数指针：`F<return-type><arg-types>E`、`PF...E`；
- substitution：`S_`、`S0_`、`S1_`、……、一位 base36 编号（如 `SA_`）和多位编号
  （如 `S14_`）。

编号和候选槽位的关系是：

| 编码 | `seqID` 返回的索引 | 候选槽位 |
|---|---:|---:|
| `S_` | 0 | 0 |
| `S0_` | 1 | 1 |
| `S1_` | 2 | 2 |
| `SA_` | 10 + 1 = 11 | 11 |
| `SZ_` | 35 + 1 = 36 | 36 |
| `S10_` | 1*36 + 0 + 1 = 37 | 37 |
| `S14_` | 1*36 + 4 + 1 = 41 | 41 |

## 候选身份和等价改写记录

编码器维护自己的 `[]imCandidate`。身份是结构化 key，而不是漂亮打印字符串：

- builtin 不自动入表；
- 每个普通限定前缀是独立候选；
- 模板的 plain name/name-prefix 是一个候选；
- 带参数的完整 `Template` 或 template-prefix 是另一个候选；
- cv 层是独立候选；
- pointer/reference 层是独立候选；
- 一次 `S..._` 引用本身不会无条件再登记一个同槽候选。

`imEncoding.ops` 记录每个节点是 `expand-register`、`ref` 还是 `prefix-ref`，
以及它引用/登记的 slot 和实际 `S..._` 编码。refs/plain/selective 编码都从结构模型
重新编码并重新计算后续编号；不存在先做字符串替换再沿用旧编号的路径。

如果检查失败，错误中包含模型 C++ 声明、模型结构投影、两种编码、两张候选表、
改写记录以及 expected/actual 的首个 rune 偏差位置。

## 手算候选表一：共享限定前缀

符号：

```text
_Z1fN1A1B1CENS0_1DE
```

对应：

```cpp
f(A::B::C, A::B::D)
```

第一个参数 `N1A1B1CE` 的登记顺序：

| slot | 编码 | 候选 |
|---:|---|---|
| 0 | `S_` | `A` |
| 1 | `S0_` | `A::B` |
| 2 | `S1_` | `A::B::C` |

第二个参数是 `N S0_ 1D E`。`S0_` 引用 slot 1 的 `A::B`，随后追加 `D`，
外层类型再登记：

| slot | 编码 | 候选 |
|---:|---|---|
| 3 | `S2_` | `A::B::D` |

因此 `NS0_1DE` 表示 `A::B::D`，而不是从 `A::B::C` 再追加 `D`。若限定前缀少登记一次，
`S0_` 会错取 slot 1 中移动后的 `A::B::C`，语义投影立即不符。

## 手算候选表二：模板名、完整模板、cv/pointer 层

符号：

```text
_Z1fN2ns3BoxIiEEPKS1_S3_
```

对应：

```cpp
f(ns::Box<int>, ns::Box<int> const*, ns::Box<int> const*)
```

第一个参数 `N2ns3BoxIiEE`：

1. 解析 `2ns` 后，因后面还有 `3BoxI...`，登记 `ns`；
2. 解析 `3Box` 后、进入 `I...E` 前，登记 plain template name `ns::Box`；
3. 模板参数 `i` 是 builtin，不登记；
4. 完整 nested/template type 返回 `demangleType` 后登记为 `ns::Box<int>`。

| slot | 编码 | 候选 |
|---:|---|---|
| 0 | `S_` | `ns` |
| 1 | `S0_` | `ns::Box`（template name/prefix） |
| 2 | `S1_` | `ns::Box<int>`（完整模板类型） |

第二个参数 `PKS1_` 先写 `P`，递归层写 `K` 并引用 slot 2：

| slot | 编码 | 候选 |
|---:|---|---|
| 3 | `S2_` | `ns::Box<int> const`（cv 层） |
| 4 | `S3_` | `ns::Box<int> const*`（pointer 层） |

第三个参数直接使用 `S3_`，所以两个指针参数的结构投影必须相同。这个表把
template plain name、完整模板类型、cv 层和 pointer 层明确分成四个不同槽位。

## 变异回归

`checks/substitution_mutations.sh` 创建两个独立临时仓库副本：

- `prefix_registration`：把 `prefix()` 中第一个 `Scope` 为 `*Name` 的
  `*Qualified` 前缀跳过一次登记；
- `subst_index`：当 `id > 0` 时执行 `id--`，让非零 substitution 索引取前一个候选。

每个副本都要求：

1. 精确匹配唯一补丁点；
2. `go test -c` 构建成功；
3. 运行 `TestItaniumSubstitutionModel/prefix_chain` 或
   `.../candidate_order`；
4. 测试以普通断言失败退出，并包含
   `ITANIUM_MODEL_SEMANTIC_ASSERTION` 和对应 `--- FAIL:` 子测试行；
5. 输出中不能有 panic、test timeout、signal 等不稳定捕获特征。

成功输出：

```text
KILLED prefix_registration
KILLED subst_index
```

## 未覆盖范围

本入口不要求覆盖：

- 函数模板和 `T_` 模板参数作用域；
- lambda 与 `Z` local-name 作用域；
- module/W 名称；
- `St`、`Sa`、`Sb` 等 ABI 内建特殊 substitution；
- constructor/destructor、operator、表达式和大部分 `X...E` 模板实参；
- pointer-to-member `M...`、pack `J...`、引用折叠和函数 cv/ref 限定；
- 完整 C++ 声明符的所有合法嵌套组合。

数组只覆盖定长数组以及声明符括号绑定所需的少量多维/指针/引用组合；整数模板实参
只使用正的简单 `Li...E` 形式。
