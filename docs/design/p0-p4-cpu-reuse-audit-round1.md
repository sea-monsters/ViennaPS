# P0–P4 / PD0–PD4 第一轮审计记录

- 审计轮次：**第一轮（Round 1）**
- 日期（UTC）：2026-08-05
- 状态：`RECORDED`（结论已入账；R1-F1 / R1-F2 已在本轮修复并经本地 CPU 与 Vulkan smoke 复验关闭，R1-F3 保留建议）
- 审计主题：Vulkan 计算迁移下，**非计算路径是否复用 / 兼容旧 CPU 路线**
- 对照基线：`D:\Codex_lib\code_reference\ViennaPS`（ViennaPS 4.6.2 原版）
- 工作树：`D:\Codex_lib\ViennaPSMod`
- 规范来源：
  - [意图白皮书 §2.2 第 8 条](vulkan-program-intent-framework.md)
  - [开发报告 §12](vulkan-compute-acceleration-development-report.md)
- 范围：P0–P4 与 PD0–PD4 已接受本地切片（控制面、原语、Level Set 缝、HRLE、表面绑定、P4 ray 设备链）；**不含** P5 Process 光线生产路由结案

本文件是 P0–P4 阶段开发的**第一轮审计正式账本**。意图白皮书、开发报告与状态看板中的摘要均指向本文件；后续轮次另开 `…-round2.md`（或在本文件追加轮次节），不得用局部改写抹掉本轮结论。

---

## 1. 审计问题

> 除真正的 compute 操作外，编排、模型、默认 CPU 引擎、主机侧输运辅助与 Advect 事务是否仍复用或兼容原版 CPU 路线？空 executor / Manual CPU 是否回到原版行为？是否存在无文档的共享 CPU 路径语义分叉？

---

## 2. 总评

**总体合规（架构未另起 Process / Flux / SurfaceModel 生产循环）。**

默认生产路径仍为 `psCPU*Engine` + `viennals::Advect`；Vulkan 以控制面选择、原语、以及可选 executor / 绑定注入。残留问题：

1. **PARTIAL** — HRLE rebuild 分类/压缩等为语义移植的 CPU+GPU 契约端口，非直接调用 Advect 私有 rebuild 体；
2. **PARTIAL（可接受于本阶段）** — P4 ray 独立 kernel `runCpu`，生产通量仍走 CPU 引擎；
3. **DEVIATION** — P3K 修改了**共享** `AdvectionHandler::performAdvection` 的早退/失败语义，即使无 Vulkan executor 也相对原版改变行为。

本轮**已将 R1-F1、R1-F2 标记为关闭**；R1-F3 保留至 P5+。

---

## 3. 分区结论表

| 区域 | 判定 | 证据要点 | 数值/兼容风险 |
|---|---|---|---|
| `include/viennaps/compute/` | **合规** | 档案、策略、探测；无工艺语义 | 低 |
| `gpu/vulkan/runtime/` | **合规** | Session / DeploymentContext；不接管 `Process::apply` | 低 |
| `gpu/vulkan/primitives/` | **合规** | 纯计算原语 + kernel-contract CPU oracle | 低（oracle ≠ 生产引擎） |
| PD0–PD2 表面覆盖度/扩散/中性缝 | **合规** | 编排仍在 CoverageManager / FluxProcessStrategy / SurfaceModel；空 executor = CPU；`psSurfaceDiffusion.hpp` 与原版一致 | 低 |
| PD3–PD4 组合/矩阵/基线 | **合规** | 生命周期与证据；无算法重写宣称 | 低 |
| `psCPUDiskEngine` / `psCPUTriangleEngine` | **合规** | 与原版一致，仍为生产默认 | 低 |
| P3 LS update via Advect executor | **基本合规** | 空 executor = 原 Advect；RK2/RK3 强制 CPU | 低 |
| P4 HRLE `psHrleRebuild*.hpp` 等 | **部分偏离** | 独立 CPU 契约 + Vulkan 核；球体 bit-exact oracle 存在；非 Advect 私有体直接调用 | 中（上游 ViennaLS 漂移） |
| P4 ray 设备链 | **部分（本阶段可接受）** | `intersectCpu` / `runCpu` 仅 kernel 差分；生产通量仍 `psCPU*` | 低（若误当 ViennaRay 等价则中） |
| **P3K `psAdvectionHandler::performAdvection`** | **偏离** | 相对原版：executor 错误传播；非有限/负步长 → `FAILURE`；零进度 → `EARLY_TERMINATION`；扩展零速度哨兵。共享 CPU 路径即生效 | 中（病态/零速度边界兼容） |

---

## 4. 关键偏离明细（P3K）与修复

修复方式：**按 executor 活性分路**。`performAdvection` 检测
`levelSetUpdateExecutor` 或 `levelSetRebuildExecutor` 是否为空：
- **空 executor 分支**：恢复 `code_reference/ViennaPS` 4.6.2 原版行为；
- **executor 活性分支**：保留 P3K fail-closed 守卫。

