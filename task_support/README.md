入口为 bash task_support/run.sh baseline / target，仅使用 C++17 编译器和系统库，无 CMake 配置或网络下载。Linux 使用 pthread/dl/rt，macOS 使用系统 Cocoa/Foundation/IOKit/Security/Accelerate；作者实际运行的是 macOS arm64，未宣称已测试其他平台。三块完整生产模块 juce_core/juce_events/juce_audio_basics 直接编译，AudioTransportSource.cpp 以自身公开头加入单独翻译单元；不编译设备驱动，不打开声卡。四个模块在 LICENSE.md 中明确为ISC许可。

基线使用8192帧可编辑内存源、4096帧后台预读，先输出64帧0.25，再将128..191帧改成-0.5，随后请求192帧。现有路径的源128帧处仍输出0.25。它描述无通知旧接口的缓存现状，不把这个现象冒称已违反原接口保证；要开发的是新编辑通知接口。

新增 task_tests/test_source_edits.cpp 使用 task_support/check.h 的 task_cases/task_case_count，注册 prefetch/resampling/continuity 三组，函数返回int。每个用例通过 TASK_CHECK 检查实际条件后正常返回0。driver.c 也按C++编译。新增生产单元可放task_src/*.cpp，build.sh会全部编译链接到基线和目标程序。基线正常功能与旧缓存现状在目标运行后仍执行；目标必须实际调用新通知，不能把基线现象当成新能力证明。使用真实 JUCE transport、buffering 和 resampling，内存源及调度信号可在测试中构造。

TimeSliceThread.removeTimeSliceClient 已等待正在执行的客户端；可复用这个同步行为，不要求发明新的线程调度器。flushBuffers 会清相位和滤波历史，不能替代连续编辑。
