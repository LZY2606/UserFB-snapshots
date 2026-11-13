# rg_etc1 / rg_etc2 — ETC1 与 ETC2 RGB8 离线压缩

本目录在原有 `rg_etc1` ETC1 压缩入口之外，新增了同一命名空间下的 ETC2 RGB8
压缩/解压接口（实现见 `rg_etc2.cpp`）。输出始终是标准的 8 字节 ETC2 RGB8
码流，不保存任何私有的模式标记；模式只能通过规范的 diff 位与红/绿/蓝差分
溢出优先级识别。

## 接口

```cpp
namespace rg_etc1
{
   enum etc2_mode { cETC2Individual, cETC2Differential, cETC2T, cETC2H, cETC2Planar };

   struct etc2_options
   {
      bool m_allow_t;        // 默认 true
      bool m_allow_h;        // 默认 true
      bool m_allow_planar;   // 默认 true
      etc1_quality m_quality; // 默认 cHighQuality
   };

   // 4x4 单块。rgba 为内存中 RGBA 字节顺序、行序的 16 个像素。
   // valid_mask 的第 4*y+x 位为 1 时该像素参与失真统计（边缘块补齐用）。
   // 返回值是“实际码流重建后”的 valid_mask 内 RGB 字节差平方和，
   // 不是未量化端点上的拟合误差。
   unsigned pack_etc2_block(void* block8, const unsigned* rgba,
                            const etc2_options& options,
                            unsigned valid_mask = 0xFFFF);

   // 解码。preserve_alpha 为 true 时保留目标 alpha，否则写 255。
   // mode 非空时返回识别出的模式。
   bool unpack_etc2_block(const void* block8, unsigned* rgba,
                          bool preserve_alpha = false,
                          etc2_mode* mode = nullptr);

   // 整图。块按“先行后列”输出，每块 8 字节；右下角不满 4x4 时边缘
   // 复制补齐，补齐像素不进搜索失真，行 padding 不会被读取。
   std::vector<unsigned char> pack_etc2_image(const unsigned char* rgba,
                                              unsigned width, unsigned height,
                                              size_t stride,
                                              const etc2_options& options);
}
```

约定与异常：

- 编码忽略 alpha，解码在 `preserve_alpha=false` 时把 alpha 置 255，
  与原 `pack_etc1_block` / `unpack_etc1_block` 的约定一致。
- `valid_mask == 0` 或 quality 非法时，`pack_etc2_block` 抛
  `std::invalid_argument`。
- `pack_etc2_image` 要求宽高非零、`stride >= width*4`，所有尺寸乘法都做
  溢出检查，非法时抛 `std::invalid_argument`。
- `pack_etc2_image` 是裸指针接口：**输入缓冲至少包含
  `stride*(height-1)+width*4` 个可读字节这一前提由调用方保证**，
  本函数不是带长度校验的解析器。
- 接口线程安全、无全局可变搜索状态、不修改输入；`pack_etc1_block_init()`
  仍可照常调用（新接口内部也会做一次幂等初始化）。

## 调用例子

```cpp
#include "rg_etc1.h"
using namespace rg_etc1;

void example_block()
{
   unsigned rgba[16];                 // 行序 4x4，RGBA 字节序
   unsigned char* p = reinterpret_cast<unsigned char*>(rgba);
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
      {
         unsigned char* q = p + 4 * (4 * y + x);
         q[0] = (unsigned char)(20 + 35 * x);
         q[1] = (unsigned char)(30 + 30 * y);
         q[2] = (unsigned char)(40 + 12 * (x + y));
         q[3] = 255;
      }

   etc2_options opt;                  // 三种新模式全开，cHighQuality
   unsigned char block[8];
   unsigned error = pack_etc2_block(block, rgba, opt);

   unsigned decoded[16];
   etc2_mode mode;
   unpack_etc2_block(block, decoded, false, &mode);
}

void example_image()
{
   const unsigned W = 5, H = 7, stride = 32;   // 每行带 12 字节 padding
   std::vector<unsigned char> image(stride * H, 0);
   // ... 填充 image，只写每行前 W*4 字节 ...

   etc2_options opt;
   auto blocks = pack_etc2_image(image.data(), W, H, stride, opt);
   // blocks.size() == ceil(W/4)*ceil(H/4)*8 == 32
}
```

## 模式与搜索质量

码流识别遵循规范优先级：diff=1 时先看红色 5 位基色加 3 位有符号 delta
是否溢出（T），再看绿色（H），再看蓝色（planar），都不溢出才是传统
differential；diff=0 是 individual。

- **individual / differential**：直接复用原 `pack_etc1_block` 的候选，
  保证三种新模式全部关闭时，满 mask、alpha 全 255、相同 quality、无抖动
  的输出与原接口逐字节一致。
- **T / H**：两个 RGB444 端点、8 档距离表 `{3,6,11,16,23,32,41,64}` 和
  每像素 2 位 selector 联合搜索。先用 k-means 在 444 量化空间求端点种子，
  再对端点各分量做坐标下降，每个候选都重新为 16 个像素选择最优 selector。
  H 模式按规范处理端点排序对距离索引最低位的贡献（必要时交换端点并翻转
  selector 的 MSB 平面）。
- **planar**：对每个通道做带权最小二乘平面拟合得到 O/H/V 三个 RGB676
  锚点初值，再对 9 个量化锚点分量做坐标下降。锚点位于参数坐标
  (0,0)、(4,0)、(0,4)，不在最后一个纹素中心；重建严格按
  `clamp(floor((x*(H-O)+y*(V-O)+4*O+2)/4), 0, 255)` 的整数公式。
- 所有候选（包括旧 ETC1 候选）最终都按**实际发出的 8 字节码流重建后**
  重新计算失真再比较，因此打开更多模式只会不变或更好，不会变差。
  搜索是启发式的（不枚举全部位串），但任何被选中的 T/H/planar 码流都能
  被标准模式判断重新识别。

selector 位保持列序存储（位索引 `x*4+y`，LSB 平面在码流第 4–5 字节、
MSB 平面在第 6–7 字节），与公开的行序像素数组之间在打包/解包处转换。

## 构建与测试

```sh
bash task_support/baseline.sh   # 原 ETC1 非对称渐变块回归（误差 13592）
bash task_support/target.sh     # decode / search / images / compatibility 四组
```

`target.sh` 会自动把 `rg_etc2.cpp` 与原源码一起编译。测试中的解码参照
独立按位域和整数公式实现，没有调用生产编码器，固定 T/H/planar/individual/
differential 块均为手工构造；搜索组会打印可复跑的输入像素、所选模式与
真实重建失真；images 组把小图解码结果写成 PPM（位于 `$TMPDIR`）。
