# PD4-HW-MATRIX 验收卡片

- **状态**：`RUN`；对应主线 `docs/design/vulkan-compute-acceleration-status.md:2848`。
- **边界**：本卡只验收 probe/profile/bootstrap 的可观测行为和硬件证据；不修改状态文档，不宣称生产路由或性能收益。
- **路径规则**：所有构建目录、profile、临时输出、SDK 和依赖位置均使用调用者提供的路径或环境变量；卡片中不得填写机器本地 VTK/Vulkan 路径。

## 验收入口

在全新构建目录执行；`<build>`、`<probe>`、`<profile>` 和 `<reason>` 是调用者替换的占位符：

```text
cmake -S . -B <build> -DVIENNAPS_BUILD_VULKAN_PROBE=ON
cmake --build <build> --config Debug --target viennaps-device-probe
ctest --test-dir <build> -C Debug --output-on-failure \
  -R "^(capabilityProfileIO|probeProfileAdapter|vulkanDeploymentProbe|vulkanDeploymentBootstrap)$"
```

四个 focused CTest 必须全部通过。它们是 CPU/fixture 测试，不得作为真实 GPU 证据。

## 必验矩阵

| 行 | 操作与证据 | 通过条件 | 失败/降级记录 |
|---|---|---|---|
| No-SDK disabled | 在独立构建中取消设置 `VULKAN_SDK`，保留 `VIENNAPS_BUILD_VULKAN_PROBE=ON`，构建并运行 `<probe>`。 | 退出码 `0`；JSON 含 `"status":"disabled"`、空 `devices`，且说明是 build-time fallback。 | 不能把 disabled 当作 Vulkan PASS；记录 SDK 未提供，CPU 路径仍可运行。 |
| Missing profile | 运行 `capabilityProfileIO` 中 `TestDeploymentProfileMissingRequiresProbeAndUsesCpuFallback`。 | `requiresProbe=true`、状态 `MISSING`、选中后端 `CPU`。 | 任何 Vulkan 选择均为失败。 |
| Stale profile | 运行 `TestDeploymentProfileMismatchRequiresProbeAndCpuFallback`，以及 `probeProfileAdapter` 的 stale fingerprint 测试。 | `requiresProbe=true`、状态 `STALE`、选中后端 `CPU`；driver UUID 变化不能复用缓存。 | 记录实际变化的指纹字段；不得保留旧 profile 的 Vulkan 能力。 |
| Malformed profile | 运行 `TestDeploymentCorruptedOrUnknownProfileFallsClosedToCpu` 的损坏 JSON 分支及 parser malformed/duplicate/type 测试。 | 状态 `INVALID` 或明确解析失败；选中后端 `CPU`；候选 profile 不被信任。 | 记录解析错误类别；不得修补输入后继续自动选择。 |
| Unknown profile/schema | 运行 `TestDeploymentCorruptedOrUnknownProfileFallsClosedToCpu` 的 schema 分支，以及 `TestUnknownSchemaVersionIsRejected`。 | 状态 `INVALID`；未知 schema 被拒绝；选中后端 `CPU`。 | 记录 schema 版本；不得把未知版本当作兼容版本。 |
| Strict-FP32 success | 在可运行 Vulkan 主机上，用环境/工具链提供 SDK 和 shader compiler，运行 `<probe> --strict-fp32-smoke --write-deployment-profile <profile> --validate-profile`。 | 退出码 `0`；strict evidence 为 `PASS`，精确 contract id，`caseCount>0`，`mismatchCount=0`，`maxUlp=0`，`elapsedMs<=watchdogMs`，空 diagnostic；profile validation PASS；strict child UUID 恰好匹配一个 profile。 | 只接受同一设备 UUID 的证据；记录完整原始 JSON 和 profile，不用广告 feature 替代数值证据。 |
| Strict-FP32 failure | 在同一 probe 上运行 `--strict-fp32-smoke --strict-fp32-force-failure`；同时保留 `vulkanDeploymentProbe` 的 nonzero/timeout/malformed/mismatched-hardware/invalid-evidence fixture 覆盖。 | 退出码非零；evidence 为 `FAIL` 或明确诊断；临时 child 输出被清理；错误 profile 不写入或不被采用。 | AUTO 必须 fail-closed 到 CPU；显式 Manual Vulkan 应报告错误，不得静默伪装成严格成功。 |
| Hardware fingerprint | 从真实 probe 原始 JSON 和写出的 profile 一并摘录：device UUID、driver UUID、vendor ID、device ID、device name、driver version/date；另记录 selected compute `queueFamily`、`dedicatedQueue` 和 `computeQueueFamilyIndices`。 | profile 的七个 identity 字段完整且与当前设备一致；存在 compute queue，优先确认 dedicated compute queue；profile round-trip 保留 UUID/vendor/device 字段。 | 缺字段、UUID 歧义、队列缺失或当前硬件不匹配均不得授权 Vulkan。 |
| CPU fallback/bypass | 运行 bootstrap 的 `failureIsCpuFallback`、`missingTransientDirectoryFailsClosed`、`manualCpuBypassesEverything`，以及 profile missing/stale/invalid 行。 | collector/probe/persistence/路径失败时计划选 `CPU`；全 CPU Manual 请求不调用 collector/probe，且无 profile 或完整 fingerprint 也能成功。 | 记录是 fail-closed fallback 还是 Manual CPU bypass；Manual Vulkan 不得被悄悄改成 CPU。 |

