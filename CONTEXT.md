# ViennaPS Compute Acceleration

This context names the concepts used to select and execute process-simulation
work across CPU, CUDA, and Vulkan without changing the physical model.

## Language

**Compute Backend**:
An implementation family that owns device resources and executes simulation
kernels, currently CPU, CUDA, or Vulkan.
_Avoid_: GPU mode, accelerator type

**Capability Profile**:
A persisted, versioned record of the hardware, driver, Vulkan features, tested
kernels, and calibration results available to backend selection.
_Avoid_: GPU info, device cache

**Execution Policy**:
The rules that choose eligible backends and capability tiers for each
simulation stage from a capability profile and workload description.
_Avoid_: Auto mode, heuristic

**Manual Override**:
An explicit user selection that takes precedence over execution policy while
remaining subject to hard correctness and device-safety checks.
_Avoid_: Force mode, custom mode

**Capability Tier**:
A coherent set of Vulkan facilities that a device can safely execute:
compute, ray query, or ray-tracing pipeline.
_Avoid_: GPU level, feature class

**Host Canonical Field**:
The authoritative ViennaHRLE/ViennaLS representation from which a device
working set can be created and to which committed results are synchronized.
_Avoid_: CPU copy, master array

**Device Working Set**:
The backend-owned sparse or dense representation kept resident on a device
across adjacent simulation stages.
_Avoid_: GPU copy, temporary buffer

**Backend Parity**:
The declared numerical equivalence level between a candidate backend and the
governing CPU or existing CUDA reference for a named scenario.
_Avoid_: Exact match, same output

**Functional Threshold**:
The non-negotiable feature, limit, memory, precision, and validation conditions
that make a backend eligible for a simulation stage.
_Avoid_: Minimum GPU, recommended hardware

**Selection Record**:
The per-run explanation of which backend and capability tier were selected or
rejected for every accelerated stage.
_Avoid_: Debug log, GPU log
