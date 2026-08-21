# P5-CPU-REUSE-RAY-ROUND2 审计记录

- 审计日期（UTC）：2026-08-06
- 状态：`DONE-LOCAL`（审计后仅修复当前已准入 Vulkan 切片；CPU 定义未改）
- 对照基线：`D:\Codex_lib\code_reference\ViennaPS`
- 被审实现：[`gpu/vulkan/ray/vulkan_ray_flux_engine.cpp`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp)
- 规范来源：[`vulkan-program-intent-framework.md`](vulkan-program-intent-framework.md) §2.2/§12、[`vulkan-compute-acceleration-status.md`](vulkan-compute-acceleration-status.md) `P5-CPU-REUSE-RAY-ROUND2`
- 审计范围：source setup、边界处理、归一化、postprocessing；这是
  2026-08-06 的零反射历史审计快照。此后 bounded 一反射由其 Process
  card 接受，`P5-S1` 接受窄 `NeutralTransport<float,2>` frontier 切片，
  `P5-M0` 将这些准入事实汇总进矩阵；当前 `checkInput` 与
  [模型矩阵清单](p5-model-matrix-inventory.md) 才是准入现状的权威记录。

## 结论

**总体判定：`PARTIAL`，不能宣称“复用 ViennaRay/CPUTriangleEngine 主机辅助函数”。**

`VulkanRayFluxEngine` 复用了 CPU 引擎的 surface extraction、CPU fallback、
`ElementToPointData` postprocessing 类型以及 `calculateSurfaceFluxes`；但是
source construction、boundary traversal 和 normalization 是本叉的复制实现。
当前硬件 differential 只覆盖固定 seed 的 2-D、`gridDelta == 1`、单 bounce
平面夹具，不能关闭这些复制路径在 3-D、非单位网格、边界组合或自定义 source
上的差异。

## 符号对照矩阵

