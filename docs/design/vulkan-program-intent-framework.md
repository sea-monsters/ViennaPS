# ViennaPS 全程序功能意图框架白皮书

- 状态：活文档（随卡片完成演进）
- 产品语义基线：`D:\Codex_lib\code_reference\ViennaPS`（ViennaPS **4.6.2**，迁移前原版）
- GPU 嵌入方案参考：`D:\mprocess`（本地生产研究快照；开发报告中的 `${MPROCESS_SOURCE_DIR}`）
- 工作树：`D:\Codex_lib\ViennaPSMod`（Vulkan 能力驱动加速叉）
- 术语表：[CONTEXT.md](../../CONTEXT.md)
- ADR：[ADR-0001](../adr/0001-add-capability-driven-vulkan-backend.md)
- 配套账本：
  - [开发报告](vulkan-compute-acceleration-development-report.md) — 架构、数值策略、mprocess 评价、P0–P7 计划
  - [状态看板](vulkan-compute-acceleration-status.md) — 卡片级证据与独占边界

本文件是 **全程序功能意图与本次 Vulkan 迁移总体意图规划** 的单一横向视图：

1. 原版 ViennaPS 的框架结构、逻辑流程与设计意图（**算什么**）；
2. mprocess 提供的后端 GPU 加速**嵌入方案**经验（**如何常驻、融合、分阶段加速而不重写产品语义**）；
3. 本叉在其上叠加的 Vulkan 控制面与设备执行层（**谁执行、如何选择、如何回退**）；
4. 迁移过程中必须守住的 **功能不漂移契约**。

本文件**不取代**开发报告与状态看板：

| 文档 | 职责 |
|---|---|
| **本白皮书** | 功能意图、总体设计理念、调用链、模型契约、mprocess 借鉴边界、漂移防线、全局不变量 |
| 开发报告 | 架构决策细节、数值/验证策略、人周估算、目标形态、mprocess 条目级评价 |
| 状态看板 | 卡片验收命令、CPU oracle、硬件证据、ownership |

任何 Vulkan 加速代码或记录改动前，先读本文件 → 状态看板 → 开发报告；需要确认嵌入模式时查阅 `D:\mprocess` 对应模块，但**不以 mprocess 物理结果为真值**。卡片完成后更新状态看板，并在本文件执行快照日志追加一行。

---

## 0. 白皮书用途：防功能漂移

Vulkan 迁移的风险不是“写不出设备核”，而是**在换后端时悄悄改变工艺语义**，或把某一 CUDA 研究实现的后端抽象/物理近似误当成 ViennaPS 权威。本白皮书把原版行为定为产品意图真值（intent oracle），把 mprocess 定为**嵌入与常驻模式参考**，把本叉新增层定为可替换执行面。

**意图真值来源（优先级）：**

1. `D:\Codex_lib\code_reference\ViennaPS` 中的公共头文件与策略/引擎实现（产品语义）；
2. 同一工件在 `CPU_DISK` / `CPU_TRIANGLE` 上的数值结果（非 CUDA 主机上的正确性 oracle）；
3. 已文档化的 tolerance / 位级契约（开发报告与状态看板）；
4. `D:\mprocess` 的数据流、常驻、融合、分层等价与运行时治理经验（**仅嵌入方案**；物理结果为 provisional）；
5. 本叉仅在明确卡片范围内扩展的 fail-closed / 选择策略行为。

**漂移判定启发式：** 若去掉 Vulkan/部署层后，用户可见的 Process API、策略路由、模型插件契约、通量→速度→平流语义与原版不一致，则构成功能漂移——即便设备路径自洽、更快，或与 mprocess 某一数值更接近。

---

## 1. 总目标（单一事实表述）

在**不牺牲正确性**的前提下，让 ViennaPS 的 process 在具备合格 Vulkan 计算能力的设备上自动选择设备执行，并在能力不足时**逐阶段 fail-closed 回退到 CPU**。

- 正确性 oracle：非 CUDA 主机上，**CPU 结果是唯一正确性基准**。设备路径必须在既定 tolerance/位级契约下与 CPU 对齐。
- 失败语义：任何阶段无法满足其设备契约时，**不得产生静默错误几何**；该阶段回退 CPU 或显式失败，其余阶段不受影响（per-stage eligibility）。
- 发布形态：分切片发布（Preview → Full Physics），每个切片带显式支持矩阵，**不以“编译通过”代表“功能支持”**。
- 共存形态：保留 CPU 与 CUDA/OptiX；Vulkan **不是**逐文件翻译 CUDA，也不是替换物理模型层。

原版程序的产品意图不变：微电子工艺/形貌仿真——多层 level-set 表面演化，可选 Monte Carlo 射线通量，以及氧化等自管耦合物理。加速只改变**谁执行**，不改变**算什么**。

---

## 2. 总体开发意图规划与设计理念

本节是本次 Vulkan 迁移的**总体意图规划**：双参考基线如何分工、从 mprocess 学什么/不学什么、以及由此固化的工程设计原则。细节与条目级评价以开发报告 §2.2 / §3 / §6.2 / §7 / §8 为准。

### 2.1 双基线分工（不可混淆）

```text
code_reference/ViennaPS     →  产品语义真值（Process / 模型 / 策略 / LS 权威）
D:\mprocess                 →  GPU 嵌入与常驻方案参考（数据流 / 融合 / parity 分层 / worker）
ViennaPSMod                 →  在 ViennaTools 契约上实现能力驱动的 Vulkan 执行面
```

| 问题 | 问谁 |
|---|---|
| 工艺该怎么编排？模型怎么挂？通量怎么变速度？ | 原版 ViennaPS |
| 如何让多阶段尽量留在设备上、少同步？如何分层证明等价？ | mprocess（模式），再映射到 Vulkan/SPIR-V |
| 无 CUDA 的 Intel/AMD 设备如何加速？如何 Auto/Manual？ | 本叉 + ADR-0001（mprocess **不能**直接回答） |

mprocess 证明了：**更广的 GPU 常驻覆盖可以改善流程**；但它使用 CuPy / CUDA 12 / NVRTC，`DeviceArray` 实际约束为 CuPy 数组，**不是** Vulkan/OpenCL/HIP 抽象，也**没有**解决无 CUDA 环境。因此本迁移的战略是：

> **借用 mprocess 的嵌入哲学与领域数据流，拒绝借用其后端抽象与物理真值地位；在 ViennaPS 原版语义之上，用能力驱动的 Vulkan 实现跨厂商加速。**

### 2.2 核心设计理念（七条）

1. **嵌入，不重写产品**  
   不把 ViennaPS 改成另一个仿真器，不引入第二套工艺模型权威。加速通过 FluxEngine / executor / deployment session **注入**原版策略主循环，保持 `Process::apply` 用户面。