## Fixture 与真实硬件证据

- `capabilityProfileIO`、`probeProfileAdapter`、`vulkanDeploymentProbe`、`vulkanDeploymentBootstrap` 使用 fixture fingerprint、fake launcher 和临时文件；它们只证明状态机、校验、清理、调用次数和 CPU fallback。
- 真实硬件证据必须来自 `<probe>` 的实际 Vulkan 枚举与 strict child：保留命令、退出码、原始 probe JSON、strict evidence JSON、写出的 profile，以及上述 fingerprint/queue 字段。不得用 fixture UUID、fake launcher 或旧状态文字代替。
- 只有真实硬件行同时满足 strict-FP32 PASS、identity 唯一匹配、profile validation PASS 和 queue 证据完整时，才能记录 `PASS`。Focused fixture 全绿不能单独提升该行。

## 不可运行硬件的记法

若主机没有可运行 Vulkan 设备、没有 compute queue、缺少 strict shader artifact/compiler、driver watchdog 超时，或硬件/权限使实际 strict child 无法执行：

```text
PD4-HW-MATRIX / <hardware-row>: NOT RUN
reason: <简短、可复现的阻断原因>
command: <实际命令，路径仍使用占位符或环境变量>
evidence: <已生成的日志/JSON；没有则写 none>
CPU oracle: PASS|FAIL
```

`NOT RUN` 只表示该真实硬件行没有完成执行，不是 PASS，也不是把未执行的 strict smoke 记为 FAIL。若 strict child 确实启动并返回失败，则记 `FAIL`，保留诊断，并确认不产生 Vulkan promotion。No-SDK 行仍单独记为 `PASS`（disabled contract），不能用来充当真实硬件行。

## 验收结论规则

- 四个 focused CTest 全 PASS 是 CPU 控制面前置条件。
- No-SDK disabled、四类 profile fail-closed、strict-FP32 success/failure、fingerprint/queue、CPU fallback/bypass 均须有逐行证据。
- 任一真实硬件行 `FAIL` 或 `NOT RUN` 都不能汇总为 PD4 硬件 PASS；应明确列出阻断原因和 CPU 结果。
- 记录完成后只更新本卡或由卡片 owner 按主线流程更新状态；本卡本身不改 `vulkan-compute-acceleration-status.md`。

## 2026-08-03 当前验收记录

所有命令均在 Git 已登记的 `claude/pd4-f84f7c` worktree 内执行；构建目录位于该 worktree 下。路径在此只以 `<build>`、`<probe>`、`<profile>` 表示，避免记录本机 SDK、VTK 或缓存位置。

