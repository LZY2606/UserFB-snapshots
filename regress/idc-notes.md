# IDC 回归说明：最小反例与三类退化的首个差异

本文件配合 `regress/target.cpp`（模型测试）与 `regress/idc_mutants/`（语义变异）
使用。所有 XML/schema 均由代码生成或写在本目录 fixture 中，不访问网络、不用
外部实体。固定种子 1383。

## 数据模型与判定依据

- 文档 = 若干 `batch`；每个 batch 内有 `rec`（记录）与 `ref`（引用）。
- 记录身份 = 二元组 `(a, b)`：`a` 为 `xs:integer`，`b` 为 `xs:token`。
- 预期结论只在模型上计算：整数值 + token 空白折叠后的值；不比较 XML 原文，
  不用 libxml2 的类型转换函数。
- 每个 batch 是独立的约束实例：同一键可跨 batch 重复，引用不能向兄弟
  batch 借键。
- `xs:key`：字段缺失即无效；完整元组重复无效。
- `xs:unique`：排除不完整元组；完整元组重复无效。
- `xs:keyref`：完整元组必须命中本作用域目标表；字段不完整的引用不参与匹配。

## 最小反例（与 `regress/idc_mutants/driver.c` 相同）

schema（selector/field 前缀固定为 `t`，约束挂在 `batch` 上）：

```xml
<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'
 xmlns:t='urn:idc:Z1383' targetNamespace='urn:idc:Z1383'
 elementFormDefault='qualified'>
<xs:element name='collection'><xs:complexType><xs:sequence>
<xs:element name='batch' maxOccurs='unbounded'><xs:complexType>
<xs:choice minOccurs='0' maxOccurs='unbounded'>
<xs:element name='rec'><xs:complexType><xs:sequence>
<xs:element name='a' type='xs:integer' minOccurs='0'/>
<xs:element name='b' type='xs:token' minOccurs='0'/>
</xs:sequence></xs:complexType></xs:element>
<xs:element name='ref'><xs:complexType><xs:sequence>
<xs:element name='a' type='xs:integer' minOccurs='0'/>
<xs:element name='b' type='xs:token' minOccurs='0'/>
</xs:sequence></xs:complexType></xs:element>
</xs:choice></xs:complexType>
<xs:key name='dk'><xs:selector xpath='t:rec'/><xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:key>
<xs:unique name='du'><xs:selector xpath='t:rec'/><xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:unique>
<xs:keyref name='dkr' refer='t:dk'><xs:selector xpath='t:ref'/><xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:keyref>
</xs:element></xs:sequence></xs:complexType></xs:element></xs:schema>
```

三个最小反例文档（正常源码下均应判无效，首个结构化错误为
`XML_SCHEMAV_CVC_IDC`（1877），domain `XML_FROM_SCHEMASV`）：

1. 重复键（同一元组的两种合法写法：整数 `1`/`+01`，token `k`/`  k `）：

```xml
<collection xmlns='urn:idc:Z1383'><batch>
<rec><a>1</a><b>k</b></rec><rec><a>+01</a><b>  k </b></rec>
</batch></collection>
```

2. 缺字段的 key（第二条 `rec` 缺 `<b>`；unique 会排除它，key 不会）：

```xml
<collection xmlns='urn:idc:Z1383'><batch>
<rec><a>1</a><b>x</b></rec><rec><a>2</a></rec>
<ref><a>1</a><b>x</b></ref></batch></collection>
```

3. 孤立 keyref（`(7,"q")` 不在本 batch 的键表里）：

```xml
<collection xmlns='urn:idc:Z1383'><batch>
<rec><a>1</a><b>k</b></rec><ref><a>7</a><b>q</b></ref>
</batch></collection>
```

## 三类退化及其首个差异

变异在临时源码副本中由 `regress/idc_mutants/*.patch` 注入，正常源码不打补丁。
每个版本都必须编译成功、正常数据仍通过；编译失败不算发现问题。
驱动按 normal → dupkey → keymissing → keyref 的顺序、先 DOM 后流式验证，
因此每类退化的**首个差异**就是它目标探针在 DOM 入口的结论翻转：

| 退化 | 补丁位置（xmlschemas.c） | 语义 | 首个差异 |
|---|---|---|---|
| 重复键漏报 `dupkey` | `xmlSchemaXPathProcessHistory` 中 `if (e)` 重复键序列报错块改为 `if (0)` | 重复完整元组不再报错 | 探针 dupkey 的 DOM 入口：期望 `ret>0`（`XML_SCHEMAV_CVC_IDC`），实测 `ret=0` |
| 缺字段 key 按 unique 放行 `keymissing` | 同函数中 key 缺字段 `goto selector_key_error` 改为 `goto selector_leave` | key 对缺字段目标像 unique 一样跳过 | 探针 keymissing 的 DOM 入口：期望 `ret>0`，实测 `ret=0` |
| 孤立 keyref 放行 `keyref` | `xmlSchemaCheckCVCIDCKeyRef` 中 `if (res == 0)` 无匹配报错块改为 `if (0)` | 孤立引用不再报错 | 探针 keyref 的 DOM 入口：期望 `ret>0`，实测 `ret=0` |

三类变异下，流式入口（`xmlSchemaValidateStream`）与 DOM 入口给出相同的
（错误）结论，即偏差是语义层面的，与读取分块方式无关；非目标探针仍然
判无效（无过度放行），正常文档仍然判有效。

## 错误类别约定

- **身份约束无效**：domain = `XML_FROM_SCHEMASV` 且 code = `XML_SCHEMAV_CVC_IDC`。
- **XML/schema 不能解析**：domain = `XML_FROM_PARSER`（或 schema 解析失败）。
- **内部/API 错误**：验证入口返回负值，或出现以上两类之外的结构化错误。

后两类出现时测试直接失败，绝不允许冒充模型预期的无效数据：模型预期无效时，
必须观察到且仅观察到 IDC 类错误。