2. **能力驱动，而非“有 GPU 就全开”**  
   部署或首次使用生成持久化 Capability Profile；运行时按阶段过滤功能阈值、精度、显存与校准结果。Manual 覆盖 Auto，但不可静默绕过正确性/安全检查。  
   选择单位是**可共享 Device Working Set 的阶段组**，不是单 kernel——避免小算子因上传开销被错误选到 GPU。

3. **Host Canonical + Device Working Set**  
   ViennaHRLE/ViennaLS 仍是主机权威场。设备持有稀疏砖块窄带或（仅当预算允许的）稠密结构化工作集；字段带 host/device generation、owner、dirty range。一次流程循环内尽量只做初始上传、必要 callback 交互与最终提交——这是对 mprocess `fields` / `stack` 常驻经验的直接采纳。

4. **四库协同契约，禁止 ViennaPS 内部复制 runtime**  
   加速链横跨 ViennaCore / ViennaLS / ViennaRay / ViennaPS。公共契约只表达共有的 buffer、dispatch、reduction、scan、sort、sparse matrix、ray query；CUDA 与 Vulkan 在契约下保留各自优化。**不做**“能表达任意 GPU API 的庞大虚基类”。

5. **分层等价，禁止单一“差不多”宣称**  
   采纳 mprocess / provisional-verification 的分层思想，映射为本项目层级：  
   Kernel contract → Strict discrete parity → Backend numerical parity → Geometry parity → Physics acceptance。  
   mprocess 的 `1e-4 voxel` strict / `1e-2 voxel` 独立 ray 数值等价仅作**初始候选**，须按 ViennaPS 各模型自然方差写入 tolerance manifest，不直接当全库统一阈值。mprocess 物理结果为 provisional baseline，**不得**替代 CPU oracle。

6. **Compute 为基线，硬件光追为可选层**  
   `VULKAN_COMPUTE`（含软件 BVH）是广覆盖基线；`RAY_QUERY` / `RT_PIPELINE` 是可选更高层。无硬件 RT 的设备仍应能跑已声明的 Compute/软件 BVH 流程——避免把 OptiX 假设带进 Vulkan 路径。

7. **分切片发布 + 显式支持矩阵**  
   Compute Preview → Level Set Preview → Process Preview → Full Physics。每个切片标注支持/不支持/回退行；完成定义见开发报告 §11。在此之前只用 Preview / experimental 名称，避免总开关暗示完整覆盖。

8. **非计算路径复用旧 CPU 路线（硬要求）**  
   本次开发目标是 **Vulkan 计算迁移**，不是重写工艺语义。除真正的 **compute 操作**（设备核、原语 dispatch、设备侧归约/扫描/排序/相交等）外，其余功能必须通过 **复用或兼容** 旧 CPU 路线实现，以提高数值一致性与向后兼容性：
   - **必须复用**：`Process` / Strategy 编排、`ProcessModel` / `SurfaceModel` 插件契约、`psCPU*Engine`、ViennaRay 采样与归一化主机逻辑、`viennals::Advect` 事务边界、材料/Domain/I/O/Python 绑定。
   - **允许替换**：仅 executor / FluxEngine 注入点背后的数值核；空 executor / Manual CPU 时行为须与原版 CPU 路径对齐。
   - **禁止**：另起一套 Process 循环、平行重写 SurfaceModel 化学、静默改写时间推进/早退语义、把 kernel-oracle CPU 副本当作生产 ViennaRay/Advect 等价物而不加标签。
   - **兼容例外**须显式文档化（卡片 ID + 与 `code_reference/ViennaPS` 的差分理由 + 回归测试），不得以“Vulkan 需要”为借口默认改写共享 CPU 路径。

   P0–P4 第一轮合规审计正式账本见
   [p0-p4-cpu-reuse-audit-round1.md](p0-p4-cpu-reuse-audit-round1.md)；摘要见本白皮书 §12.1。

### 2.3 从 mprocess 借鉴什么（嵌入方案清单）

开发报告 §2.2 的评价在此上升为**意图级采纳决定**：

| mprocess 经验 | 参考位置（`D:\mprocess`） | 本迁移的意图用法 | 对应阶段 |
|---|---|---|---|
| 设备数据跨阶段常驻 | `backend/mprocess/fields.py`、`stack.py` | 显式 Device Working Set + 脏标记；禁止每算子边界下载 | P3–P7，P7 强化跨步常驻 |
| Level Set 融合核 | `backend/mprocess/levelset.py` | 差分/速率/RK 按带宽热点融为 SPIR-V compute | P2–P3 |
| 稀疏砖块实验 | `backend/mprocess/hybrid/` | 砖分类/邻接/裁剪/stencil；**禁止**全局稠密 SDF 成为权威 | P2–P3 |
| 表面扩散 | `backend/mprocess/surface_diffusion.py` | 窄带融合 stencil；图拉普拉斯走 CSR/邻接 | P5 surface |
| GPU 常驻 BVH8 | `backend/mprocess/physical_etch/disk_bvh.py` | 借鉴数据语义与差分测试；**不**复用单线程确定性 builder 作性能实现 | P4–P5 ray |
| 粒子输运与 scatter | `backend/mprocess/physical_etch/transport.py` | 映射为 compute BVH 或 Vulkan RT；保留守恒 scatter | P4–P5 |
| 氧化 BiCGSTAB | oxidation diffusion/deformation 节点 | source-order reduction、收敛标量过主机、ILU 实验路径 | P6 |
| 运行时治理 | `backend/mprocess/worker.py`、`profiling.py` | 预热上下文、持久 pipeline cache、可疑设备失效、阶段事件计时 | PD*/P7 |
| 分层等价性 | `docs/baselines/provisional-verification.md` | 严格离散 / 独立后端数值 / 最终几何分宣称 | 全程验证 |

**移植对象：** 契约、数据流、诊断方法、常驻边界。  
**禁止移植：** 把 CuPy `DeviceArray` 抽象当跨后端 ABI；把 CUDA 源字符串机械翻译成 Slang；把 mprocess 数值当新物理真值；为复现某源顺序而牺牲本项目已约定的确定性/并行归约策略（除非卡片明确要求 strict source parity 调试）。

### 2.4 明确拒绝的路线（负面意图）

| 拒绝项 | 理由 |
|---|---|
| 用 Vulkan 替换 CUDA/OptiX | 会回归成熟 NVIDIA 部署，并失去 CUDA 对照面 |
| 只在 ViennaPS 顶层孤岛实现 Vulkan | LS/ray/分配/求解所有权已跨库 |
| 把稠密 SDF 设为权威表示 | 改变 ViennaHRLE 内存缩放；稠密仅作有界 workload 选项 |
| 要求硬件 RT 扩展才能用 Vulkan | Compute 广覆盖；软件 BVH 是基线 |
| 以 mprocess 后端抽象为模板 | 其 DeviceArray≈CuPy，不解决无 CUDA，也不适合 ViennaTools 契约 |
| 以“GPU 覆盖率”为 KPI | 小几何、I/O、复杂 callback、极小 CSR 等本就不适合 GPU（开发报告 §5.8） |
| 编译通过 / 本地 smoke = 功能支持或远端 CI | 支持矩阵与远端 run 证据不可替代 |
| 另起非计算编排 / 平行重写 CPU 工艺逻辑 | 破坏与原版数值一致性与向后兼容；仅允许替换 compute 核 |

