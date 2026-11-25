# Game Boy NR50 立体声主音量示例

`gb_stereo_wav.cpp` 是一个不依赖声卡、不需要 Python 的命令行示例：在 C++ 中
构造一段带寄存器写入的最短 GBS，通过公开的 `gme_open_data` / `gme_play`
接口播放，并把交错的 16-bit 立体声 PCM 写成 WAV。前半段使用 `NR50 = 0x70`
（左声道最大、右声道最低增益），后半段使用 `NR50 = 0x07`，可以直接听出或
从波形上测出约 8 倍的左右振幅差。

## 寄存器语义

* `NR50`（`0xFF24`）
  * bit 6..4：左侧主音量字段 `L`（0..7）
  * bit 2..0：右侧主音量字段 `R`（0..7）
  * 每侧增益为 `(字段值 + 1) / 8`。字段为 0 时增益是 `1/8`，是最小增益而不是静音。
  * bit 7 / bit 3 是 VIN 位，仍按原实现忽略。
* `NR51`（`0xFF25`）
  * 低四位（bit 0..3）分别把 Square 1、Square 2、Wave、Noise 送到右侧。
  * 高四位（bit 4..7）把对应声部送到左侧。

当某个声部同时送左右、且两侧增益相等时，仍像以前一样只向共享的
center 缓冲贡献一次；两侧增益不同时，改为分别向左、右缓冲以各自增益合成。
只送一侧时只写对应的 left/right 缓冲。

## 实现要点（`gme/Gb_Apu.*`、`gme/Gb_Oscs.*`）

* 为每一种合成器精度（Square 的 good-quality `Blip_Synth`，Wave/Noise 的
  medium-quality `Blip_Synth`）准备了 8 个增益档 `synth[0..7]`，其音量为
  `(档位 + 1) * volume_unit`，与原 `volume_unit` 中已含的 `/8` 基准一致。
* 音量作用在芯片时钟上的幅度跳变事件里（不同档位的 synth 自带不同
  `delta_factor`），不是在输出端对整块 PCM 乘比例，因此块中途改 `NR50`
  时，滤波器和重采样的先后顺序仍然正确。
* 每个声部维护最多两个真实端点（center，或 left + right）。改 `NR50`
  或 `NR51` 时，先用“旧端点 + 旧增益档 synth”发出 `-last_amp` 撤下旧的
  持续电平，再按新路由/新增益重建端点，并把 `last_amp` 置 0；下一次
  `run()` 会在同一芯片时刻按当前波形电平重新接上。连续同刻写寄存器时，
  中间尚未发声的配置不会产生多余事件，调用次序保持不变，也不会重触发或
  改动相位、波表位置、LFSR、包络和长度计数。
* 撤电平时 Square 走 Square synth、Wave/Noise 走 other synth，保证同一
  脉冲核精确抵消。原来硬编码到 `wave.outputs[3]` 的 wave 电平修正不再向
  “未被 NR51 连接的 center”额外补一份；通用的按真实端点撤下/重接已经
  完整覆盖它（平坦波表的独立 Blip 脉冲参照逐样本为 0 差异）。
* `output(center, left, right)` 三缓冲接法不变；`output(Blip_Buffer*)`
  的 mono 便捷接口会收到三个相同指针，此时只向该缓冲贡献一次，增益取
  NR51 实际路由侧中的较大值，不会因指针相同而加倍。其他任意部分别名组合
  不在支持范围内。
* `NR52` 断电会从所有旧端点撤下持续电平，尾部归零；`gme_start_track`
  仍通过 `Multi_Buffer::clear()` 与 `Gb_Apu::reset()` 清掉上一曲状态。
* 整段 `end_frame(T)` 与按 1/7/257 等不规则时钟多次切段，事件的重采样
  时刻完全一致，输出逐样本相同。

## 构建与运行

先运行一次任务构建，生成 `.task-build/libtaskgme.a`：

```sh
bash task_support/build.sh
c++ -std=c++11 -O2 -I. -Igme \
    demo/gb_stereo_wav.cpp .task-build/libtaskgme.a \
    -o .task-build/gb_stereo_wav
.task-build/gb_stereo_wav gb_stereo.wav
```

也可以直接用 CMake 安装后的 libgme 编译本示例（包含 `gme/gme.h` 并链接
`gme`）。生成的 `gb_stereo.wav` 是 48 kHz、16-bit、双声道 PCM。

## 测试

```sh
bash task_support/run.sh target
```

`test/gb_stereo.cpp` 注册 `gain`、`transitions`、`playback` 三组用例：
四个振荡器的非零输出与左右比例/最低增益；用独立 Blip 脉冲参照核对同周期
改音量/路由、center 与单侧/分离端点转换、`NR52`、mono 接法以及
1/7/257 周期切段；以及经 `gme_open_data`/`gme_play` 的方向、非零声音、
重播清状态和碎块读取。
