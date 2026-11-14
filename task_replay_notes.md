# task_replay 证据链说明

本文件说明 `task_replay_test.go` 如何把"缩减结果 → failfile 持久化 → 字节重放"三段证据
接成一条含义不变的链。所有夹具均为自建、良性，只驱动 rapid 自身机制；生产代码与既有
测试未改动，也不涉及任何第三方程序。三个主测试都用
`task_support/taskcheck` 的 `Run`/`Check` 记录内容断言，固定 seed `1815`、有限检查次数，
所有文件都落在 `t.TempDir()`。

## 1. 独立参照（不依赖 rapid 运行结果）

- **有限语义模型**：`taskModelClassify` / `taskModelVerify` 用一段与 rapid 搜索顺序无关的
  语义描述属性——三个 level（各自 4-bit、0..15），`S = L0+L1+L2`；`S>=2` 在站点 A 失败、
  `S==1` 在站点 B 失败、`S==0` 通过。模型穷举整个有限域（16³ = 4096 个输入），核验：
  手算最小 A 见证 `(1,1,0)`（level 和恰为 2）确实是 A 失败；不存在更便宜的 A 见证；
  每个 B 见证都严格便宜于任何 A 见证；三类输入都非空。期望值由语义推导，不是把一次运行的
  输出另存下来。
- **词流直读重放**：`taskReplay` 把缩减后的 `[]uint64` 重新喂给 `newBufBitStream` +
  `checkOnce`，独立核对实际生成值、失败来源（按 traceback 叶子帧名 `taskOriginOf` 判定）和
  命中分支，而不是只调用 `compareData` 之类的底层比较。
- **独立小端编码**：`taskEncodeLE` 是测试自己定义的小端编码器；手算非对称样例
  `0x0102030405060708, 0xff` 编码为 `0807060504030201ff00…`，字节序一旦写反，每个解码词都会变。
- **丢弃尝试的独立演示**：直接在 bitStream 层用 `beginGroup/endGroup(discard=true)` 构造
  "try" 分组，验证 `prune()` 会删掉被拒尝试的词、保留接受分组，对应 `Filter` 重试机制。

属性刻意设计成两个失败位置 `taskFailSiteA` / `taskFailSiteB` 发出**同一条消息**但位于不同
源码行（traceback 不同）。每个 level 是独立的一个 bitstream 词，块最小化会按 0,1,2,… 顺序
尝试，因此缩减必然路过 `S=2`(A) → `S=1`(B) → `S=0`(通过)：B 见证更小但含义不同，正好检验
缩减器是否守住原始失败来源。嵌套结构由 `Custom` + `SliceOfN` 固定三元组 + 带 `Filter`
重试（可丢弃 "try" 分组）的 marker 提供。

## 2. 旧覆盖的缺口

- `shrink_test.go` 的 `checkShrink` 只用 `refDraws` 比对最终生成值、并要求两次 traceback
  相等；其属性只有单一失败点，无法发现"缩减把失败迁移到另一个同消息、不同 traceback 的
  位置"。新测试用双失败点 + 独立模型，要求最终见证保持站点 A 且达到模型推导的最小 level 和。
- `persist_test.go` 的 `TestFailFileRoundtrip` 只覆盖 `saveFailFile`/`loadFailFile` 的字节
  往返，不覆盖 `checkFailFile`/`doCheck` 的重放语义、失效后忽略、属性改跳过后继续正常检查。
  新测试真实保存/重读 failfile，用返回值、属性调用计数和 `recordingTB` 日志区分"未执行"
  （随机搜索 valid/invalid 全 0）、"失效后继续"（跑到 invalid 预算 300 次）与"重现失败"
  （两次重放、命中计数 `[2,0]`），并另写注释、版本不匹配、无效词、读取失败、空文件等
  非 `saveFailFile` 产出的文本夹具。
- `engine_fuzz_test.go` 里的 `Fuzz*` 目标全部 `t.Skip()`，`MakeFuzz` 没有可执行的字节序、
  尾段补零、词流耗尽或子进程测试结果校验。新测试在隔离子进程里经公开 `MakeFuzz` 重放同一
  词流，覆盖正常、主动跳过、耗尽（引擎级 SkipNow）、同源失败，以及崩溃/超时/编译失败三类
  不得算作成功检出的负面对照。

## 3. 三份退化的检出证据（`task_support/mutations.json`）

QA 驱动在临时副本里各应用一份退化，均先 `go test -c` 编译成功，再由内容断言报错
（`--- FAIL: TestTask…`，无 panic / fatal / data race）：

1. **failure-origin（`shrink.go`，放宽缩减失败来源）**：把 `accept` 的 traceback 校验放宽为
   仅在消息不同或 invalidData 时拒绝。缩减于是从站点 A（level 和 2）滑向更小的站点 B
   （level 和 1）。报错点：`TestTaskShrinkIdentity` 的"preserved site-A origin"、
   "minimum level sum got 1 want 2"、重放命中 `[0 1]`；`TestTaskReplayFiles` 的 site-A 与
   命中计数断言；`TestTaskByteBridge` 子进程痕迹变成 `origin=1`。
2. **invalid-replay（`engine.go`，让失效文件冒充失败）**：`checkFailFile` 对 invalidData
   改为返回该错误而非 `nil`。属性切到跳过后，旧文件本应被忽略并继续完整的有界搜索，现在却
   被当成失败。报错点：`TestTaskReplayFiles` 的"skipped replay invalidates the fail file"、
   "no failure is reported"、`invalid=0`（期望 300）以及调用计数 `1`（期望 301）。
3. **byte-order（`engine.go`，小端改大端）**：`checkFuzz` 改用 `BigEndian.Uint64`。
   报错点：`TestTaskByteBridge` 非对称词解码反转（`0x807060504030201…`）、九字节尾段补零词
   错误、词流耗尽轨迹不符，以及同源失败子进程退出码由 1 变 0（字节序改变后不再触发站点 A）。

## 4. 备注

- `taskShieldTB` 是一个永不标记 failed 的内部 TB 包装：属性失败以 `*testError` 返回、交由
  `c.Check` 判定，避免 `doCheck` 入口的 `!tb.Failed()` 断言把一次内容失败升级成 panic，
  从而保证变异下呈现的是干净的内容拒绝。日志仍透传到真实测试输出。
- 正常源码下 `run.sh target` 三个主测试与既有基线全部通过；结果对固定 seed 确定性可复现。