| 行为 | reference symbol（`code_reference`/ViennaRay） | Mod symbol | 复用 / 复制 / 漂移 | 证据与差异 | CPU differential | P5 处理 |
|---|---|---|---|---|---|---|
| 默认 source setup | `CPUTriangleEngine::initialize`；`TraceTriangle::apply` → `Trace::prepareSource`；`rayInternal::adjustBoundingBox` / `getTraceSettings`；`SourceRandom` | `impl_detail::DefaultSourceSetup::captureBox/buildDefaultSource`；`Impl::generateRays`；`initialize` 的 `sourceSetup_` 设置 | **复制（带 helper 复用）** | 调用了 ViennaRay 的 `adjustBoundingBox/getTraceSettings/SourceRandom`，但没有调用 `TraceTriangle`。custom source 由 `checkInput` 拒绝；particle 的 `initNewWithDirection` 未在 Mod 执行（当前 `SingleParticle` 未覆盖该方法，继承的默认实现返回零向量，故本夹具兼容）。Mod 注释称 CPU 首次 `runNumber` 为 0，但 ViennaRay `KernelConfig::runNumber` 初值为 1，`TraceKernel` 使用 `runNumber + rngSeed`；Mod 只使用 `rngSeed`（`generateRays` 392–409），固定 seed 的 ray population 并不逐 ray 相同。 | `ray_flux_process_route_smoke` 记录 Intel Arc：`totalRelDiff=0.29%`、`maxRelDiff=2.07%`、20,000/20,000 命中；仅是聚合容差，不是逐 ray 证明。 | 保持 eligibility gate；不得把聚合 differential 写成 source 等价。若需要逐 ray/多维等价，另开卡；本卡不改实现。 |
| 边界设置与反射/周期 | `CPUTriangleEngine::initialize` 的 `setBoundaryConditions/setMaxBoundaryHits`；ViennaRay `Boundary::processHit`（D=2 只消费 first boundary；D=3 消费两组面） | `Impl::boundaryConds_`、`maxBoundaryHits_`；`applyBoxBoundary`；`generateRays` 调用 | **复制（解析 host replica）** | Mod 没有把 boundary geometry 交给 ViennaRay，而是在 GPU dispatch 前推进 host ray。D=2 只显式处理 `firstDir`，与 ViennaRay D=2 的 first condition 形状相近；D=3 的两侧 slab、back-side pass-through、面 ID 顺序尚无证明。`applyBoxBoundary` 在 IGNORE/超限时返回高位失败标志，但 `generateRays` 忽略返回值并仍 push ray（440–451），与 CPU 终止语义存在潜在漂移。epsilon（`1e-5`/`1e-7`）也不是 ViennaRay 的同一交点路径。 | 现有 route 只使用一个 2-D plane/default boundary 夹具；没有 reflective、periodic、ignore、max-boundary-hit 矩阵，也没有 3-D Vulkan route（eligibility 拒绝 D=3）。 | 2-D fixture 可作为**已测兼容例外**，但 boundary helper 仍标 `PARTIAL`；D=3、失败标志消费和边界组合保持未证据。 |
| Flux normalization | `TraceTriangle::normalizeFlux` → `GeometryTriangle::getPrimArea`；SOURCE 因子使用 `sourceArea / (numPoints * raysPerPoint)` | `Impl::normalizeFlux`、`triangleArea` | **复制且存在已知几何漂移** | SOURCE/MAX 公式形状相同，且当前 API 只使用 `raysPerPoint`。但 ViennaRay D=2 `GeometryTriangle::getPrimArea` 对每个三角形使用 `0.5 * lineLength`，忽略 ribbon 的 Z 厚度；Mod `triangleArea` 使用完整 3-D cross product，即 `0.5 * lineLength * gridDelta`。因此非单位 `gridDelta` 时归一化不同；route 的 `gridDelta=1` 未暴露此差异。Mod 还对 MAX `maxv<=0` 直接 return，而 reference 会按 `maxv*area` 计算（零值行为不同），并未复现 reference 的 size assertion/source-null warning。 | 只有 `MakePlane(gridDelta=1)` 的总量/最大相对误差记录；没有 MAX 零通量、非单位 grid、SOURCE null 或 per-element exact check。 | 这是 `DRIFT`，不能标作“已兼容”。保持 CPU oracle 与 eligibility；修复需独立 card（不得在本审计文档中改生产代码）。 |
| source flux postprocessing | `CPUTriangleEngine::calculateSourceFluxes` → `runRayTracer`（逐 particle：`setParticleType/apply/normalizeFlux/mergeParticleData`）→ `saveElementFluxesToTriangleMesh` → `ElementToPointData::apply` | `VulkanRayFluxEngine::calculateSourceFluxes`（device hit accumulation、`normalizeFlux`、cell data 写入、同一 `postProcessing_`） | **postprocessing 类型复用；ray/particle 前处理复制** | `postProcessing_` 的 labels、conversion radius、mesh/KD-tree、disk mesh 设置与 CPU 相同，最终 `setPointData/setElementDataArrays/apply` 也相同。Mod 未调用 `setGlobalData`（coverage）；未调用 particle collision/reflection/init/log；按 label 复制同一 hit flux，并在有 log size 时合成全零 `DataLog`。这些仅在当前单粒子/单 label/零反射 predicate 下部分兼容；`SingleParticle::surfaceCollision` 不读取 globalData/material，且默认 log size 为 0，因此当前夹具不显现。D=3 intermediate VTK 写出也未复制。 | route smoke 只验证最终 label、size、总量和 max relative diff；未覆盖 coverage flag、material mask、particle logs、多 label、多 particle 或 intermediate output。 | `PARTIAL`：postprocessing 可称复用，ray/particle bookkeeping 不能。扩大模型矩阵前必须逐模型 CPU differential；当前 unsupported 继续 Auto CPU / Manual fail-closed。 |
| surface update / desorption fallback | `CPUTriangleEngine::updateSurface`、`calculateSurfaceFluxes` | `Impl::updateSurfaceMesh` + `VulkanRayFluxEngine::updateSurface`；`calculateSurfaceFluxes` 直接 `cpuEngine.calculateSurfaceFluxes` | **CPU delegated + device geometry copy** | Mod 先构造 device triangle/source box，再调用 CPU engine 的 `checkInput/initialize/updateSurface` 保持 CPU mesh、material IDs、desorption source 状态；surface-flux calculation 完全委托 CPU。device side 不上传 material IDs，但当前 eligible `SingleParticle` 的 hit accumulation 不读取 material ID。 | route smoke 覆盖 source flux，不覆盖 desorption；CPU delegated path 未单独在本卡重跑。 | 这是本卡最强的复用边界；不得把 device triangle copy 当作 ViennaRay geometry ownership。 |

