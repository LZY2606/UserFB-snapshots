# 语料管理确定性回归：开发说明

本目录下的 `corpus_model.cpp` 是 libFuzzer 语料管理（离线 `Merger` 与在线
`InputCorpus`）的确定性模型回归。被测对象始终是生产代码
（`lib/fuzzer/FuzzerMerge.*`、`lib/fuzzer/FuzzerCorpus.h`），模型侧只做独立、
小规模的推导与记账，不复制生产实现。用例通过 `task_support/check.h` 的
`TASK_CHECK` 注册为 journal / projection / ownership 三组，由
`task_support/driver.c` 驱动，`task_support/run.sh target` 编译运行。

## 夹具构成

- `merge_fixture.cpp`：良性覆盖夹具，任何输入都返回成功，不崩溃、不终止进程。
  真实二进制由 `run.sh` 编译为 `.task-build/merge-fixture`。
- journal 组：合成 STARTED/FT/COV 控制日志（含末尾只有 STARTED 的正常停止
  元数据、按完整记录边界的截断），同一份日志分别以 coverage 开/关交给两个新建
  的 `Merger` 解析，再与独立逐行模型逐项核对；其中两个用例直接启动真实
  merge-fixture 跑 `-merge=1`，解析真实日志并把真实字节、记录尺寸、feature 集
  送入同一套离线/在线模型。
- projection 组：独立预测贪心 merge（先扣除已知 feature，再按尺寸升序、剩余
  feature 数降序排序，单趟贪心选文件），与生产 `Merger::Merge` 的
  NewFeatures / NewCov / NewFiles 按集合语义核对。
- ownership 组：按 `Fuzzer::RunOne` 的真实顺序（先逐特征 `AddFeature`，有更新
  才一次 `AddToCorpus`）驱动真实 `InputCorpus`，同时维护独立的
  「feature 槽 -> 当前最小尺寸输入」映射，覆盖 shrink 开关、等尺寸不接管、
  异号同槽、失去最后一个 feature 被清空、单候选一次接管多个旧输入、Replace
  与固定种子采样。

## 两边都必须一直成立的共同性质

1. 归属唯一性：任一时刻，每个 feature 槽（在线）或每个未被覆盖的 feature
   （离线）至多有一个当前归属者；离线侧体现为贪心集合 `AllFeatures` 的成员判定，
   在线侧体现为 `SmallestElementPerFeature` 与模型 `SlotOwner` 一致。
2. 单调增长、只减不跳：离线 `AllFeatures` 与在线 `NumFeatures` 只在遇到真正
   未见过的 feature 时增长；计数（`NewFeatures->size()`、`NumAddedFeatures`、
   `NumUpdatedFeatures`）必须与独立记账逐值相等。
3. 尺寸优先：离线选文件先比尺寸、再比剩余 feature 数；在线接管只在
   `Shrink` 开启且新尺寸严格更小时发生，等尺寸永不接管。
4. 记录与真实字节一致：日志里的 `STARTED` 尺寸等于磁盘文件实际字节数；
   输出语料里被选中的文件以内容 SHA1 命名，字节与输入完全相同。
5. 可重放：所有轨迹只用本地有限字节串与固定种子，步数有硬上限；失败信息带
   种子、步骤与最小记录（`G_ctx`），任何目标进程都不崩溃。

## 不能直接互相等同的输出

- 控制日志里的 `FT` 只记录「此前任何输入都没见过」的 feature，它不是单个输入
  的完整覆盖；因此真实日志的在线重放中每个 feature 只出现一次，共享 feature 的
  接管关系必须用合成记录（ownership 组）单独构造，不能拿 FT 当全量覆盖。
- `COV`（PC 边）的统计与文件是否被选中是两个独立问题：没有新增 feature 的
  文件不会进 `NewFiles`，但其未见过的 PC 仍会计入 `NewCov`；PC 还按整个日志
  去重，不能把每条 `COV` 行的并集直接当成逐文件覆盖。
- 离线 `Merge` 选出的文件名集合与在线 corpus 的活跃输入集合不是同一个集合：
  前者按文件去重选最小代表，后者按 feature 槽维护最小归属输入，二者只共享
  「尺寸优先、归属唯一」的性质，不存在一一对应。
- 同尺寸同剩余 feature 数的打平时生产排序不保证固定次序，用例只比较集合含义；
  需要精确比较名字的用例在构造时避开平局。
- 采样检查只断言「固定种子、有限步数下只返回仍有归属的非空输入，且每个活跃
  输入都可达」，不断言某个标准库给出固定采样序列，也不用概率比例阈值替代
  归属检查。

## 运行方式

- `bash task_support/run.sh target`：编译真实 libFuzzer 静态库、merge-fixture
  与本模型，运行 journal/projection/ownership 三组用例，再跑已有对照。
- `bash task_support/run.sh baseline`：只编译运行上游对照与有限 merge 夹具。
