# Scheduling model — 派发契约回归说明书

本文件说明 `test/scheduling-contract.test.mjs` 使用的工作账本、状态迁移、三个场景各自独立的
预期序列，以及旧的 `priority.js` / `limiter.js` / `rateLimit.js` / `errorHandling.js`
测试没有覆盖的交叉关系。退化检测由 `test/run-scheduling-mutations.mjs` 执行。

## 1. 测试边界与设施

### 1.1 真实 Crawler、真实 HTTP，不接真实站点

- 入口固定为**构建产物** `dist/index.js`（可用环境变量 `SCHED_CRAWLER_ENTRY` 指向变异副本的
  构建产物），所有请求都走 `Crawler.add` → 请求池 → got → 真实 `node:http.Server`，
  生产源码与旧测试一律不改。
- 运行环境的沙箱禁止 TCP 绑定（`listen` 一律 `EPERM`），因此本地 HTTP 服务不监听端口，
  而是用一对内存 `Duplex` 流把自定义 agent（`agent.http.addRequest` → `req.onSocket`）
  与 `http.Server` 的 `connection` 事件直接对接。HTTP/1.1 报文仍由 Node 自身的 parser 编解码，
  Crawler、got、cheerio 解析链全部是真实实现，没有任何桩。
- 服务端**闸门**（一个 gate Promise）决定每个响应何时放行，从而精确控制“网络响应什么时候
  回来”。响应体全部是 ASCII 纯文本（冒烟与回归都不依赖浏览器或在线资源）。
- `Crawler` 构造固定 `homogeneous=false`、`skipDuplicates=false`，至少使用两个数字
  `rateLimiterId`（池 1 与池 2）。

### 1.2 受控时钟：只替换调度计时

`installVirtualClock` 按**调用栈帧**识别计时器来源，只虚拟化两类调度计时：

| 栈帧 | 计时器用途 |
| --- | --- |
| `dist/rateLimiter/rateLimiter.js` | 预留槽之后的派发 timer（`_schedule` 里的 `setTimeout`） |
| `dist/crawler.js` | 外层重试 timer（`_handler` 里的 `setTimeout(..., retryInterval)`） |

其余一切（got 的请求超时、HTTP parser、`check.cjs`、进程自身）继续使用真实 timer。
`Date.now` 与虚拟时钟保持同一个 `now`，因为派发间隔就是用 `Date.now()` 与
`nextRequestTime` 计算的。`advance(ms)` 每触发一个虚拟 timer 就让出一个真实 macrotask，
让真实 HTTP 解析与 Promise 链推进；断言基于虚拟时间点，不做真实等待与容差比较。
每个场景的 `finally` 都先恢复时钟、再关闭服务，并在场景体内等到 `drain`。

## 2. 工作账本（ledger）

账本是测试自己维护的独立记录，**所有期望值只从账本推导**，不读取 crawler 的私有
`_limiters`、`runningSize`、`nextRequestTime` 等运行值。

### 2.1 每条逻辑任务的记录

`ledger.tasks` 按任务 id 保存：`id / path / pool / priority / options`（add 时的原始对象）、
`reservations[]`（每次池预留的时刻、日历 due 与单调 timerId；一条逻辑任务终身恰有一次）、
`pre[]`（preRequest 时刻）、`requestEvents[]`（request 事件时刻、当时的 `retries`、options 引用）、
`hits[]`（服务端收到的真实请求记录，含 attempt 与最终 status）、`callbacks[]`、`dones[]`。

### 2.2 全局事件流

`ledger.log` 按发生顺序追加带虚拟时间戳的事件：

```
enqueue → schedule → reserve → preRequest → request → hit → callback → done
                      ▲                                  ↘（失败且还有 retries）
                      └ timer 安排即预留                    retryWait(虚拟 retry timer，槽不释放)
                                                              → preRequest → request → hit → …
```

- `reserve`：虚拟**派发 timer 被安排**的那一刻记账（源码在同一处 `runningSize++` 并安排 timer，
  因此 timer 安排时刻就是槽的可外部观察预留时刻），记录 timerId 与 due；