## CPU differential 与证据边界

已记录的 P5 route 证据来自 `gpu/vulkan/ray/ray_flux_process_route_smoke.cpp`：

- CPU oracle：`FluxEngineType::CPU_TRIANGLE`，固定 `rngSeed=42`、
  `useRandomSeeds=false`、`maxReflections=0`；
- Vulkan：Intel Arc 真设备、prepared deployment context、同一 plane fixture；
- 结果：`totalRelDiff=0.29%`、`maxRelDiff=2.07%`、20,000/20,000 命中；
  Auto 不满足 predicate 时回退 CPU，Manual 未准备 context 时不沉积 flux；
- 该 smoke 的断言只有 `totalRelDiff < 2%`，不能证明逐 ray RNG、边界事件、
  三角形面积或 coverage/log 语义一致；状态看板中的 `0.286101%` 是同一类
  聚合证据的更精确打印值。

## 审计后本地修复与验收

在审计记录形成后，主线以本文件列出的 ViennaRay 实现为准完成了当前准入
`SingleParticleProcess<float,2>` 切片的最小修复：

1. `generateRays()` 以 `KernelConfig::runNumber == 1` 开始，并在每次成功
   Vulkan source-flux dispatch 后推进；固定 seed 与 CPU `TraceTriangle` 的
   首次/后续调用顺序一致。
2. 消费 `applyBoxBoundary()` 的高位终止标志；终止光线不会上传，但
   `totalRays` 仍作为 SOURCE 归一化分母，符合 CPU 的已发射光线语义。
3. D2 `triangleArea()` 按 ViennaRay 的偶/奇 ribbon 三角形对应边长计算
   `0.5 * lineLength`；`MAX` 不再把零通量短路为未归一化值。

`ray_flux_process_route_smoke` 已改为 `gridDelta=0.5`，在本地 Intel Arc
真设备运行通过：`totalRelDiff=0`、`maxRelDiff=0`。该证据只接受当前
eligibility gate；它不把 host replica 推广为通用 ViennaRay 替代。

## 未关闭项目（不得写成兼容）

1. 为 source 与边界 replica 建立逐 ray source differential，以及
   reflective/periodic/ignore、max-boundary-hit 的 CPU 对照夹具；
2. 为 `NormalizationType::MAX` 零通量行为补 CPU 对照夹具；
3. 在扩大 eligibility 以前，为 coverage、material IDs、particle logs、
   多粒子/多 label、desorption 和 D=3 分别建立 CPU oracle。

## 审计命令

本轮仅做只读定位与文档校验：

```powershell
codegraph explore "VulkanRayFluxEngine::checkInput initialize updateSurface calculateSourceFluxes calculateSurfaceFluxes generateRays getOriginAndDirection applyBoxBoundary"
codegraph explore "CPUTriangleEngine initialize generateRays calculateSourceFluxes updateSurface normalizeFlux source setup"
git diff --check -- docs/design/p5-cpu-reuse-ray-audit-round2.md
```

`git diff --check`：通过（无 whitespace error）。

本文件没有执行或宣称新的硬件测试；引用的 route 数值仅来自已有状态记录与
smoke 输出。任何实现修复必须由新卡授权，不能在本审计卡内顺手改 production。