### 2.5 嵌入架构意图（如何挂进原版）

目标形态（开发报告 §3）：

```text
Simulation request
  → Manual? validate : load Capability Profile
  → classify workload by stage → filter thresholds → score
  → Execution Plan / Selection Record
  → ViennaPS orchestration（原版策略循环不变）
       → ViennaLS / ViennaRay / ViennaCore kernels
       → Device Working Set（跨相邻阶段常驻）
```

相对 mprocess 的“嵌入”差异化要点：

- mprocess：Python 服务内 CuPy 常驻流程 + 产品文档/flow 编排；
- 本叉：C++ 头文件优先的 ViennaPS Process 编排不变；Vulkan 经 **deployment context + FluxEngine override / ProcessDeploymentBinding / LevelSet controller** 挂入；公共头不暴露 Vulkan 类型（PIMPL）。

因此“嵌入成功”的判据不是“代码里出现了 Vulkan”，而是：

1. 原版策略主循环步骤顺序仍可在白皮书 §6（逻辑流程）对齐；
2. 同一 fixture 上 CPU oracle 可复核；
3. 不合格阶段 fail-closed 或回退，且 Selection Record 可解释；
4. 长流程能体现跨阶段常驻收益（P7），短作业允许 Auto 仍选 CPU。

### 2.6 总体意图路线图（规划层）

与状态看板卡片一一对应的**意图主题**（非验收证据）：

| 意图主题 | 规划含义 | 主要借鉴 |
|---|---|---|
| PD0–PD4 控制面 | 探测、档案、策略、硬件矩阵、性能基线——先能“选”，再能“跑” | mprocess worker/profiling；拒绝其 CuPy 抽象 |
| PD5 CI/install | 构建卫生与本地边界；远端证据独立 | 路径隔离；不模拟 CI |
| P5-JD/JE/JF | 设备驻留光线链、单 submission、严格 FP32 | 常驻与提交边界；分层等价的 kernel/strict 层 |
| P5-RAY-ROUTE | 第一次把设备链**嵌入**真实 Process（单弹射） | 嵌入缝，非新策略 |
| P5-RAY-PHYSICS | 输运物理对齐 ViennaRay / 原版模型 | transport/scatter 语义；非 mprocess 真值 |
| P5-SURFACE-INTEGRATION | 覆盖度/扩散/中性速度与 ray **同 session 常驻** | fields/stack 常驻；surface_diffusion |
| P5-MODEL-MATRIX | 显式支持矩阵，防总开关幻觉 | capabilities 式“宣称与证据分离”思想 |
| P5-DEPLOYMENT-EXIT | Preview 可交付 | 分切片发布 |
| P6 | 氧化与 FP64 耦合求解上设备 | BiCGSTAB 数据流；per-stage FP64 |
| P7 | 跨步常驻、成本模型、soak、恢复、发布门 | 常驻收益 + Selection Record 重放 |

单人执行顺序与卡片边界仍以状态看板为准；本节只冻结**为什么按这个顺序做**。

### 2.7 完成定义（意图层摘要）

正式声明“ViennaPS 支持 Vulkan 计算加速”之前，必须同时满足开发报告 §11 的条件摘要：

- CPU / CUDA / Vulkan 可独立或共存构建；
- 探测、档案失效、Auto/Manual 有文档与测试；
- 公开支持的模型通过场/几何/物理验收；
- 无硬件 RT 但满足 Compute 阈值的设备可跑已声明流程；
- 缺 FP64 等只排除相关阶段；
- 每次运行有可重放 Selection Record；
- Auto 满足正确性、显存与 break-even；
- device lost / OOM / 不支持特性 / 收敛失败可诊断；
- 旧 CPU/CUDA 兼容测试与支持矩阵文档齐全。

在此之前，对外表述保持 Preview / experimental，与 mprocess 区分 claim strength 的做法一致：**能力宣称强度不得超过证据强度**。

---

## 3. 程序身份与目录地图

### 3.1 原版（意图基线）

路径：`D:\Codex_lib\code_reference\ViennaPS`

| 路径 | 意图角色 |
|---|---|
| `include/viennaps/` | 公共 API 与几乎全部编排逻辑（头文件优先 / INTERFACE） |
| `lib/` | 可选预编译特化（`specProcess` / `specModels*` / `specGeometries`） |
| `gpu/` | CUDA/OptiX 粒子 callable 与 GPU 基准 |
| `python/` | 同一 C++ API 的 pybind11 绑定，不另建执行路径 |
| `examples/` / `tests/` | 端到端工艺示例与 CTest 聚焦测试 |
| `cmake/` | 依赖拉取（ViennaCore / ViennaLS / ViennaHRLE / ViennaRay / ViennaCS） |

依赖边界意图：ViennaPS 编排工艺；ViennaRay 负责粒子输运与相交；ViennaLS/HRLE 负责 level-set 权威表示与平流；ViennaCore 提供日志、智能指针、设备上下文等基础设施。

### 3.1.1 库外嵌入参考（mprocess）

路径：`D:\mprocess`（开发期经 `VIENNAPS_MPROCESS_SOURCE_DIR` 注入，**不**进入本仓库树）。

| 路径 | 对本迁移的意图角色 |
|---|---|
| `backend/mprocess/` | GPU 常驻流程、场语义、蚀刻输运、hybrid 砖块、worker 治理 |
| `docs/baselines/provisional-verification.md` | 分层等价与 claim strength 分离 |
| `docs/architecture/overview.md` | 产品/执行/数值边界（对照“嵌入而非重写”） |
| `docs/decisions/` | 其自身 ADR；可启发、不自动继承为本仓库决策 |

详见 §2 与附录 D。mprocess **不是** ViennaPS 语义分叉，也不是可链接依赖。

### 3.2 本叉增量（执行面扩展，非产品语义重写）

路径：`D:\Codex_lib\ViennaPSMod`

| 增量 | 意图角色 |
|---|---|
| `include/viennaps/compute/` | 能力档案、部署决策、逐阶段后端选择策略（公共控制面，不暴露 Vulkan 类型） |
| `gpu/vulkan/` | 可选 Vulkan 运行时、原语、光线链、表面/LS 执行器与 smoke |
| `include/viennaps/process/` 中的 executor / override 缝 | 在不改策略主循环语义的前提下注入设备实现 |
| `include/viennaps/levelset/`（若存在） | LS 相关扩展挂点 |
| `docs/design/` + `docs/adr/` | 迁移意图与验收账本 |

构建意图：`VIENNAPS_USE_GPU`（CUDA）与 `VIENNAPS_ENABLE_VULKAN` 解耦；Vulkan 可独立 `cmake -S gpu/vulkan` 验证。公共物理 API 继续头文件优先。

---

