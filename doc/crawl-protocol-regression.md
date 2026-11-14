# 并发协议回归说明（crawl_protocol_target_test.go）

## 旧测试已覆盖与缺失的证据

既有用例（`tbldef_test.go`、`complex_test.go`、`worker_test.go`、
`basetag_test.go` 等）通过 `spyExtender` 统计钩子调用次数，并单独核对了
重定向上下文（`TestRedirectURLContext`、`TestCloneForRedirect`）与 HTML
base 标签（`TestHandleBaseTag`、`TestBaseTag`）。它们在**单主机、顺序**
视角上很扎实，但缺少以下“交错（interleaving）”方面的证据：

1. **跨主机并发推进没有证据。** 旧用例只断言聚合的钩子总数，无法证明当
   一个主机的 `Visit`/正文读取被卡住时，另一个主机的 worker 仍在独立前进。
2. **响应正文缺少可跟踪的生命周期核对。** 没有任何用例用真实 `io.Reader`
   跟踪每个响应体，并断言 `Run` 返回前每个 Body 恰好被关闭一次；也没有
   校验含二进制字节的正文身份（哈希）在读出、解析后保持不变。
3. **准入结果没有被区分建模。** “已入队”“被 robots 拒绝”“HEAD 后被
   `RequestGet` 否决”“真正完成访问”在旧计数里容易被混为一个总数，缺少
   带 URL、方法、来源、`State`、正文身份的事件多重集与因果先后核对。
4. **真实跨站重定向下的来源/State 传递缺少端到端证据。** 旧用例直接构造
   上下文或在单主机内重定向，没有经过 `DefaultExtender.Fetch` →
   `HttpClient.Do` → 3xx → `ErrEnqueueRedirect` →
   `cloneForRedirect` 的完整生产路径来核对跨站来源与 `State` 继承。
5. **同一 `Crawler` 连续两轮运行缺少独立重算。** 没有证据表明第二轮不会
   继承第一轮的去重表与 robots 结论，且来源与 `State` 都属于当前轮。
6. **`MaxVisits` 的计数边界没有被单独刻画。** 旧用例未区分“已访问页面
   计数”与 robots 拒绝、HEAD 否决、重定向响应这些不计入访问的结果。

新用例用声明式站点规则/页面/重定向关系推导允许出现的事件模型，再与缓冲
事件记录做多重集、同主机因果先后、跨主机并行（不要求全局顺序）以及正文
关闭次数的核对；同步全部使用通道屏障，sleep 不承担同步职责，超时仅用于
死锁时打印事件并结束测试。

## 三种退化分别由哪条内容断言查出

`task_support/check_mutations.sh` 每次只在临时副本引入一种仍可编译的
退化，并且只跑新测试。三者都由内容断言（`c.Check`）报出，而非构建失败、
panic、超时或源码文本匹配：

1. **绕过 robots 准入**
   （`} else if true || w.isAllowedPerRobotsPolicies(...) {`）
   - `TestCrawlProtocolAdmission` 的事件多重集首先报出：期望的
     `Disallowed .../private/box`、`.../locked` 记录为 0，且出现模型外的
     `Fetch`/`Visit`/`Visited`。
   - 显式断言同时失败：“must be robots-disallowed”以及
     “robots-denied ... must never be fetched”。
   - `TestCrawlProtocolRuns` 两轮独立重算的 robots 断言
     （“round 1 must disallow /deny1”、“round 2 must disallow /root”
     等）也会失败。

2. **忽略 `RequestGet` 的否决**
   （`if false && !w.opts.Extender.RequestGet(...) {`）
   - `TestCrawlProtocolAdmission` 中 `/head-no` 的断言失败：
     “HEAD veto must suppress the GET”、
     “HEAD-vetoed page must not count as a visited page”，以及
     `RequestGet ... allow=false` 的事件多重集不匹配。
   - `TestCrawlProtocolRuns` 的 MaxVisits 场景中
     “HEAD veto must not produce a visit” 同样失败。

3. **把 `Visited` 的 harvested 参数换成 nil**
   （`w.opts.Extender.Visited(ctx, nil)`）
   - 扩展器在 `Visited` 中比对收到的值与 `Visit` 返回值是否为同一底层
     切片；不一致时记录 `HARVEST-MISMATCH`。
   - 因此 `TestCrawlProtocolAdmission` 的
     “Visited must receive the same harvested value Visit returned”
     与各 `Visited` 事件多重集断言失败。

## 运行方式

```sh
bash task_support/run.sh baseline   # 既有 witness 与重定向/base 对照
bash task_support/run.sh target     # 三个新协议组 + 既有对照
bash task_support/check_mutations.sh
```

`TestCrawlProtocolHandoff` 使用真实 `DefaultExtender.Fetch` 与本地
`httptest` 服务，需要允许本机回环端口绑定；在禁止绑定端口的沙箱中该用例
会在 `httptest.NewServer` 处失败，这与既有的 `TestBaseTag` 受同一环境
限制，其余两组不依赖套接字，可正常运行。
