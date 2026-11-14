Ggr BiDi 协商与代理边界

参考上游 WebDriver BiDi 规范 https://w3c.github.io/webdriver-bidi/ （webSocketUrl capability），Ggr原route/proxy和Host.Sum仍是HTTP行为依据。

1. 只处理原route接受的W3C成功形状：value.sessionId为非空字符串，value.capabilities为对象。缺少webSocketUrl或为null表示不开放入口；旧JSON Wire仍走原逻辑。非空但不是ws/wss绝对URL字符串的能力属于协议错误，返回502且不登记入口；原代码其他会话字段的判断不扩展。
2. 地址必须有host，不能带userinfo或fragment。后端响应给出的地址是协商结果；客户端/bidi路径中的query不覆盖它。创建时保存URL的EscapedPath与RawQuery，不能Decode后再拼接导致%2F变成路径分隔。无路径按URL的通常根路径处理。测试使用无账户的回环地址与不含秘密的查询参数。
3. gateway URL为ws或wss（仅由创建请求的TLS决定），Host来自该请求Host，路径为/bidi/加PathEscape后的完整外部sessionId；不使用X-Forwarded-*。服务端必须注册/bidi/入口。与原devtools路径相同，不新增鉴权策略。
4. 后端ws/wss选择相应明文/TLS连接；测试用httptest.NewTLSServer时可以给已有httpClient配置该测试CA/Transport，禁止关闭TLS验证。保留升级相关请求头和Sec-WebSocket-Protocol协商结果；拒绝握手时保留后端状态，无需把它伪装成JSON协议错误。
5. DELETE退休只看原HTTP代理针对会话根的DELETE且后端HTTP2xx；元素删除、失败状态和传输错误都不退休。先阻止新连接及未完成握手发布，再关闭该记录拥有的流；锁不覆盖实际网络等待。无需解析BiDi消息、重连或命令级重放。
6. 保持原路由前缀、会话ID和HTTP JSON处理。测试可直接用updateQuota改变配置，用后端通道控制101返回与DELETE完成先后；不要求实现完整浏览器。