## 4. 全局架构分层（自顶向下）

### 4.1 原版生产栈（必须保持）

```
User / Python
  └─ Process::apply() / calculateFlux()
       └─ ProcessStrategy（按 flags 择一）
            ├─ GeometricProcessStrategy
            ├─ OxidationStrategy          ← model->applyModel()
            ├─ CallbackOnlyStrategy
            ├─ AnalyticProcessStrategy
            ├─ FluxProcessStrategy        ← 通量主路径
            └─ ALPStrategy
                 └─ FluxEngine
                      ├─ CPU_DISK / CPU_TRIANGLE
                      └─ GPU_DISK / GPU_TRIANGLE / GPU_LINE  (CUDA，可选)
                           └─ SurfaceModel::calculateVelocities
                                └─ VelocityField + TranslationField
                                     └─ AdvectionHandler → viennals::Advect
                                          └─ Domain (多层 LS + MaterialMap)
```

### 4.2 本叉叠加的 Vulkan 栈（可替换执行面）

```
Process / FluxProcessStrategy        ← 生产路由层（P5 正在打通）
  └─ FluxEngine 抽象
       ├─ CPU_* / GPU_*（原版行为）
       └─ VULKAN_RAY（经 setFluxEngineOverride 注入，非工厂直建）
            └─ VulkanRayFluxEngine (PIMPL)
                 └─ DeploymentComputeContext
                      └─ ComputeSession
                           └─ DeviceRayFluxPipeline
                                └─ hit / compact / scan / radix / reduce / BVH

并行可注入（表面 / LS，非改模型类）：
  ProcessDeploymentBinding → Coverage / SurfaceDiffusion / NeutralVelocity
  LevelSetProcessController / LevelSetDeploymentSession

部署期：
  probe → CapabilityProfile → DeploymentProfileDecision
       → buildSelectionPlan(per-stage) → backend selection
```

**关键不变量：**

- 公共头文件不暴露 Vulkan 类型（PIMPL / 仅 `std::string` 等中性配置）。
- 设备数据链的“输入上传 / 终端下载”与“单次 compute submission”必须严格区分。
- fail-closed 输出哨兵与严格 FP32 状态传播行为必须保留。
- Host Canonical Field 仍是 ViennaHRLE/ViennaLS；设备仅持有 Device Working Set。

---

## 5. 原版核心设计意图

以下意图从参考树直接归纳，是迁移时的**不可谈判语义**。

### 5.1 编排与模型分离

- `Process` **不知道**具体化学（SF6、TEOS、氧化…）。
- 模型通过组合插件声明能力：`SurfaceModel`、粒子（`insertNextParticleType`）、`VelocityField`、`GeometricModel`、`AdvectionCallback`。
- `ProcessContext::updateFlags()` 从模型与参数推导：`useFluxEngine`、`isGeometric`、`isAnalytic`、`isALP`、`managesOwnPhysics` 等。
- 策略按注册优先级匹配第一个 `canHandle==true` 的实现。

### 5.2 策略优先级（原版与本叉一致）

注册顺序（`Process::initializeStrategies`）：

1. `GeometricProcessStrategy` — 有几何分布模型
2. `OxidationStrategy` — `managesOwnPhysics()==true`
3. `CallbackOnlyStrategy` — `processDuration==0` 且有 advection callback
4. `AnalyticProcessStrategy` — 有速度场、无粒子通量、非几何
5. `FluxProcessStrategy` — 使用通量引擎且 duration>0，非 ALP/几何/解析
6. `ALPStrategy` — 原子层工艺标志

**漂移禁令：** 不得因引入 Vulkan 而改变上述匹配顺序或 `canHandle` 条件。

### 5.3 形貌权威在 Level Set

- 射线/通量只提供表面通量与覆盖度输入。
- 最终表面由 `viennals::Advect` / `GeometricAdvect` / 氧化求解器决定。
- `Domain` 持有多层 `viennals::Domain` + 对齐的 `MaterialMap`；`getSurface()` 为顶层 LS。

### 5.4 FluxEngine 契约（五步接口）

抽象（`psFluxEngine.hpp`）固定生命周期：

1. `checkInput`
2. `initialize`
3. `updateSurface` — 几何输入面（与计算提交分离）
4. `calculateSourceFluxes` — 源面→表面粒子追踪
5. `calculateSurfaceFluxes` — 可选脱附/表面源

原版 `FluxEngineType`：`AUTO | CPU_DISK | CPU_TRIANGLE | GPU_DISK | GPU_TRIANGLE | GPU_LINE`。

AUTO 意图（原版）：

- 若 GPU 可用且模型有 GPU 实现：周期边界偏 `GPU_DISK`，否则 `GPU_TRIANGLE`；
- 否则 **`CPU_DISK`**。

本叉增加 `VULKAN_RAY`，注释明确：**仅经 override 注入**；在 `P5-DEPLOYMENT-EXIT` / AUTO 生产路由完成前，不得把 AUTO 默认改成 Vulkan。

### 5.5 模型插件契约

`ProcessModelBase` 关键意图：

| 钩子 | 含义 |
|---|---|
| `useFluxEngine()` | CPU 模型上由 `particles.size()>0` 决定 |
| `managesOwnPhysics()` | 走 `OxidationStrategy`，绕过 FluxEngine |
| `applyModel(domain)` | 自管物理入口 |
| `getGPUModel()` | CUDA 对偶模型；与 Vulkan 无关 |
| `initialize` / `finalize` | 工艺前后钩子 |

`SurfaceModel` 关键意图：

| 钩子 | 含义 |
|---|---|
| `calculateVelocities(fluxes, coords, materialIds)` | 通量→表面速度 |
| `updateCoverages` / `initializeCoverages` | 覆盖度动力学 |
| `getDesorptionWeights` / `getDiffusionCoefficients` | 脱附与表面扩散系数 |

**漂移禁令：** Vulkan 路径不得绕过 `SurfaceModel` 私自改写速度语义；不得在引擎内硬编码某一化学模型的速率公式。

### 5.6 失败与可观测性

- 多数路径：`ProcessResult` + 日志；`PROCESS_CHECK` 早退。
- Python：`USER_INTERRUPTED` 抛出以响应信号。
- 零速度可 `EARLY_TERMINATION`。
- 元数据层级：`MetaDataLevel::{NONE,GRID,PROCESS,FULL}`。

本叉在 Manual Vulkan 不可满足时的 fail-closed，是对“显式选择必须可诊断失败”的强化，**不得**变成静默改算法。

---

## 6. 逻辑流程详解

### 6.1 `Process::apply()` 骨架

```
apply()
  ├─ checkInputUpdateContext()
  │    ├─ 校验 domain / level sets / model
  │    ├─ updateFlags() / printFlags()
  │    └─ AUTO → 解析 FluxEngineType（原版：CPU/CUDA；本叉暂不把 AUTO→Vulkan）
  ├─ findStrategy()
  ├─ if requiresFluxEngine → setFluxEngine(createFluxEngine())
  │    └─ 本叉：若 setFluxEngineOverride 已安装，工厂返回 override
  ├─ strategy->execute(context)
  └─ handleProcessResult() + 可选 process metadata
```