| 行 | 当前结果 | 证据 |
|---|---|---|
| Worktree isolation | `PASS` | `git worktree list` 列出当前 `claude/pd4-f84f7c` worktree；旧 `pd4-hw-matrix` 条目为 `prunable`，未被用作构建或恢复来源。 |
| No-SDK disabled | `PASS` | 经 `cmake/invoke-cmake-clean-env.ps1` 从 `gpu/vulkan` 配置独立 `<build>`，取消 `VULKAN_SDK` 并启用 probe；构建 `viennaps-device-probe` 成功。运行 `<probe>` 退出 `0`，输出 `status="disabled"`、`devices=[]`、`source="build-time-fallback"`，原因为构建时 Vulkan headers/libraries 不可用。 |
| Missing / stale / malformed / unknown profile | `PASS` | 在当前 worktree 的本地 VTK override Debug 构建中运行 `capabilityProfileIO` 与 `probeProfileAdapter`，均通过（分别 `0.18 s`、`0.09 s`）。覆盖 missing→`MISSING`、driver UUID stale→`STALE`、malformed/duplicate/type→`INVALID`、unknown schema rejection，且均 fail-closed 选择 `CPU`。 |
| CPU fallback / Manual CPU bypass | `PASS` | 同一 focused CTest 运行 `vulkanDeploymentBootstrap`（`0.09 s`）与 `vulkanDeploymentProbe`（`0.12 s`），均通过。前者覆盖 collector/transient-path failure 的 CPU fallback 和 Manual CPU collector/probe bypass；后者覆盖 nonzero、timeout、malformed、hardware mismatch、invalid evidence 的 fail-closed 与临时输出清理。 |
| Strict-FP32 success | `PASS` | SDK-enabled standalone `<probe> --strict-fp32-smoke --write-deployment-profile <profile> --validate-profile` 退出 `0`。严格 evidence：`PASS`，contract `fp32-bitwise-watchdog-v1`，`caseCount=18`，`mismatchCount=0`，`maxUlp=0`，`elapsedMs=130 <= watchdogMs=60000`，diagnostic 为空；profile validation `pass`。child UUID `8680557d080000000002000000000000` 与唯一 profile/device UUID 匹配。 |
| Strict-FP32 failure | `PASS` | SDK-enabled standalone `<probe> --strict-fp32-smoke --strict-fp32-force-failure` 退出 `1`。输出包含 strict evidence `FAIL` 与 `failureDiagnostic="forced strict-FP32 child failure"`，父级诊断为 `strict FP32 child exited nonzero`；没有向该命令传入 profile 输出路径。`vulkanDeploymentProbe` focused CTest 已通过，覆盖 failure cleanup/adoption fail-closed 行。 |
| Hardware fingerprint / queue | `PASS` | 真实 Intel Arc 枚举：device UUID `8680557d080000000002000000000000`；driver UUID `33322e302e3130312e38383630000000`；vendor/device IDs `32902`/`32085`；name `Intel(R) Arc(TM) Graphics`；driver version `1663644`（driver info `101.8860`）；API `1.4.348`。selected `queueFamily=1`、`dedicatedQueue=true`、`computeQueueFamilyIndices=[0,1]`。profile validation 随 strict-success 命令通过。 |

### 全项目 focused CTest 阻断详情

下列命令都使用 Windows clean-environment wrapper，并只创建当前 worktree 内的临时构建目录：

```text
cmake -S . -B <build> -DVIENNAPS_BUILD_TESTS=ON -DVIENNAPS_ENABLE_VULKAN=OFF
ctest --test-dir <build> -C Debug --output-on-failure -R "^(capabilityProfileIO|probeProfileAdapter|vulkanDeploymentProbe|vulkanDeploymentBootstrap)$"
```

- 使用本地 VTK source 的首次生成走到了 PD4 no-SDK diagnostic stub 配置，但生成阶段被外部 ViennaLS export-set 依赖的 VTK targets 阻断。
- 不设置本地 VTK source 的重试在 VTK 子模块 checkout 时遇到 Windows path-length 失败。
- 随后的本地 VTK retry 在获取 CPM 时网络超时。

这些均是此前遇到的外部依赖/环境问题，不是已定位的 PD4 源码失败；其后复用当前 worktree 中已配置的本地 VTK override 完成四项 focused CTest 的构建和执行，四项均通过。PD4-HW-MATRIX 的本卡验收证据现已完整；后续仅由卡片 owner 按主线流程决定状态文档更新。
