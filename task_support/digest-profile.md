# 离线 Digest 协议范围

来源：[RFC 7616](https://www.rfc-editor.org/rfc/rfc7616.html)，重点为 3.3、3.4.1–3.4.3、3.4.5。以下是本次客户端所需的范围；测试只使用良性自建端点及虚构凭据，不连接第三方认证服务。

令 H(x) 为 x 字节的 SHA-256，表示成小写十六进制。冒号表示一个 ASCII 冒号，拼接公式中的摘要使用十六进制字符串，不使用二进制摘要。username/password 限 ASCII；realm、nonce、opaque 是挑战中解开 quoted-string 后的值。

SHA-256 的 HA1 = H(username:realm:password)。
SHA-256-sess 的 HA1 = H(H(username:realm:password):nonce:cnonce)。

auth 的 HA2 = H(method:uri)。
auth-int 的 HA2 = H(method:uri:H(entity))。
entity 是 Request.GetBody 提供的原始实体字节，不包括传输编码；空实体使用空字节串的摘要。uri 是当次实际 URL 的转义路径与原始查询，即 URL.RequestURI()，空路径按 /。

response = H(HA1:nonce:nc:cnonce:qop:HA2)。

nc 是从 00000001 开始的八位十六进制无符号计数。相同 origin、username、realm、规范化 algorithm、nonce 的会话共享一个随机 cnonce 和计数；即使认证发送失败也不回收已分配的计数。不同 nonce 独立保存，迟到的旧 nonce 不能清空或重置其他会话。

生成的 Authorization 方案为 Digest。username、realm、nonce、uri、response、cnonce、opaque 使用 quoted-string，内部双引号和反斜杠要转义；algorithm、qop、nc 不加引号。opaque 仅在挑战存在时携带。方案、参数名、algorithm、qop 和 stale 值按大小写不敏感识别，值中其他字节不作大小写转换。

可用挑战必须有且只有一份 realm、nonce、algorithm、qop，nonce 非空，算法只接受 SHA-256 或 SHA-256-sess。qop 必须包含 auth 或 auth-int，优先 auth-int。按字段及其内部挑战的出现顺序选择第一份可用挑战。重复已识别参数使该份挑战失效；其他独立挑战仍可选择。Basic、未知算法、算法缺省为 MD5、未知 qop、userhash=true、未知 charset 均不在此范围。charset 缺省或 UTF-8 可接受；不实现无 qop 的旧式计算、userhash、domain 预认证或 Authentication-Info/rspauth。

每个实际请求先不带库生成的认证。第一份有效挑战允许一次认证发送；下一份 401 只有 stale=true 且 nonce 变化才允许第二次认证发送。后续不再认证。配置来源以请求 URL 判断，不能用 Host 头替代；自带 Authorization 的请求整段绕过自动认证。

替换中间 401 前关闭其 Body；若开始认证后不能重放实体或读取失败，关闭中间响应并返回错误。最终响应由现有 backend 消费和关闭。重放、实体哈希使用的 reader 都拥有独立生命周期。新模式通过 Client.Jar 管理 Cookie，不支持调用方手写 Cookie 头；中间 401 的 Cookie 更新后重试应查询当前 URL 的 Jar，不能直接复制旧 Cookie 值。
