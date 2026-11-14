# 正文流接口约定

## 公开接口和批次

goquery 增加 `StreamBody(r io.Reader, yield func(*Document) error, opts ...html.ParseOption) (BodyStreamStats, error)`。
随附的 html 包增加底层 `ParseBodyStream(r io.Reader, yield func(*Node) error, opts ...ParseOption) (BodyStreamStats, error)`；回调 Node 是下述独立文档根。
两个包的 `BodyStreamStats` 可以用类型别名共享，公开 int 字段 `Batches`、`Nodes`、`PeakPendingNodes`。

每个非空批次是一份新的 DocumentNode/html/head/body 骨架，head 为空，骨架没有原页属性；body 下是本次移交的正文直接孩子。原页的 head、doctype、html/body 属性不在采集范围。正文直接孩子包括元素、文字和注释；子树内部保留实际 HTML5 构树结果及节点身份，移交原节点，不重新解析或克隆出另一份作为交付物。

yield 同步调用；一批完成后才继续消费下一 token。批次从活动正文中移出，不能仍挂在原 body 下。保存的批次文档可以照常查询、移动或改写；后续输入不得改变旧批次，回调里的改写也不得影响剩余构树。goquery 批次保存本次 ParseOptions，使后续 AppendHtml/SetHtml 使用同一选项。普通 NewDocumentFromReader、Parse、ParseFragment 及其选项入口保持行为不变，不启用流式释放。

## 安全前沿

每消费完一个真实 token（含它触发的隐式 token 与重处理），在读取下一 token 前检查以下条件：

- 开放元素栈恰为 HTML 命名空间的 html、body；活动格式元素列表为空，form pointer 为空，template 模式栈为空；framesetOK 已为 false。
- 没有正在执行的 foster 插入或尚未完成的 token 重处理。

满足时移交当前 body 的完整直接孩子前缀，但保留最后一个直接 TextNode（如果有），因为下一段文字仍可能与它合并。可移交前缀为空则不回调。边界未满足时可以等待，不需要猜测其中某个子树是否已经稳定。此规则刻意保守；不提供 CSS 稳定选择、片段解析、浏览器执行或 shadow DOM 扩展。

真正 EOF 要先完成原 HTML5 EOF 容错处理，再交付所有余下正文孩子，包括未闭合元素和尾文字，不再要求上述栈条件。frameset 文档没有 body 时不产生批次。把各批 body 的孩子按交付顺序接起来，在调用者不修改批次时，节点类型、属性、文字和子树形状应等同于一次普通整页解析后的 body 孩子序列；批次骨架不参与比较。保留 HTML5 的隐式段落闭合、表格 foster parenting、活动格式元素重建、template 和 form 规则，不能拿源码标签嵌套代替真实 DOM。

移交前文本必须完整，不能到 EOF 才补写；移交后解析器的文本缓冲、活动树、栈和其他保存节点的状态不得继续持有已移交子树。连续独立记录的流，解析器保留的节点及文本不能随已交付记录总数增长；未达安全前沿的单个大区域仍可能很大，不承诺每页固定内存。不得保存整页原始输入、建立第二棵全页树、反复重读或单独解析每个截取片段。

`Batches` 是已经调用回调的批次数，包括回调报错的那批；`Nodes` 是这些批次中所有正文节点的累计数（递归计数，含文字/注释，不含每批骨架）。`PeakPendingNodes` 是每个真实 token 处理完成、释放之前的活动 body 后代节点数的最大值；EOF 的最终处理也计一次。统计必须来自实际构树和释放，不能按输入长度或标签数估算。长流测试不要用机器内存阈值代替此精确计数。

## 读取与失败

nil Reader 或 nil yield 在读取前返回非 nil 错误。正常返回 nil error；Reader 的非 EOF 错误和回调错误原对象返回。已经交付的批次和统计保留，但非 EOF 故障不得触发最终强制释放，也不能随后继续读或继续回调。Reader 单次返回 `(n > 0, err)` 时，先按 tokenizer 既有语义消费这 n 个字节中的完整 token，再观察 err；不丢掉它们，也不把不完整 token 当成 EOF 补全。回调失败后不再调用 Reader.Read；已经发生的 tokenizer 有限预读允许存在。Reader 的关闭责任仍归调用者，函数不关闭它。

## Go 回归和运行

在根目录新增 `body_stream_contract_test.go`，同 goquery 包，四个顶层测试名是 `TestBodyStreamFrontier`、`TestBodyStreamReclaim`、`TestBodyStreamErrors`、`TestBodyStreamParity`。每组通过 `task_support/taskcheck.Run(t, func(c *taskcheck.C){...})` 执行，并用 `c.Check` 写真实断言；Go 原生 testing 的失败信息照常保留。

Frontier 用带读入闸门的 Reader 证明 `<body><p>A</p>` 已经交付才允许读取后缀，并组合尾文字合并、表格外移文字、错配 b/i、模板和 form 边界。Reader 可以每次只交一个字节或一个明确分片，不靠 sleep；不能用完整字符串 Reader 加一次最终结果比较冒充提前交付。

Reclaim 用足够多独立 `<p>x</p>` 记录证明输出计数正确且 PeakPendingNodes 保持小常量，不随记录数增长；另保留首批，修改它再继续解析，核对双方隔离。还需在 `vendor/golang.org/x/net/html/body_stream_retention_test.go` 新增同包的 `TestBodyStreamRetention`，用 ParseOption 捕获真实 parser，在批次回调时检查已移交子树不再由活动树、延迟文字和保存节点的状态持有。此组同样用 taskcheck.Run/Check；不要只检查返回统计，或用 GC 调度时机猜测释放。

Errors 覆盖普通错误、同读返回字节加错误、回调错误原对象/不继续读取、不关闭 Reader、真实 EOF 尾部交付及新调用隔离。Parity 用独立写出的结构期望和普通 Parse 对照检查分片输入，不只对比两个可能同错的调用；至少保留表格、格式重建和未闭合模板的结构断言，并检查非默认 scripting 选项及批次后续 AppendHtml 的选项继承。

`bash task_support/stream.sh target` 编译当前源码、运行上述五组非空断言及相关原测试。缺文件、空组、skip、FailNow 或异常退出不当作完成。`baseline` 只展示原整页行为，不是新接口的完成条件。保留原测试与许可证，补 API 文档和一个可运行的 Go 示例；禁止 Python 实现、测试和示例。
