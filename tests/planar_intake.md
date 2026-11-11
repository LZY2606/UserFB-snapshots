# Planar intake regression

回归目标：证明 `encoder_encode2` 的三个独立视频平面在非紧凑（带字节
stride、行尾填充、前后保护区）输入下，与紧凑公开入口 `encoder_encode`
摄入的是同一张图；同时覆盖内部 source padding 补边、输入位深到内部位深的
升位，以及完整的真实编解码闭环。崩溃、越界或编译错误都不算正确检出，
所有断言都报告首个差异的用例、平面与坐标。

## 文件

- `planar_intake.cpp`：回归主程序（公开编码 API 比对 + 真实解码闭环 +
  `Resampler::ConvertFrom` 内部样本核对）。
- `planar_intake_ref.h` / `planar_intake_ref.cc`：独立参照模块。只依赖
  `xvc_common_lib/common.h` 的枚举/常量，不调用任何生产的复制、补边或
  升位 helper；紧凑参照由独立的图案压紧函数生成。
- `run_planar_intake.sh`：在临时目录中分别以 `HIGH_BITDEPTH=ON`（完整
  组合）和 `HIGH_BITDEPTH=OFF`（仅 8 位组合）构建并运行，成功打印
  `xvc planar intake: ok`。
- `mutation_planar_intake.sh`：变异自检，见下文。

从仓库根目录运行：

```
bash tests/run_planar_intake.sh
```

## 输入构造

- 确定性短序列，每序列 17 帧；`sub_gop_length` 分别取 1 和 8，最后完整
  flush。
- 位深组合（输入/内部）：8/8、8/10、10/10、10/12；`HIGH_BITDEPTH=OFF`
  构建只支持 8/8。
- 色度格式：4:2:0 与 4:4:4。
- 尺寸：32x24（CU 尺寸整数倍，走快速复制分支）与 34x26（内部补到
  40x32，crop 6x6，真正走 `source_padding` 的 `CopyFromBytesWithPadding`
  补边分支）。
- Y、U、V 三个平面使用互不相同的横纵图案，U 与 V 图案不同；图案样本恒低于
  输入码最大值，padding 值取图案不可能出现的高端值，因此误读 padding 必然
  表现为内容差异。
- 每个平面独立缓冲，stride 单位为字节且为正；行尾填充与图像取值不同，
  缓冲前后各有 4 字节保护区。高位深缓冲以本机 `uint16_t` 存储有效低位
  样本，首地址按 `uint16_t` 对齐；不支持也不测试奇数字节对齐。
- 每次 `encoder_encode2` 返回后立即覆写它刚消费的图像区（保护区不动），
  下一帧再填入新图案，并校验保护区/行尾填充未被编码器改写。
- 不新增无效指针用例；所有缓冲完整可用。

## 断言内容

1. 每组用两个全新编码器，参数、输入帧与 SIMD 配置（`simd_mask=0`、
   `threads=0`）一致：比较全部输出 NAL 的类型、顺序与逐字节内容，并按
   POC 比较立即复制下来的重建画面。
2. 同一序列用两个不同的 padding 值各跑一次 planar 入口，码流必须完全
   不变。
3. `sub_gop_length=8` 必须观察到编码（NAL 输出）顺序与显示（POC）顺序
   不同；`sub_gop_length=1` 不允许出现重排。仅把配置写成 8 而实际只编
   一帧无法通过该检查（每序列固定 17 帧）。
4. 用真实解码器解全部输出 NAL（逐 NAL 喂入并排空输出，规避解码器滑动
   窗口回收缓冲），按 POC 与编码器重建结果逐平面比较有效像素，同时核对
   尺寸与位深、校验 `user_data` 与 POC 一致、校验 conforming 计数为 0。
   输出是有损的，不与原始输入做逐字节比较。
5. `Resampler::ConvertFrom` 核对：对紧凑入口、带 stride 的分离入口以及
   source padding 补边路径，从原始图案独立计算升位后的每个有效样本并
   逐一比对；补边用例额外检查右边延拓最后有效列、下边延拓最后有效行。
   目标 `YuvPicture` 不带 border margin，断言只读取已初始化区域。

## 覆盖用例数（实际输出）

`HIGH_BITDEPTH=ON`：2 尺寸 x 2 色度 x 4 位深 = 16 个基础组合。

- resampler 核对：16 组合 x 2 帧 x 2 入口（紧凑/分离）= 64 个用例。
- 端到端：16 组合 x 2 个 sub-gop 长度 = 32 个用例。

`HIGH_BITDEPTH=OFF`：2 尺寸 x 2 色度 x 1 位深（8/8）= 4 个基础组合。

- resampler 核对：4 x 2 x 2 = 16 个用例。
- 端到端：4 x 2 = 8 个用例。

## 变异自检与实际检出输出

```
bash tests/mutation_planar_intake.sh
```

脚本在两份独立临时源码副本中各注入一处缺陷（原工作树不被修改，并在结束时
校验 `git status` 不变）。两份副本都必须编译成功，且由样本/NAL 断言失败
检出，崩溃或编译失败不算检出：

- `stride`：把高位深带 stride 分支的行前进 `input_bytes += input_stride`
  改成紧凑步长 `width * 2`。实际首个检出：
  `MISMATCH case=resampler-strided w32 h24 cf1 in10 int10 gop1 frames17 frame=0 plane=0 x=0 y=1 expected=44 actual=0`
- `upshift`：去掉 8 位输入到高位深内部的升位
  （`input_bytes[x] << bit_shift` 改为 `input_bytes[x]`）。实际首个检出：
  `MISMATCH case=resampler-compact w32 h24 cf1 in8 int10 gop1 frames17 frame=0 plane=0 x=0 y=0 expected=52 actual=13`

首个差异的用例、平面与坐标同时写入环境变量 `PLANAR_INTAKE_FAIL_FILE`
指向的转储文件，作为最小失败输入的定位信息。

## 既有回归

`bash tests/baseline.sh` 的三帧公开接口编解码回归保持可运行并继续通过。