- `retryWait`：虚拟**重试 timer 被安排**时记账，明确这是“槽仍被占用的等待”，不是新的预留。
  timerId 单调递增，用于区分同一虚拟时刻的多个预留先后。

### 2.3 状态迁移

1. **QUEUED**：`add()` 后任务进入某池的多优先级队列；账本表现为有 `enqueue/schedule`、
   还没有 `preRequest`，即 `ledger.waiting(pool)` 的成员。
2. **RESERVED**：`RateLimiter._schedule` 在 `runningSize < maxConnections` 时**立即**
   `runningSize++`、计算 `delay`、推进 `nextRequestTime` 并安排派发 timer（账本记 `reserve`）。
   预留发生在 timer 安排的那一刻，而不是 timer 触发的那一刻——因此一个已经预留且已排 timer
   的任务，后来到达的高优先级任务抢不走它；优先级只能改变仍在 QUEUED 的那部分顺序。
3. **DISPATCHED**：timer 触发，执行提交函数 → `preRequest` → `request` 事件 → got 发出报文。
   `preRequest` 把对应的待匹配派发 timer 绑定到该任务（重试尝试没有新的派发 timer，
   因此不会产生第二次预留）。
4. **WIRE / RESPONDED**：真实 server 产生 `hit`，响应经解析链回到 `_handler`。
5. **RETRY_WAIT**（仅失败且 `retries>0`）：安排 `retryInterval` 的虚拟 retry timer
   （账本记 `retryWait`），**槽仍归这条请求所有**，不回到池里；timer 到期后对**同一个
   options 对象**再次 `_execute`，所以第二次尝试仍走原请求的 `preRequest` 与 `request` 事件。
6. **CALLBACK**：成功响应或重试耗尽后的终止错误交给用户回调。回调返回前槽不释放。
7. **DONE**：用户调用 `done()` 恰好一次，`runningSize--` 并触发同池下一次 `_schedule`。
8. **DRAIN**：所有池的 `runningSize + waitingSize` 归零，`_release` 触发一次 `drain`，
   它必须是账本的最后一个事件。

### 2.4 账本级通用不变量（每个场景都检查）

- 每条逻辑任务**恰好一次池预留**（`reservations.length === 1`），重试不产生第二次预留，
  且预留时刻 ≤ 首次 preRequest 时刻 ≤ 预留日历 due；
- 每个 `request` 事件都对应一次真实 server `hit`，且 request 时刻 ≤ hit 时刻；
- 每条逻辑任务恰好一次用户回调、恰好一次 `done()`，且 `done ≥ callback ≥ 最后一次 hit`；
- 回调/done 不可能出现在自己的 request 之前；
- `drain` 全场景恰好一次且位于事件流末尾。

## 3. 三个场景各自独立的预期序列

### 3.1 Pool dispatch（虚拟 t0 = 1000，池 1 `maxConnections=1`，池 2 独立）

1. `blocker`（池 1，优先级 2）入队即预留并派发，server 收到 `/blocker` 后被闸门挂起；
2. 再入队 `alpha`、`beta`（均优先级 2）、`urgent`（优先级 0）与池 2 的 `parallel`；
3. 此时线上路径恰为 `["/blocker", "/parallel"]`，池 1 等待账本为
   `["alpha", "beta", "urgent"]`，blocker 回调尚未发生；
4. 虚拟时间空转 5000ms：池 1 线上仍只有 `/blocker`，证明已预留槽不可被后来任务抢占，
   而池 2 的 `parallel` 已完整走完 request→callback→done；
5. 放行 blocker 闸门。`urgent` 的用户回调里**重入 `add("/extra")`**，新任务排当时队尾；
6. 池 1 线上顺序：`/blocker → /urgent → /alpha → /beta → /extra`
   （数字越小优先级越高；同优先级 FIFO；重入任务在队尾）；
7. 全量线上路径：`/blocker, /parallel, /urgent, /alpha, /beta, /extra`，
   `request` 事件 id 序列与之同序——核对的是相对次序而不是回调总数；
