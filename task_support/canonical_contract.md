新增 crawler.WithCanonicalLinks(bool) Option，默认 false；不新增 CLI 参数。false 时保持原 Run、ExtractHTML 和 HTTP Get/Head 的公开调用方式与原语义。true 的语义如下。

URL 身份按 net/url 解析：只接受 http/https，Scheme 与 Hostname 转小写，去掉默认端口和 fragment，空 Path 改为 /；保留查询及顺序、路径大小写和转义形式，不去掉尾斜杠、不任意解码。userinfo 不允许成为 canonical 目标。origin 按正规化后的 scheme/hostname/effective port 比较。已有 cleanURL、可抓取范围、robots、深度、Ignored 和 Dirs 约束仍用于普通发现，别名绝不能扩大准入范围。

只从成功的 2xx HTML GET 响应接收 canonical；HEAD 的声明、错误页和非 HTML 正文不产生别名。要用实际 GET 最终 Request.URL 作为文档地址，不能猜 Location 或用最初请求地址替代。现有 HTTP.Get/Head 签名继续能用，内部可扩展响应元数据传递方式。对重定向只记录请求起点→最终地址，不要求记录中间跳；两端必须同 origin 才记录这条边。

HTML head 按真实 HTML 结构解释（可以复用随附的 x/net/html）；template 内容、注释、script/style 字面量及 body 内的伪声明不算。head 第一个带 href 的 base 决定解析基址：相对最终文档地址展开，空值或无效/非HTTP值回退最终地址，后续 base 不接管。head 的 link 按 rel 的 ASCII 空白 token、不区分大小写识别 canonical。从头选第一个 href 非空、可解析成HTTP且与最终文档同origin的 canonical，其他候选跳过；声明不受标签过滤器影响。所有 head canonical link 都作为控制信息，不作为普通链接输出或抓取。一般链接仍受原标签过滤，开启时相对链接按这份有效基址解析。ExtractHTML 的既有独立调用保留默认旧行为。

记录有效的最终文档地址→canonical目标边。不因声明本身新增HTTP工作；声明和重定向建立的是已观察URL之间的身份关系。一个URL在本轮响应固定，相同URL不会产生相互矛盾的canonical声明。普通 URL 发现保留为输出候选，初始种子不自动加入候选。最终代表沿有向边走到无出边终点；遇环则取环内正规化URL按字节字典序最小者，所有流向此环的前驱也用它。自己指向自己是长度1的环。重定向边与canonical边按实际文档身份串接；若同一源同时有重定向最终边与文档声明，重定向描述的是起点→最终，文档声明描述最终→目标，不得用其中一个覆盖另一条。

每当调度器收到别名声明，它都要立即影响后续的普通发现去重。若新发现所属的已知无向等价组已有某个URL被派发，则不再为这个新发现派发HEAD/GET；但仍记录输出候选。只在执行原准入检查并成功入队后记派发；仅输出的资源不能占掉稍后正常a/iframe发现的抓取机会。以调度器收到声明/发现事件的先后决定“已知”，无需追溯取消已经入队或在途的工作。已有任务仍可提供进一步别名和出链，并且各自只结算一次；一次归并不能吞掉任何未完成任务的计数。过滤器、目录策略和只输出资源路径仍然有效。

true 时普通 URL 回调延至本轮所有派发工作及其发现事件处理完以后，再把每个候选映射为最终代表，去重并按绝对URL字节字典序发布，每个结果一次。代表即使只是canonical目标而未请求也可以输出。回调结束后Run才能返回；不得因原有seen或定时发送丢掉别名/候选。失败请求不凭空产生canonical边。下一次顺序调用同一Crawler.Run必须重新建立本轮所有身份、调度和输出状态；不支持同实例并发Run。此模式不引入重试、持久缓存或可撤销回调。

新增 internal/crawler/canonical_target_test.go，顶层 TestCanonicalHead、TestCanonicalGraph、TestCanonicalLifecycle，各组用 task_support/taskcheck.Run(t, func(c *taskcheck.C){...}) 和 c.Check 做真实断言。用本地 httptest 与通道安排迟到声明和在途任务，检查实际HEAD/GET、输出内容/顺序和Run结束点，不能只调用一个新归并函数。保留旧测试，补默认关闭的对照，以及连续Run、同origin重定向后的相对canonical、链/环、标签过滤和仅输出资源先发现的组合。go test 自动编译当前internal生产包的所有Go文件；runner要求三组正常返回并实际执行断言，缺文件、零用例、全skip或提前进程退出均不成功。可用测试超时作为失败上界，不用sleep决定交错。