### 6.2 通量主路径单步（防漂移核心链）

`FluxProcessStrategy::processTimeStep`（原版与本叉共享）：

```
prepareAdvection                    // LS 按离散格式展开
updateState → ToDiskMesh            // 表面盘网格 + 材料 ID
fluxEngine_->updateSurface          // 几何交给射线后端
calculateSourceFluxes               // 源面粒子 → PointData 通量
[+ calculateSurfaceFluxes]          // 可选脱附
[+ surface diffusion on fluxes]     // 可选
[+ updateCoverages]
SurfaceModel::calculateVelocities
VelocityField::prepare
[+ pre-advection callback]
AdvectionHandler::performAdvection  // viennals::Advect
[+ coverages from new surface]
[+ post-advection callback]
```

**Vulkan 切片当前允许改动的范围：** 仅 `FluxEngine` 内 `updateSurface` / `calculateSourceFluxes` 的设备实现，且须对齐 `CPU_TRIANGLE`（当前 route 契约）的源采样、归一化与点数据落盘语义。策略循环步骤顺序不得重排（见 §6）。

### 6.3 其它策略流（简表）

| 策略 | 流 | Vulkan 相关 |
|---|---|---|
| Analytic | `VelocityField::prepare(..., nullptr)` → Advect | 无 ray；未来可加速速度场/LS，不得改解析语义 |
| Geometric | `GeometricAdvect` + 材料布尔一致化 | 独立阶段 `GEOMETRY_EXTRACTION` |
| Oxidation | `model->applyModel`（Deal–Grove + LS 扩散/变形） | P6：`OXIDATION_LINEAR_SOLVE` |
| ALP | 脉冲内多次通量/覆盖 → 可选 purge → 速度 → 整层 Advect | 依赖完整 ray 物理后才可宣称 |
| CallbackOnly | 仅 `applyPreAdvect(0)` | 无设备路径需求 |

### 6.4 `calculateFlux()` 

只跑一轮通量，返回 `diskMesh`；三角引擎可另填 `triangleMesh`。用于诊断与差分，不推进工艺时间。Vulkan route smoke 应能在此路径与 CPU oracle 对齐。

---

## 7. 模型目录与路由意图

模型均位于 `include/viennaps/models/`。原版与本叉文件集基本一致；本叉额外有 `psNeutralTransportVelocityExecutor.hpp`（执行器缝，非新化学）。

### 7.1 通量 / 粒子 → Flux 或 ALP

| 模型 | 策略 | 原版 GPU | Vulkan 宣称门槛 |
|---|---|---|---|
| `SingleParticleProcess` | Flux | 有 | P5-RAY-ROUTE 首个真实模型 |
| `MultiParticleProcess` | Flux | 有 | P5-MODEL-MATRIX |
| `SingleParticleALD` | ALP | 有 | 需 P5-RAY-PHYSICS + ALP 重验证 |
| `TEOSDeposition` / `TEOSPECVD` | Flux | 部分 | MODEL-MATRIX |
| `IonBeamEtching` / `FaradayCageEtching` | Flux | 有 | MODEL-MATRIX（自定义 Source） |
| `FluorocarbonEtching` | Flux | — | 多物种守恒 |
| `PlasmaEtching*` + SF6/HBr/CF4 化学特化 | Flux | 多数有 | MODEL-MATRIX |
| `NeutralTransport` | Flux | 有 | 与 `NEUTRAL_TRANSPORT_VELOCITY` 阶段协同 |

### 7.2 解析速度 → Analytic

`IsotropicProcess`、`DirectionalProcess`、`WetEtching`、`SelectiveEpitaxy`、`CSVFileProcess`、`OxideRegrowth`

意图：无粒子；速度直接驱动 Advect。Vulkan 不得误路由到 FluxEngine。

### 7.3 几何平流 → Geometric

`SphereDistribution`、`BoxDistribution`、`CustomSphereDistribution`、`GeometricTrenchDeposition`

### 7.4 自管物理 → Oxidation

`Oxidation`：`managesOwnPhysics()==true`；**不走 FluxEngine**。设备加速属于 P6，正确性以氧化收敛与几何 oracle 为准，不是 ray 差分。

### 7.5 典型插件装配（意图模板）

以 `SingleParticleProcess` 为代表：

```
insertNextParticleType(...)     → useFluxEngine()==true
setSurfaceModel(...)            → flux × materialRate → velocities
setVelocityField(Default...)
hasGPU=true; getGPUModel()      → CUDA 对偶（与 Vulkan 并行存在）
```

---

## 8. Domain / 几何 / 材料集成点

| 阶段 | 机制 | 漂移注意 |
|---|---|---|
| 域表示 | 多层 LS + MaterialMap + 可选 cell set | 不得改为全局稠密 SDF 作为权威 |
| 表面离散 | `ToDiskMesh`；三角引擎另 `CreateSurfaceMesh` | 引擎切换不得改变材料 ID 绑定 |
| 速度映射 | `TranslationField`（translator 或 k-d tree） | 高阶中间速度回调依赖同一映射 |
| 平流 | `AdvectionHandler` → `viennals::Advect` | 时间推进与 CFL 语义属 LS，不属 ray |
| 覆盖度 | 表面 ↔ 顶层 LS PointData | 设备覆盖度执行器必须保留哨兵/拒绝语义 |
| 几何构建 | `psMake*`、`GeometryFactory`、GDS | 工艺前几何；与后端无关 |

---

## 9. 本叉 Compute 控制面意图

术语遵循 `CONTEXT.md`：Compute Backend、Capability Profile、Execution Policy、Manual Override、Host Canonical Field、Device Working Set、Backend Parity、Functional Threshold、Selection Record。

### 9.1 阶段枚举（`Stage`）

与工艺循环的对应关系：

| Stage | 原版对应工作 | 选择独立性 |
|---|---|---|
| `GEOMETRY_EXTRACTION` | 盘/三角网格提取 | 可独立 |
| `LEVEL_SET` | 扩展、重建、平流相关核 | 可独立 |
| `RAY_TRACING` | FluxEngine 粒子输运 | 可独立 |
| `COVERAGE` | 覆盖度更新 | **独立于**表面扩散 |
| `SURFACE_DIFFUSION` | 通量/覆盖表面扩散 | 可独立 |
| `NEUTRAL_TRANSPORT_VELOCITY` | 中性输运速度绑定 | 可独立 |
| `OXIDATION_LINEAR_SOLVE` | 氧化耦合线性解 | P6 |
| `CUSTOM` | 扩展槽 | — |

**意图：** per-stage eligibility 是防“整仿真被一个不合格阶段拖死或拖错”的核心；一个阶段失败不得污染其它阶段输出。

### 9.2 选择语义

