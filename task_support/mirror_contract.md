镜像抓取的接口约定

Request 新增公开 MirrorURLs []string 与 MirrorDelay time.Duration，现有构造函数参数不变。镜像仅由非空列表显式开启；保留列表顺序，按 URL 原字符串去掉重复项及主地址。开启后所有地址必须为绝对 http/https URL，MirrorDelay 不能为负，method 必须是 GET 且 Postdata 为空；配置错误时任何地址都不发送。RespType 只支持原有 html/json/jsonp/text。

主请求实际发送后计时，到期开放剩余地址；主请求失败可提前开放。请求因并发额度而等待时尚未发送。多个镜像可以同时竞争额度。对于完全同时完成的两个有效候选不规定谁胜出。主地址和每个去重后的镜像在一次 Download 内至多尝试一次，重定向仍走现有 net/http 规则；同一条重定向链始终占一个物理名额。全失败返回 IsSucc()==false 的 Page，Errormsg 非空且提供候选地址与失败原因。不在候选层自动重试，Spider 原有三次 Download 继续适用。

候选必须是完整成功的 HTTP 表示：2xx、正文完整、现有字符集和 gzip 路径正确处理、RespType 解析成功。读取/解压错误必须判失败，即使已收到的前缀刚好是合法 JSON。不得用 HEAD 替代 GET 或在收到首部时胜出。输掉的 Page、正文和副作用不得进入处理器。胜者返回前取消未发送候选、关闭已发送候选并等本地任务释放全部物理名额；远端处理器不配合关闭时也须能返回。

HttpDownloader.SetMaxConcurrentRequests(n uint) *HttpDownloader 作用于该实例的所有 Download 调用，包括无镜像请求。0 不设上限，默认 0。配置仅要求在该实例没有活跃调用时进行。Spider.Run 使用其实际 threadnum（未指定时仍为 1）配置具备该方法的下载器；不得把逻辑页面数限制当成物理请求限制，也不要求不具备该扩展的自定义 Downloader 更改接口。

逻辑身份不变：胜者 Page.GetRequest() 返回最初传给 Download 的指针，GetUrlTag()/GetMeta() 的既有取法不变；Meta 作为调用方值保留，不要求深复制任意用户对象。Page.GetFetchedURL() string 为最终成功 response.Request.URL.String()，包括普通下载；失败为空。镜像使用独立的 Header 值切片、Cookie 对象及内部请求配置，原 Request 不因发送 Cookie、重定向或取消而被改写。代理及 CheckRedirect 继续生效；自行使用普通请求路径绕过代理不算镜像支持。普通请求没有镜像时保留已有状态和解析行为。

Spider 只处理一次胜者。用户可用 GetFetchedURL 解析 JSON 中的相对 next 地址，再用 Page.AddTargetRequest 进入现有调度；库无须把所有 AddTargetRequest 自动改为相对链接模式。失败候选不能触发 PageProcesser、Pipeline 或子请求。Finish 等逻辑请求和其实际产生的子请求正常结束再执行。

新测试放在 core/spider/mirror_target_test.go，package spider_test，顶层组名 TestMirrorWinner、TestMirrorPermits、TestMirrorLifecycle。每组用 taskcheck.Run(t, func(c *taskcheck.C) { ... })，在具体的输出、请求次数、事件顺序等内容断言处调用 c.Check(condition, message)。Winner 组包含正文停滞、解析失败、非2xx和读取失败前缀；Permits 组通过服务端闸门和并发计数确认普通/竞速请求共用限额且等待者不越线；Lifecycle 组覆盖胜者实际地址、原请求不变、真实 Spider 的单次处理与发现、全失败重试及后续普通请求能继续用额度。应有一例使用本地转发代理，并检查重定向回调生效；保留原队列对照。

run.sh target 会构建当前 core/spider 和 core/downloader，运行以上三个新组，并运行正常生产基线。未提供目标实现和目标测试。此入口拒绝缺文件、无断言、跳过、提前退出等情况，但其通过只说明新测试实际执行，内容质量仍须审查。
