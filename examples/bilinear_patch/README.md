# Bilinear patch 图元（nanort-patch.h）

`nanort-patch.h` 在 nanort 之上新增双线性四边形图元，不拆三角形、不引入第三方运行依赖。
本目录是一个无窗口的 C++ 绘制例子：一个非共面双线性面 + 一个带参数域矩形孔的平面四边形，
按 (u, v) 参数坐标与朝向法线着色，输出 `bilinear.ppm`（P6）。

构建与运行：

```sh
c++ -std=c++11 -O2 -I. examples/bilinear_patch/main.cc -o bilinear_patch_example
./bilinear_patch_example /path/to/bilinear.ppm
```

## 接口约定

- 图元面数据：`nanort::BilinearPatchMesh<T>`（`T = float` 或 `double`）。
  - 顶点布局与 `TriangleMesh` 相同：裸顶点数组 + `vertex_stride_bytes` 步长。
  - `faces` 每面 4 个顶点索引，顺序固定为 `p00, p10, p11, p01`，曲面为
    `P(u,v) = (1-u)(1-v)p00 + u(1-v)p10 + uv p11 + (1-u)v p01`，`u,v ∈ [0,1]`。
  - `trims` 可为 `NULL`；非空时每面 4 个值 `(u0, v0, u1, v1)`，表示参数域矩形孔。
    **孔的严格内部不属于表面，边界仍属于表面**。`u0 >= u1` 或 `v0 >= v1` 表示该面无孔。
- SAH 划分谓词：`nanort::BilinearPatchSAHPred<T>`，按四角质心划分，与 `TriangleSAHPred` 用法一致。
- 命中记录：`nanort::BilinearPatchIntersection<T>`，含
  - `t`：射线参数。调用者方向**不做归一化**，`t` 保持 `org + t * dir` 的含义；
  - `u, v`：参数坐标，可直接用于着色；
  - `normal[3]`：`normalize(dP/du × dP/dv)`，必要时翻转使其朝向射线来向；
  - `prim_id`：**输入面编号**，与 BVH 内部排列无关。
- 求交器：`nanort::BilinearPatchIntersector<T>`，可直接交给
  `BVHAccel::Build`/`Traverse`（配合上面的 mesh 与 pred）。
  `BVHTraceOptions` 的 `prim_ids_range`、`skip_prim_id`、`cull_back_face` 语义与三角形路径一致；
  背面剔除以几何法线 `dP/du × dP/dv` 指向的一侧为正面。
- 一条射线可与同一面相交两次：求交器先把参数域外、孔内、射线区间 `[min_t, t_inout]` 外的候选
  全部筛掉，再取最近的有效交点。`PrepareTraversal` 每次遍历重置全部候选状态，
  同一 intersector 连续处理不同射线不会残留上一次的参数。
- 包围盒取四角顶点的 AABB。双线性面位于四角的凸包内，因此该包围盒保守覆盖整张面。

## 数值退化行为

所有判定阈值都是**相对（尺度不变）**量，整体缩放场景不会把普通面误判成退化面：

- 参数域容差：`kBilinearPatchUVEpsilon<T>() = 16 * epsilon`，用于 `u,v ∈ [0,1]` 的判定
  （u、v 本身无量纲，绝对容差即相对容差）。
- 退化容差：`kBilinearPatchDegenerateEpsilon<T>() = 64 * epsilon`，以
  `|du × dv| <= eps * |du| * |dv|`（导数夹角正弦的尺度不变度量）判定候选点处两条参数导数
  线性相关、无法定义单位法线——该候选被拒绝，其余根仍会被检查。
  同一度量用于拒绝“等参线与射线平行”（u 无法唯一确定）和“射线与曲面在候选点处相切”。
- 二次方程 `A v² + B v + C = 0` 用数值稳定的 q 公式求解；`A == 0`（平面四边形、
  仿射参数化的平行四边形、或二次项极小的近平面面片）退化为线性方程求解，
  有限根不会丢失，也不会产生 NaN。`A == B == 0` 对应无解或射线与曲面重合，明确拒绝（未命中）。
- 完全退化的面（面积处处为零）与射线落在曲面上的重合情形同样走上述相对判定，明确拒绝。

输入假设：角点与射线有限、射线方向非零。
