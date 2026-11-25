# Horde3D 双动作根运动播放器 (Dual-Action Root-Motion Player)

新增 `RootBlendPlayer`，把两个根运动动画的**平面体速度/转向速度**按时间权重混合后积分，
得到唯一的世界根轨迹；而不是插值两份绝对根坐标。骨骼其余部分仍走引擎现有的
同层非加性 TRS 混合。实现为纯 C++14、无窗口、无 GPU、无新增第三方库。

## 文件

- `Horde3D/Source/Horde3DEngine/egRootBlend.h` — 公开接口、输入/失败语义、资源寿命说明
- `Horde3D/Source/Horde3DEngine/egRootBlend.cpp` — 绑定校验、双精度导数、分段积分、姿态应用
- `examples/root_blend.cpp` — 最小可运行示例（程序化 H3DA + 蒙皮网格，头less）
- `tests/root_blend.cpp` — 五组共 10 个用例，独立双精度标量参照，经 `task_support/check.h` 注册

## 接口

```cpp
struct RootBlendKey { float time; float weight; };  // time 严格递增, weight∈[0,1]

class RootBlendPlayer {
public:
    enum { MaxQueryTime = 30 };
    bool bind( AnimationResource *anim0, AnimationResource *anim1,
               float fps0, float fps1, float phase0, float phase1,
               float rate0, float rate1,
               const char *rootJointName, ModelNode *model,
               const Matrix4f &startWorldTrans,
               const std::vector<RootBlendKey> &keys );
    bool evaluate( float time );                 // time∈[0,30]，可乱序/重复/倒序查询
    bool getWorldRoot( Matrix4f &outTrans ) const;
    void unbind();
    bool isBound() const;
};
```

权重 `w` 表示第二动作的占比，第一动作为 `1-w`；键间线性插值，范围外钳到端值。

## 输入约束（不满足即 `bind()` 返回 false，且不改动任何场景/资源/播放器状态）

1. 两个动画资源均已加载；模型与根关节名非空。
2. 每个动画 `N>=2` 帧；周期为 `[0,(N-1)/fps]`，最后一帧是周期端点，
   不额外补末帧到首帧的一段；动作时间 `phase + rate*t`，用 `floor` 取圈，
   `rate` 可为零（暂停）或负值（反向），`fps>0`，相位/速率/权重均为有限数。
3. 命名根关节在该模型下恰好存在一个，且是模型的直接子关节。
4. 根轨道只允许 XZ 平移、绕 Y 旋转、单位缩放：
   - 平移 `transVec.y==0`，缩放三分量均为 1，四元数 `x==z==0`、模长为 1；
   - 常量（压缩）根轨道允许。
5. 起始世界位姿是刚体且无倾斜：只含绕 Y 的旋转（无 tilt），Y 平移被保留。
6. 模型父节点链为刚体（正交旋转 + 平移，无缩放）；非均匀缩放只允许出现在
   与根轨道无关的旁支上。
7. 权重键时间严格递增，权重落在 `[0,1]`。

## 积分约定

- 每个动作在周期 `u∈[0,(N-1)/fps]` 上：位置线性插值，旋转为最短弧 nlerp。
- 体速度 `v = R(u)^T · dp/dt`，转向速度 `ω = dyaw/dt`，其中
  `dp/dt = rate·fps·(p[f+1]-p[f])`；导数全部在双精度下从实际插值曲线求取，
  不使用有限差分。四元数在提取姿态前规范化到 `w>=0`，消除双覆盖歧义。
- 混合 ODE：`dθ/dt = (1-w)ω0 + wω1`，
  `dx/dt = R(θ)·((1-w)v0 + w v1)`；世界根 `W=(R(θ),x)`，Y 坐标保持起始值。
- 时间轴先按权重键和帧段（整数播放头）边界切分，段内做固定细步长 RK4；
  每次查询都从零开始积分，因此任意切分/乱序查询得到同一条轨迹。

## 姿态应用（`evaluate` 内部）

1. 两个同层、非加性动画阶段用 `(1-w, w)` 驱动引擎现有 `AnimationController`
   的局部 TRS 混合（临时关闭快速动画路径，nlerp/线性插值路径）。
2. 根关节去掉全部平面运动：只保留高度（Y 平移）与绕 Y 旋转，平移 X/Z 置零、
   缩放置单位。
3. 把积分得到的世界根刚体换到模型父坐标系后设置模型变换；随后刷新
   `JointNode` 相对模型矩阵、inverse-bind 调色板，以及模型自身几何体上的
   软件蒙皮（`updateGeometry`）与 morph。没有另写蒙皮器。

## 失败与状态

- 非法资源、缺根、非有限参数、坏权重键、越界查询（`time∉[0,30]`）等都让
  `bind/evaluate` 返回 false；失败前不提交任何改动，已有的世界根保持原样，
  合法调用仍可继续，两个模型互不污染。
- `unbind()` 清空两个动画阶段（置 NULL）并释放独占，之后普通阶段播放恢复可用。
- 动画资源在绑定期间不会被写入；借用方负责其生命周期长于播放器。

## 构建 / 运行

```
bash task_support/run.sh target    # 构建引擎 + 示例 + 五组测试，并运行基线检查
bash task_support/run.sh baseline  # 仅保留原阶段动画、morph、CPU 蒙皮检查
```

`run.sh target` 期望输出每个 `GROUP <name> ... COMPLETE` 以及
`TARGET_COMPLETE cases=10`，并运行 `root_blend_example`（打印
`ROOT_BLEND_EXAMPLE_OK`）与原基线（`HORDE_MOTION_BASELINE_OK`）。