| 项 | 原版 (`code_reference`) | 本叉修复前（P3K） | 修复后行为 |
|---|---|---|---|
| 零速度（`timeStep == double::max()`） | 警告，时间跳满，仍 `SUCCESS` | 识别 `double::max` 与 `NumericType::max` | executor 活性分支保留双哨兵；空 executor 仅识别 `double::max` |
| 非有限 / 负 `timeStep` | 仍累加并 `SUCCESS` | `FAILURE` | 空 executor 分支恢复累加 `SUCCESS`；executor 活性分支仍 `FAILURE` |
| 零进度 / 时间不前进 | 仍累加（含 `+= 0`）并 `SUCCESS` | `EARLY_TERMINATION` | 空 executor 分支恢复 `SUCCESS`；executor 活性分支仍 `EARLY_TERMINATION` |
| LS update/rebuild/time 错误 | 无对应检查 | 显式 `FAILURE` + 日志 | 仅在 executor 活性分支检查 |
| `totalAdvectionSteps_` | `apply` 后即 `++` | 仅成功推进或零速度早退路径计数 | 空 executor 分支恢复无条件 `++`；executor 活性分支保留条件计数 |

触发文件：
- `include/viennaps/process/psAdvectionHandler.hpp`
- `cmake/patches/viennals-v5.8.5-levelset-update-v2.patch`

验证：
- `tests/advectionProgressGuard` 新增空 executor legacy 场景，断言 `SUCCESS`、
  `processTime == 0.0`、`timeStep == 0.0`、`advectionSteps == 1U`。
- `tests/advectionInnerLoopGuard` 默认注入 FALLBACK executor 验证 P3K fail-closed，
  并新增无 executor legacy 正速度/零进度场景。
- `tests/levelSetUpdateExecutorRouting` 的 `NONE` 模式已覆盖空 executor 默认 CPU
  路径，并验证成功单步步数 `advectionSteps == 1U`。
- 2026-08-05 复验：focused CTest 在 `Debug` 配置运行
  `advectionInnerLoopGuard`、`advectionProgressGuard`、
  `hrleRebuildClassification`、`hrleRebuildCompaction`、
  `hrleRebuildCpuFixture`、`hrleSparseReconstruction` 与
  `levelSetUpdateExecutorRouting`，结果 **7/7 通过**。重建后的 ViennaLS patch 已用
  `git apply --check` 对干净 5.8.5 源树验证。
- 2026-08-05 追加复验：`advectionInnerLoopGuard` 在 `Release` 配置运行
  `core` 与 `rk2-no-progress`、`rk3-no-progress`、`recovery`、`rk2-max`、
  `rk3-max` 全部通过；`hrleRebuildCpuFixture` 2-D/3-D fingerprint 分别为
  `0x0e839fa59b04e247` / `0x899ac3f2b0e90b79` 且 reference/mirror 位模式一致。
- 同日 Vulkan-only 配置显式保持 `VIENNAPS_USE_GPU=OFF`（不启用遗留 CUDA
  后端）；修正 level-set composition smoke 对 surface target 的 CMake 注册依赖后，
  controller、controller-execution、composition、composition-execution 四个 smoke
  可构建并全部通过。composition-execution 报告五个 callback 共享 generation/device
  且 CPU oracle 通过；这仅是本机运行证据，不替代远端 CI。

---

## 5. 本轮纠偏结果

| ID | 状态 | 实现与验证 |
|---|---|---|
| R1-F1 | **已关闭** | `psHrleRebuildClassification.hpp`、`psHrleRebuildCompaction.hpp`、`psHrleSparseReconstruction.hpp`、`gpu/vulkan/levelset/viennals_rebuild_executor.hpp` 已添加冻结镜像声明；`tests/hrleRebuildCpuFixture` 经 Advect rebuild callback 直接调用本地 classify/compact/reconstruct 镜像，显式断言 callback 已执行，并与未安装 callback 的原生 `Advect::rebuildLS` 逐 HRLE 位模式及 PointData 差分；固定 2-D/3-D fingerprint 分别为 `0x0e839fa59b04e247` / `0x899ac3f2b0e90b79` |
| R1-F2 | **已关闭** | P3K 改为 executor 活性分支专属 fail-closed；空 executor / Manual CPU 恢复原版 ViennaPS 4.6.2 行为。ViennaLS integration 在无 executor 时不再绕过原版 update/rebuild/lower-layer 序列；有 executor 的多步 `advectionTimeError` 现在还原整次操作的 snapshot，避免失败结果发布部分 step 状态；相关回归测试已拆分 legacy / executor-active 路径，并在 Debug 与 Release 配置复验通过 |
| R1-F3 | 保留建议 | P5+ 光线 Process 路由优先调用 ViennaRay / `CPUTriangleEngine` 主机辅助，减少归一化等副本；属后续卡，不在本轮关闭 |

---

## 6. 本轮不做的宣称

- 不宣称 P0–P4「已完全无偏离」（R1-F3 仍为开放建议）；
- 不宣称 R1-F1/R1-F2 的验证等同于远端 CI 或硬件矩阵；
- 不把本审计当作远端 CI 或发布门证据；
- 不以 mprocess 数值代替 CPU oracle。

---

## 7. 签名栏（记录元数据）

| 字段 | 值 |
|---|---|
| 审计轮次 | Round 1 / 第一轮 |
| 审计日期 | 2026-08-05 |
| 修复日期 | 2026-08-05 |
| 审计人 | 开发会话（对照原版树只读审查） |
| 修复人 | 开发会话 |
| 方法 | 状态看板 ownership 对照 + 工作树 vs `code_reference/ViennaPS` 关键路径差分 + 分区打分；按 executor 活性分路修复 P3K；冻结镜像标签 + 差分夹具修复 R1-F1 |
| 下一轮触发 | 进入 P5-DEPLOYMENT-EXIT 前再审一轮，或 P5+ 光线 Process 路由验证 R1-F3 时 |

---

*本文件为 P0–P4 第一轮审计的权威结论记录。摘要同步见状态看板文首、开发报告 §12.2、意图白皮书 §12.1。*
