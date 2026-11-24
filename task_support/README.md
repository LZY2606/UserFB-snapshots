离线入口：bash task_support/run.sh baseline / target。需要 JDK 17+ 与 bash。构建使用 javac，未调用 Maven。macify 1.6 的原始源文件含 Latin-1 注释，先按 ISO-8859-1 编译；项目及其余依赖使用 UTF-8。user.home JVM 属性仅指向构建目录，未加载个人配置。上游原定 macify 1.4 在 Maven Central 没有该源包，使用同 API 的 1.6，仅提供构建支持。

two-objects.sup 是手写 PGS 素材：8×4画布，PCS引用对象0、1；分别放在(0,0)、(4,0)，两对象各2×1，独立WDS；PTS为90000..180000。每个ODS为完整单片。当前导入仅显示第一个2×1对象。原始单对象素材来自 src/test/resources/test.sup。新增程序应自己构造交错分片、缓存和裁剪数据；这里没有目标实现。

TargetDriver 分别调用 task_tests.PGSCompositionTest 的 objects/snapshots/roundtrip(Checks)。Checks.runCase(String,CaseBody) 要正常返回并且该用例至少执行一次 check(boolean,String)，各组必须有用例。异常与进程提前退出均不算完成。输出只证明程序被执行；测试自身仍须真实断言目标语义。

运行说明：
- `bash task_support/run.sh baseline`：编译主程序，跑原始单对象素材 src/test/resources/test.sup，并打印手写双对象素材 two-objects.sup 当前实际读出的帧数、位图尺寸与偏移。
- `bash task_support/run.sh target`：在 baseline 基础上额外编译并运行 task_tests/PGSCompositionTest。objects 组覆盖对象分片交错、PCS 裁剪与窗口/画布求交、无几何交集、未完成对象报错与 forced 逻辑或；snapshots 组覆盖对象缓存复用、区间拆分、对象版本、调色板快照与新 epoch 不继承；roundtrip 组用 SupBDWriter.createSupFrame 写出后重新读入，核对像素、绝对位置和 90kHz 起止时间。最后再执行一次单对象正常路径。
- 仅需 JDK 17+ 与 bash，javac 构建，不引入新的运行依赖；临时文件使用 $TMPDIR。

