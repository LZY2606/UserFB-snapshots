这里约定新增的公开 API，类型放在 selenium 包，不给 WebDriver 接口增加方法。

```go
func OpenBiDi(ctx context.Context, wd WebDriver) (*BiDiClient, error)
func (c *BiDiClient) EvaluateObject(ctx context.Context, contextID, expression string) (*BiDiObject, error)
func (o *BiDiObject) Call(ctx context.Context, declaration string, args []json.RawMessage) (json.RawMessage, error)
func (o *BiDiObject) Release(ctx context.Context) error
func (o *BiDiObject) Realm() string
func (o *BiDiObject) Handle() string
func (c *BiDiClient) Close() error
```

NewRemote 的创建请求允许 webSocketUrl:true；仅采用该次成功创建响应 value.capabilities.webSocketUrl 的字符串端点，不补发 GET capabilities。没有端点、端点不是 ws/wss URL 或传入不支持的 WebDriver 实现时返回 ErrBiDiUnavailable。调用者只在首次创建的当前会话上打开一次 BiDi，不要求扩展 SwitchSession、多次 NewSession 或多个 BiDi 连接的管理。原来的 HTTP 请求形状、接口和默认行为继续兼容，输入能力 map 不修改。

OpenBiDi 建立 WebSocket，启动接收，并发送 session.subscribe，events 为 ["script.realmDestroyed"]，等成功回复才交付客户端；订阅确认前也可能收到销毁事件，不能丢弃。失败就关闭这条侧连接。未知事件忽略。每个命令发唯一 id，回复按顶层 type 和 id 关联；不同对象可以同时在途，先发者不必先完成。取消等待不重放命令、不复用 id，迟到回复不重新结算。传输关闭、非法 JSON 或客户端 Close 结束所有等待。Close 幂等，后续方法返回 ErrBiDiClosed；它不发 session.end 或 WebDriver.Quit。ctx 结束应能结束对应等待。

EvaluateObject 发 script.evaluate，target 为 {"context":contextID}，awaitPromise:true、resultOwnership:"root"。成功的脚本结果内必须有非空 realm 和 result.handle，才交付对象；没有 handle 返回 ErrBiDiNotObject。对象记住该客户端、realm、handle，不能把旧 HTTP 元素 ID 当成 BiDi handle。

Call 发 script.callFunction，target 为 {"realm":对象的realm}，this 为 {"handle":对象的handle}，functionDeclaration 为 declaration，arguments 原样采用 args 中的 BiDi LocalValue，awaitPromise:true、resultOwnership:"none"。返回脚本成功结果的 result RemoteValue JSON，不展开为普通 Go float64/map；bigint、非有限 number、节点 sharedId 等标签和内容保持。顶层 success 内的脚本结果若 type:"exception"，返回 *BiDiScriptError{Realm string, Details json.RawMessage}，Details 保存 exceptionDetails；顶层 type:"error" 则返回 *BiDiProtocolError{Code, Message, Stacktrace string}。二者可用 errors.As 获取，普通结果对象含 error 属性也不能误判。

同一对象的 Call 与 Release 串行，不同对象互不阻塞。Release 发 script.disown，target 使用 realm，handles 仅含自身 handle；成功才将对象标为已释放。重复成功释放直接返回 nil，之后 Call 本地返回 ErrBiDiObjectReleased。释放被协议拒绝时保留对象可调用和再次释放的能力，不自动重发。

script.realmDestroyed 按入站消息顺序记下失效 realm，使其已发布对象和仍在等回复的 Call/Release 返回 ErrBiDiRealmDestroyed。别的 realm 继续工作；旧对象以后不再发命令。EvaluateObject 的 realm 在回复之前未知：若该 realm 已先销毁，迟到成功回复不能发布对象。接收循环不能因为等待对象锁或同步等另一条命令而停住，否则销毁通知将无法结束在途调用。已经按入站顺序处理完成的回复不追溯改判。

这里只管理客户端已经交付的对象；ctx 取消不保证浏览器停止执行，取消求值后尚未交付的远端结果回收、全量 RemoteValue 转换、其他 BiDi 模块均不扩展。标准字段参考 W3C 2026-11-13 工作草案的 script.evaluate、script.callFunction、script.disown 和 script.realmDestroyed：
https://www.w3.org/TR/2026/WD-webdriver-bidi-20261113/#module-script
