固定生产基线：bash task_support/run.sh baseline。
新增测试入口：bash task_support/run.sh target，测试包含 task_support/check.h，使用 task_cases/task_case_count 和 TASK_CHECK，不定义 main。
测试可包含 h264bsd_decoder.h 和 minih264e.h，所有 src/*.c 及固定编码器实现都会直接编译并链接。H264E_MAX_THREADS=0；编码器素材生成使用纯CPU路径。
minih264e.h 位于 task_support/vendor/minih264，属于固定素材生成依赖，仅生成旧I/P对照，不具备目标帧间8×8编码能力。CAVLC的基线输入由真实编码产生，无运行时下载。
