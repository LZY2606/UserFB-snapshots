离线入口：bash task_support/run.sh baseline / target。需要 JDK 17+ 与 bash。构建使用 javac，未调用 Maven。macify 1.6 的原始源文件含 Latin-1 注释，先按 ISO-8859-1 编译；项目及其余依赖使用 UTF-8。user.home JVM 属性仅指向构建目录，未加载个人配置。上游原定 macify 1.4 在 Maven Central 没有该源包，使用同 API 的 1.6，仅提供构建支持。

two-objects.sup 是手写 PGS 素材：8×4画布，PCS引用对象0、1；分别放在(0,0)、(4,0)，两对象各2×1，独立WDS；PTS为90000..180000。每个ODS为完整单片。当前导入仅显示第一个2×1对象。原始单对象素材来自 src/test/resources/test.sup。新增程序应自己构造交错分片、缓存和裁剪数据；这里没有目标实现。

TargetDriver 分别调用 task_tests.PGSCompositionTest 的 objects/snapshots/roundtrip(Checks)。Checks.runCase(String,CaseBody) 要正常返回并且该用例至少执行一次 check(boolean,String)，各组必须有用例。异常与进程提前退出均不算完成。输出只证明程序被执行；测试自身仍须真实断言目标语义。