- `SelectionMode::AUTO`：需通过硬功能阈值；Vulkan 还需严格 FP32 numerical smoke（策略测试覆盖）。
- `SelectionMode::MANUAL`：覆盖 AUTO 排名，但**不可**静默降级算法/精度；不合格则失败并诊断。
- `allowFallback` / `allowStageFallback`：仅在策略允许时回退 CPU；Manual 强制 Vulkan 且无 profile → fail-closed。

### 9.3 部署生命周期

```
probeDevices / viennaps-device-probe
  → CapabilityProfile (+ fingerprint)
  → DeploymentProfileDecision
  → buildSelectionPlan(workloads)
  → DeploymentComputeContext::prepare
  → ComputeSession（generation 守卫）
  → 各阶段 executor / FluxEngine 绑定
```

无有效匹配 profile 时选择 Vulkan 必须失败；Manual CPU 可 bypass 探测。

---

## 10. Vulkan 迁移映射：意图对齐表

| 原版意图单元 | 本叉实现落点 | 当前状态 | 不漂移验收 |
|---|---|---|---|
| `Process` 公共 API | `psProcess.hpp` + override/executor 缝 | 兼容扩展中 | 旧示例/测试无参数行为回归 |
| 策略路由 | 同名 strategy 头文件 | 保持 | canHandle 顺序不变 |
| `CPU_TRIANGLE` 通量 | `psCPUTriangleEngine.hpp` | **oracle** | route smoke 差分 |
| `CPU_DISK` 通量 | `psCPUDiskEngine.hpp` | 默认 AUTO 终点之一 | 未宣称 Vulkan 对齐前不改语义 |
| CUDA GPU_* | `psGPU*Engine.hpp` + `gpu/models` | 保留 | P7 发布门无回归 |
| 设备光线链 | `DeviceRayFluxPipeline` | P5-JD/JE/JF 本地接受 | 单 submission；严格 FP32；CPU `runCpu` |
| Process←Vulkan 通量 | `VulkanRayFluxEngine` + override | **DONE-LOCAL（P5-RAY-ROUTE）** | 单弹射；与 CPU_TRIANGLE 对齐；无 profile fail-closed |
| 反射/多弹射 | 仍属 ViennaRay / 未来物理卡 | 未做 | 不得用单弹射结果冒充完整输运 |
| 覆盖度/扩散/中性速度 | `gpu/vulkan/surface/` + binding | 本地有；待与 ray 同 session | 哨兵未改写；CPU 回退 |
| Level Set 设备核 | `gpu/vulkan/levelset/` | 本地组合接受 | FALLBACK/FAIL 策略明确 |
| 氧化求解 | 仍 `psOxidation.hpp` | P6 | 收敛 history；无静默几何 |
| Python | `python/pyWrap*` | 绑定同一 API | 不另开 Vulkan 专用语义 |

---

## 11. 功能不漂移契约（检查清单）

在合并任何加速改动前，逐项确认：

1. **API 面：** `Process` / Domain / models 的用户调用方式与原版一致；新增仅为可选注入。
2. **策略面：** 六策略优先级与 flags 语义未改。
3. **通量接口：** 五步 FluxEngine 生命周期未改；`updateSurface` 与计算提交分离。
4. **物理面：** `SurfaceModel` / 粒子局部数据标签 / 材料速率映射仍是速度来源。
5. **形貌面：** LS 仍为 Host Canonical；提交回主机的字段 generation/dirty 可解释。
6. **oracle 面：** 每个设备宣称都有同 fixture 的 CPU 路径结果可复核。
7. **失败面：** Manual 不合格 → 显式失败；AUTO 不合格 → 阶段回退 CPU；禁止静默换公式。
8. **度量面：** 上传/下载不计为 compute submission。
9. **构建面：** SDK/本机路径只经环境变量；CPU/no-SDK 构建不冒充硬件证据。
10. **宣称面：** 支持矩阵行 = 测试 + 显式不支持/回退行；编译通过 ≠ 支持。

### 11.1 推荐差分方法

| 场景 | Oracle | 比较量 |
|---|---|---|
| 单弹射 route | `CPU_TRIANGLE` | 点通量 / normalize 后场；文档 tolerance |
| 设备链单元 | `DeviceRayFluxPipeline::runCpu` | 终端缓冲；严格 FP32 状态 |
| 表面执行器 | 本地 `oracle()` | 输出哨兵在拒绝路径不变 |
| 完整工艺（未来） | 同模型 CPU `apply()` | 覆盖率、守恒、最终几何 |

参考树定位：差分时优先对照 `code_reference/ViennaPS` 的引擎与策略头，确认本叉未改契约后再比数值。

---

## 12. 当前执行位置（2026-08-05）

- **已接受的本地里程碑**：PD0–PD4 控制面、`PD5-CI-DOCS-INTEGRATION`、`PD5-INSTALL-EXPORT`（仅 CPU/no-SDK 边界）、`P5-JD/JE/JF`（设备驻留组合、单 compute submission、严格 FP32 fail-closed）。
- **证据基线**：5/5 聚焦 ray CTest + standalone 12/12 ray suite，均在本机 Release Vulkan / Intel Arc 上重验证。CPU/no-SDK 构建只验证 fallback/构建卫生，**不构成硬件证据**。
- **远端 CI**：`PD5-CI-REMOTE` 仍 `BLOCKED`——远端默认分支为 `master`，无 `build.yml`，无可引用 run URL。**禁止用本地 YAML 审查冒充远端 CI。**
- **当前在制卡片**：`P5-RAY-PHYSICS`——在 `P5-RAY-ROUTE` 已验证的单次弹射
  Process 路由上，定义反射/roulette/事件队列/材料响应的 CPU 侧契约，为后续
  多弹射设备实现做准备。`P5-RAY-ROUTE` 已 DONE-LOCAL，详见执行快照日志。- **工作树注意**：可能存在未提交的 P5 回归加固 diff（`gpu/vulkan/ray/CMakeLists.txt`、`device_ray_flux_pipeline_smoke.cpp`）；保持与 Process/模型集成隔离。

### 12.1 P0–P4 / PD0–PD4 第一轮合规审计（2026-08-05）

**正式账本：** [p0-p4-cpu-reuse-audit-round1.md](p0-p4-cpu-reuse-audit-round1.md)
（Round 1 / `RECORDED`；纠偏未在本轮关闭）。

对照要求（§2.2 第 8 条）与 `code_reference/ViennaPS` 只读差分结论摘要：

**总评：总体合规（架构未另起 Process 循环）；存在两处 PARTIAL 与一处共享 CPU 路径上的编排偏离。**

