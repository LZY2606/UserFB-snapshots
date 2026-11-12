support 仅含原绘制路径基线、原 VertexProcessorTest.cpp 的无窗口显示适配、目标测试注册和编译入口。没有 SSR、几何缓冲、反射示例或目标用例。

运行 bash support/baseline.sh；目标入口为 bash support/target.sh [PPM输出目录]。新增 tests/ssr_cases.cpp 定义 projection_trace、depth_intervals、capture_and_composite、render_integration 四个 void(Checks&) 函数，以 Checks::require(bool, message) 留下真实数值检查。入口由固定 main 调用每组并核对计数，零检查或异常均失败，不接受 skip。

本目录新增的测试基础设施按上游 MIT License 提供；原上游文件未改动。