8. 每个任务一次回调一次 done，`drain` 一次且最后；
9. 收尾再调一次 `crawler.send("/send-only")`：server 多收到一次真实请求，但
   `schedule` 账本条数不变、不产生 drain——`send` 不占共享池，不是这本账的派发入口。

### 3.2 Retry occupancy（虚拟 t0 = 2000，`retries=1`，`retryInterval=100`，got retry 关闭）

1. `flaky`（池 1）第一次尝试在 t=2000 收到真实 **503**；账本出现一次 `retryWait`（retry timer，
   `due - scheduledAt = 100`），且 `flaky` 的 `reservations` 始终只有 1 条，池 1 等待账本为
   `["high"]`；
2. t=2050（重试窗口中点）：池 1 线上仍只有一次 `/flaky`，高优先级 `high` 无法偷跑；
   池 2 的 `p2-a`、`p2-b` 均已 done，且它们的 done 时刻早于 retry timer 的 due——
   一个池的回调暂缓不会堵住另一个池；
3. t=2110（跨过 t+100）：第二次尝试重新执行原 `preRequest`（pre 记录 2 条）、
   重新发 `request` 事件，事件上的 `retries` 序列为 `[1, 0]`，两次事件引用同一个
   options 对象，线上 `/flaky` 共两次；第二次响应被服务端闸门挂起，此时仍无用户回调；
4. 放行响应闸门：成功响应进入用户回调，但回调被另一道闸门 hold 住、尚未 `done()`，
   `high` 依旧没有派发——成功响应交给用户之后、用户调 done 之前槽不释放；
5. 用户放行并 `done()` 后 `high` 才派发；池 1 线上顺序为
   `/flaky, /flaky, /high`；回调 id 顺序为 `p2-a, p2-b, flaky, high`；
6. 重试耗尽子用例 `doomed`：线上两次 503，之后恰好一次终止错误回调；错误回调的
   `response.options` 就是 add 时的原 options（`retries` 已为 0，`userParams`、`url`
   均保留），done 仍只调一次；
7. `got-off` 子用例以 `retries=0` 入队：线上**恰好一次** hit 即把 503 作为外层错误
   交给回调，证明 got 自身 retry 已被 `retry.limit=0` 关闭，重试完全由 Crawler 外层控制。

> 本组分三波顺序执行（主波 4 任务、`doomed`、`got-off`），每一波队列排空时各产生一次
> `drain`，断言按各自波次核对“恰好一次且为该波最后事件”；`done()` 在三个波次里都只调一次/任务。

### 3.3 Rate calendar（虚拟 t0 = 1000，构造时 `rateLimit=100`，池并发因此为 1）

1. `cal-a`（池 1）t=1000 预留即派发并被闸门挂起；此时入队同优先级 `cal-b` 与高优先级
   `cal-c`，以及池 2 的 `p2-a`、`p2-b`。这是要求钉住的时刻：**前一个请求占着槽，
   同优先级与高优先级任务都已经在等**；池 2 的 `p2-a` 同一时刻独立派发；
2. t=1050 调用 `setLimiter(1, "rateLimit", 400)`：调用前后所有未触发/未取消 timer 的
   快照完全相同——已安排的 timer 不回改，既有的 `nextRequestTime` 也不清；
3. 在旧日历点（1100）**之前**放行 `cal-a`：t=1050 时 `cal-c` 不会立刻派发，证明预留时
   算出的发送日历仍然生效；
4. t=1100：`cal-c` 派发（沿用旧 `nextRequestTime=1100`，而不是“当前时间 1050”），
   高优先级先于同优先级的 `cal-b`；池 2 的 `p2-b` 同样在 t=1100 按自己的 100ms 日历派发；
5. t=1490 时 `cal-b` 仍未发出；t=1500 `cal-b` 派发——新的 400ms 间隔只作用于更新之后
   才预留的任务（1100 + 400）；