| 区域 | 判定 | 说明 |
|---|---|---|
| `include/viennaps/compute/` 控制面 | **合规** | 仅选择/档案/探测；不改工艺语义 |
| `gpu/vulkan/runtime/` | **合规** | Session/DeploymentContext；不接管 `Process::apply` |
| `gpu/vulkan/primitives/` | **合规** | 纯计算原语 + kernel-contract CPU oracle（非生产引擎分叉） |
| 表面 coverage/diffusion/neutral 缝 | **合规** | 编排仍在 `CoverageManager` / `FluxProcessStrategy` / SurfaceModel；空 executor = 原 CPU；`psSurfaceDiffusion.hpp` 与原版一致 |
| `psCPU*Engine` | **合规** | 与原版一致，仍为生产通量默认 |
| Level Set 更新注入 | **基本合规** | 经 `viennals::Advect` executor 缝；空 executor = 原 Advect |
| HRLE rebuild 分类/压缩/重建端口 | **已纠偏** | `include/viennaps/levelset/psHrleRebuild*.hpp` 为冻结语义镜像，非直接调用 Advect 私有 rebuild 体；`hrleRebuildCpuFixture` 经实际 rebuild callback 执行本地镜像，断言 callback dispatch，并以 canonical HRLE / PointData 位模式与上游 CPU 差分，仍需随上游 ViennaLS 漂移复核 |
| P4 ray 设备链 | **部分（可接受）** | 独立 `runCpu`/`intersectCpu` 仅作 kernel 差分；生产通量仍走 `psCPU*Engine`（Process 路由属 P5） |
| **P3K 平流进度守卫** | **已纠偏** | `psAdvectionHandler::performAdvection` 按 executor 活性分路：空 executor 恢复原版 4.6.2 语义及 ViennaLS update/rebuild 序列；有 executor 时保留 fail-closed，时间错误同 update/rebuild 错误回滚完整多步 snapshot；回归测试拆分 legacy / executor-active 路径 |

**最小纠偏建议（账本 R1-F1…F3；R1-F1/R1-F2 已关闭，R1-F3 保留建议）：**

1. ~~将 `psHrleRebuild*` 标为 ViennaLS rebuild 的冻结镜像；增加 Advect-CPU vs oracle 固定夹具差分 CI。~~（已完成：冻结镜像标签 + `hrleRebuildCpuFixture`；fixture 实际经过 callback 执行本地镜像并断言 dispatch，CI 接入仍属后续工程化动作）
2. ~~明确 P3K：若确认为产品级 fail-closed 改进，在开发报告/状态看板单列“有意偏离原版”并保留回归；否则改为仅设备路径/可选开关，恢复共享 CPU 路径与原版一致。~~（已完成：executor 活性分路）
3. P5+ 光线 Process 路由优先调用 ViennaRay / `CPUTriangleEngine` 主机辅助函数，避免长期维护归一化副本。

详细条目、P3K 对照表与不做宣称见第一轮审计账本。

---

## 13. 剩余工作的全局分解（P5→P7）

依赖顺序（单人执行）：

```
PD5-CI-REMOTE(BLOCKED) → P5 回归加固宣称释放 → P5-RAY-ROUTE(DONE-LOCAL) → P5-RAY-PHYSICS
  → P5-SURFACE-INTEGRATION → P5-MODEL-MATRIX → P5-DEPLOYMENT-EXIT
  → P6-LA-BASELINE → P6-OXIDATION-COUPLING → P6-PHYSICS-EXIT
  → P7-RESIDENT-EXECUTION → P7-CALIBRATION → P7-CI-SOAK-RELEASE
```

`P6-LA-BASELINE` 在多人 setup 下可与 P5 并行；单人严格按序。一卡一 worktree、独占文件边界。

| 阶段 | 全局意图 | 退出本质 |
|---|---|---|
| **P5** 表面物理与模型覆盖 | 让真实模型经 Vulkan 路由达到 CPU 对齐 | 每个支持行有测试 + 显式不支持/回退行；覆盖率/守恒/几何同时通过 |
| **P6** 氧化与耦合求解 | FP64 线性求解与氧化耦合上设备 | 合格 FP64 设备进 Auto；不合格仅回退相关阶段；收敛失败保留 residual history |
| **P7** 常驻、调优与发布 | 跨步常驻 + 成本模型 + 发布门 | Auto 在硬件矩阵上只选合格且接近最快计划；Selection Record 可重放；CPU-only/CUDA 无回归 |

---

## 14. 卡片级边界与验收门（P5 前段）

> 完整看板以状态文档 `Total-plan continuation board: P5 to P7` 为准；此处仅列当前焦点的全局约束。

- **P5-RAY-ROUTE**（在制）
  - 意图：至少一个真实 ray 模型经 Vulkan 路由，带 CPU 位级/差分覆盖与显式 fail-closed Vulkan 选择。
  - 独占文件边界：`include/viennaps/psUtil.hpp`（FluxEngineType）、`include/viennaps/process/psProcess.hpp`（engine override）、`gpu/vulkan/ray/vulkan_ray_flux_engine.{hpp,cpp}`、`gpu/vulkan/ray/ray_flux_process_route_smoke.cpp`、`gpu/vulkan/ray/CMakeLists.txt`。
  - 验收：Intel Arc Release Vulkan 上，Vulkan 通量与 CPU_TRIANGLE oracle 在文档化 tolerance 内一致；无 profile 的 manual Vulkan **显式失败**且 CPU 路径不受影响；No-SDK 配置/构建干净跳过；**禁止机器本地 SDK/路径字符串进入受版本控制文件**。
  - 解锁：model matrix 与 surface integration。

- **P5-RAY-PHYSICS**：边界/反射、roulette/事件队列、材质/表面响应、多弹射契约；产出代表性模型的确定性输运差分。

- **P5-SURFACE-INTEGRATION**：coverage/表面扩散/中性速度/Process 回调安装在**同一共享 deployment session** 上重验证。

- **P5-MODEL-MATRIX**：多粒子/species、ion/neutral、fluorocarbon/plasma/TEOS、wet-etch/selective-epitaxy/oxide-regrowth；逐行测试 + 守恒/几何 + 显式不支持行。

- **P5-DEPLOYMENT-EXIT**：install/export、deployment profile 持久化、Process preview 文档与支持矩阵。

---

## 15. 必须始终遵守的工程不变量（复核清单）

1. **CPU oracle 优先**：任何 device 声明都必须可在非 CUDA 主机上用 CPU 结果复核；不接受“device 自身一致”或“与 mprocess 一致”作为正确性证据。
2. **原版意图优先**：公共编排与模型契约以 `code_reference/ViennaPS` 为语义基线；本叉只扩展执行面。
3. **mprocess 仅嵌入参考**：可借鉴常驻/融合/分层等价/worker 治理；禁止照搬 CuPy 抽象、禁止机械翻译 CUDA 源、禁止把其 provisional 物理结果当真值。
4. **不模拟远端**：无已发布 commit + run ID + URL + conclusion + runner 上下文时，远端卡片一律 `BLOCKED`。
5. **提交/下载 ≠ compute submission**：保持二者在声明与度量上分离。
6. **fail-closed 与严格 FP32 状态**不得削弱；输出哨兵保持。
7. **时间戳**用 `datetime.now(timezone.utc)`；**哈希**仅 SHA-256。
8. **构建复用**：默认复用已验证构建；确需刷新时先移除过时旧构建；验收构建保持 worktree-local。
9. **机器本地路径隔离**：SDK/绝对路径（含 `VIENNAPS_MPROCESS_SOURCE_DIR`）只经环境变量注入，不写入受版本控制文件。
10. **清理边界**：验证后只删除当前任务创建的临时目录（先确认无活动进程），保留 `.claude/` 与用户自有改动。
11. **一卡一界**：单卡独占文件边界；不把回归加固与 Process/模型集成混提交。
12. **非计算复用 CPU**：除 compute 核外，编排/模型/主机输运辅助/默认引擎须复用或兼容原版 CPU 路线；有意偏离必须卡片化文档 + 回归（见 §2.2 第 8 条、§12.1）。

