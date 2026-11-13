# 声明式影子根（Declarative Shadow Roots）采集指南

资料页上的 Web Component 会直接输出 `<template shadowrootmode="open">`。默认情况下 jsoup 把它当作普通
`template`，宿主的 light DOM 和组件内部字段会混进同一棵子树。打开 `Parser.setDeclarativeShadowRoots(true)`
之后，HTML 构树时会把第一个有效的、直属的开放声明模板构造成独立的 `ShadowRoot`，宿主普通内容和组件内部
字段可以分开读取。XML 解析与未打开开关的解析行为完全不变。

## 打开开关

```java
Parser parser = Parser.htmlParser().setDeclarativeShadowRoots(true); // 返回 Parser，可链式调用
Document doc = Jsoup.parse(html, baseUri, parser);
```

- 默认关闭；`Parser.isDeclarativeShadowRoots()` 可读当前配置。
- `parser.newInstance()` 与 `parser.clone()` 会复制该配置；每次 parse 的运行状态互相独立，
  同一个 Parser 先后解析不同文档，根不会串。
- `StreamParser` 传入同一个 Parser 即生效：解析过程中根就建好，iterator / stream / selectNext
  的普通候选不会交出根里的元素；返回一个已完成的宿主时，它的根已经完整可读。

## 读取模型

```java
Element host = doc.selectFirst("#profile");
ShadowRoot root = host.shadowRoot();   // 没有根时返回 null
root.host();                           // 指回宿主
root.select(".name");                  // 根内可用现有 select / text / childNodes 等接口
```

- 根不占宿主的 `childNodes()` / `children()`，也不占兄弟序号；宿主的 `select` / `text` / 遍历不会穿进根里。
- 根内查询同样看不到宿主外面的祖先（根的 `parent()` 为 `null`）。
- 根里的相对链接继承宿主的 `baseUri()`，`ownerDocument()` 指向所属文档；宿主被移动后两者跟着宿主走。
- 能附着根的宿主：HTML 命名空间下的 `div`、`span`，以及带连字符的自定义元素（如 `<my-el>`）。
  同一宿主只有第一个有效的直属开放声明模板会建根；后续声明、`closed` 或其他 mode、非法宿主、
  以及普通 `template` 里面的声明，都仍然按普通 `template` 保存。影子内容里还可以再有自己的宿主和开放根。

## 克隆与序列化

- `Element.clone()` / `Document.clone()` 会深复制附着的根，并把副本根的 `host()` 指到对应的副本上，
  改副本不影响原页。
- 普通 `outerHtml()` 只输出 light DOM。
- `Element.outerHtmlWithShadowRoots()` 在每个宿主的开标签后面，先输出一个代表根的
  `<template shadowrootmode="open">`，再输出普通孩子；影子内容和嵌套模板只输出一次。
  把这份输出用打开开关的 Parser 重新解析，可以恢复出同样的根层级。

本次只实现独立的树模型：slot 分配、CSS 布局、脚本执行都不在范围内。

完整可运行示例见 [ShadowRootExample.java](ShadowRootExample.java)，回归用例见
[ShadowRootRegression.java](ShadowRootRegression.java)（`sh support/run.sh target`）。