6. 池 1 发送日历 `[1000, 1100, 1500]`，池 2 发送日历 `[1000, 1100]`，两者互不影响；
7. 串行不变量：池 1 按预留时刻排序后，每个任务的 pre 时刻都不早于前一任务的 done 时刻；
8. 全场景一次 drain 且 drain 为最后事件。

## 4. 旧测试缺失的交叉关系

| 旧测试 | 已覆盖 | 缺失的交叉关系 |
| --- | --- | --- |
| `test/priority.js` | 单池、四个不同优先级任务的最终回调数组顺序 | 没有同优先级 FIFO 断言；没有“已预留槽+已排 timer 不可被后来高优先级抢占”的边界；没有第二个池并存；不核对 request/callback/drain 的相对次序；没有回调内重入 `add`；没有“回调持有 done 期间槽不释放” |
| `test/limiter.js` | 固定 rateLimit 下用真实时钟与 ±50ms 容差比较时间差，验证多池并行/独立 | 没有受控时钟下的精确发送日历；没有“一个池在重试/回调中暂停时另一个池照常前进且回调不被堵”；没有优先级与池、rateLimit 的组合场景 |
| `test/rateLimit.js` | 单池固定间隔、真实等待容差；`setLimiter` 用例是**先改值再加任务** | 没有覆盖任务已在等待、timer 已安排时更新 rateLimit 的语义：已排 timer 不回改、既有 `nextRequestTime` 不清、新间隔只影响之后预留的任务；没有跨池 setLimiter 隔离断言 |
| `test/errorHandling.js` | nock 持久桩下最终 error/code、超时与 `retries` 归零 | 没有 503→成功的外层重试探查；不检查重试等待期间槽位仍被占用、高优先级任务排队等待；不验证第二次尝试仍走原 `preRequest`/`request` 事件与同一 options；不验证终止失败时 options 保留；不验证成功后 done 之前槽不释放；不验证跨池不堵塞；不证明 got 自身 retry 已关闭 |
| 共性 | 均使用 nock 拦截、断言最终计数或时间容差 | 没有真实 HTTP server 对**路径与次数**的核对；没有事件流次序断言；没有独立于内部状态的工作账本；没有受控虚拟时钟 |

## 5. 三种退化与对应检测点

`test/run-scheduling-mutations.mjs` 在彼此独立的临时源码副本里各做一处退化，分别重新构建，
先通过 ASCII 本地 HTTP 冒烟（排除构建/导入/传输问题），再跑同一套回归。只有内容断言
（带 expected/actual 的 AssertionError）才算检出：

| 退化 | 改动位置 | 被哪个断言抓住（实际表现） |
| --- | --- | --- |
| 同优先级改成 LIFO | `src/lib/queue.ts` 出队改为弹尾部 | Pool dispatch 的池 1 线上顺序：期望 `[..., "/urgent", "/alpha", "/beta", "/extra"]`，实际 `[..., "/urgent", "/extra", "/beta", "/alpha"]` → `SCHEDULING_FIFO_CAUGHT` |
| 重试时先归还原槽再重新排队 | `src/crawler.ts` 失败分支先 `release()` 再 `_schedule` | Retry occupancy 重试等待中的等待账本：期望 `["high"]`，实际 `[]`（high 提前偷跑）→ `SCHEDULING_RETRY_CAUGHT` |
| 更新 rateLimit 时把 `nextRequestTime` 清成当前时间 | `src/rateLimiter/rateLimiter.ts` `setRateLimit` | Rate calendar 的 `cal-c` 派发时刻：期望 `1100`，实际 `1050` → `SCHEDULING_CALENDAR_CAUGHT` |

正常源码完整通过时打印 `SCHEDULING_BASE_OK`。构建失败、导入失败、超时或目标组根本没有
执行都会以 `SCHEDULING_RUNNER_ABORT` 中止，不会冒充检出。

## 6. 运行方式

```sh
bash task_support/run.sh target    # 构建 → 三组回归 → 变异副本检测（含四个 marker 校验）
bash task_support/run.sh baseline  # 正常采集链路与原有优先级行为（WIRE_BASELINE_OK / SCHEDULING_NORMAL_OK）
```