---

## 16. 执行快照日志（每卡片完成追加一行）

| 日期(UTC) | 卡片 | 结果 | 证据锚点 |
|---|---|---|---|
| 2026-08-05 | P5-JD/JE/JF-REGRESSION-HARDENING | DONE-LOCAL | 状态看板；standalone 12/12，聚焦 5/5 |
| 2026-08-05 | P5-RAY-ROUTE | DONE-LOCAL | `ray_flux_process_route_smoke` PASS：totalRelDiff=0.29%, maxRelDiff=2.07%, 20000/20000 命中；fail-closed 未准备上下文无 flux；状态看板已更新 |
| 2026-08-05 | INTENT-WHITEPAPER | DONE-LOCAL | 本文件扩写为全程序功能意图白皮书；基线=`code_reference/ViennaPS` |
| 2026-08-05 | INTENT-DESIGN-PHILOSOPHY | DONE-LOCAL | 补入双基线+mprocess嵌入理念与总体意图规划（§2）；参考=`D:\\mprocess` + 开发报告 §2.2 |
| 2026-08-05 | CPU-REUSE-REQUIREMENT + P0-P4-AUDIT-R1 | DONE-LOCAL | 非计算复用CPU硬要求入账；第一轮审计账本 `docs/design/p0-p4-cpu-reuse-audit-round1.md`（RECORDED；R1-F1…F3 未关闭） |

---

## 17. 附录 A：原版 vs 本叉目录对照（速查）

| 域 | 原版 | 本叉增量 |
|---|---|---|
| Process | `include/viennaps/process/*` | override、LS/coverage/diffusion executor 缝 |
| Models | `include/viennaps/models/*` | `NeutralTransportVelocityExecutor` |
| Compute policy | （无） | `include/viennaps/compute/*` |
| CUDA GPU | `gpu/` + `psGPU*Engine` | 保留 |
| Vulkan | （无） | `gpu/vulkan/{runtime,primitives,ray,surface,levelset,shaders}` |
| 文档 | README / 示例 | `docs/design/*`、`docs/adr/*`、本白皮书 |
| mprocess 嵌入参考 | （库外）`D:\\mprocess` | 仅模式参考；见 §2；环境变量 `VIENNAPS_MPROCESS_SOURCE_DIR` |

## 18. 附录 B：关键源文件索引（意图锚点）

**原版参考树：**

- `include/viennaps/process/psProcess.hpp` — apply / 工厂 / AUTO
- `include/viennaps/process/psFluxProcessStrategy.hpp` — 通量主循环
- `include/viennaps/process/psProcessModel.hpp` — 模型插件基类
- `include/viennaps/process/psSurfaceModel.hpp` — 通量→速度
- `include/viennaps/process/psCPUTriangleEngine.hpp` — 三角通量 oracle
- `include/viennaps/models/psOxidation.hpp` — 自管物理范例
- `include/viennaps/psDomain.hpp` — 多层 LS 域

**本叉工作树：**

- `include/viennaps/psUtil.hpp` — `FluxEngineType`（含 `VULKAN_RAY`）
- `include/viennaps/compute/backendPolicy.hpp` — Stage / 选择计划
- `gpu/vulkan/ray/vulkan_ray_flux_engine.{hpp,cpp}` — Process 注入点
- `gpu/vulkan/ray/device_ray_flux_pipeline.*` — 设备驻留光线链
- `gpu/vulkan/ray/ray_flux_process_route_smoke.cpp` — route 验收
- `gpu/vulkan/surface/process_deployment_binding.hpp` — 表面三阶段绑定
- `gpu/vulkan/runtime/deployment_compute_context.*` — 部署会话

## 19. 附录 C：用户故事级时序（SingleParticle 通量）

```
用户: Process(domain, SingleParticleProcess, duration).apply()
  → flags.useFluxEngine=true
  → FluxProcessStrategy
  → createFluxEngine()
       原版 AUTO → 通常 CPU_DISK（无 CUDA 时）
       本叉 route 试验 → setFluxEngineOverride(VulkanRayFluxEngine)
  → 每步: updateSurface → calculateSourceFluxes
       → SurfaceModel::calculateVelocities
       → Advect → Domain 更新
```

意图测试问题（开发中自问）：

- 若强制 CPU_TRIANGLE，几何是否与 Vulkan override 在 tolerance 内一致？
- 若去掉 profile 且 Manual Vulkan，是否失败且未改写 domain？
- 若 `maxReflections>0`，是否拒绝而非近似？

---

## 20. 附录 D：mprocess 嵌入参考索引

路径根：`D:\mprocess`（或环境变量 `VIENNAPS_MPROCESS_SOURCE_DIR`）。

| 主题 | 建议先读 |
|---|---|
| 系统边界与依赖方向 | `docs/architecture/overview.md` |
| 分层等价 / claim strength | `docs/baselines/provisional-verification.md` |
| 场与设备数组语义 | `backend/mprocess/fields.py` |
| 能力宣称清单（产品侧） | `backend/mprocess/capabilities.py` |
| 常驻 / 栈 | `backend/mprocess/stack.py`（若存在）及 fields 协作 |
| LS / hybrid 砖块 | `backend/mprocess/levelset.py`、`backend/mprocess/hybrid/` |
| 表面扩散 | `backend/mprocess/surface_diffusion.py` |
| 粒子输运 / BVH | `backend/mprocess/physical_etch/transport.py`、`disk_bvh.py` |
| 运行时治理 | `backend/mprocess/worker.py`、`profiling.py` |

**使用规则：** 只抽取契约、数据流、诊断与常驻边界；物理结果为 provisional；后端抽象绑定 CuPy，不可作为 ViennaPS Vulkan ABI。

*维护说明：本白皮书随每张卡片完成而演进。修改 Vulkan 加速代码或其记录前，请先读本文件 + 状态看板 + 开发报告，并在需要确认原版语义时打开 `D:\Codex_lib\code_reference\ViennaPS` 对应头文件；需要确认 GPU 常驻/融合/分层等价嵌入模式时查阅 `D:\mprocess` 与开发报告 §2.2，但不得把 mprocess 物理结果当作真值。局部实现不得偏离本白皮书记载的功能意图与总体设计理念。*
