---
status: accepted
---

# Add a capability-driven Vulkan compute backend

ViennaPS will retain its CPU and CUDA/OptiX paths and add an optional Vulkan
backend through coordinated interfaces in ViennaCore, ViennaLS, ViennaRay, and
ViennaPS. ViennaHRLE/ViennaLS remains the host-canonical representation while
Vulkan owns sparse or workload-specific dense device working sets; this avoids
forking the physical models and avoids imposing a dense global grid on existing
users.

Backend selection is capability-driven. Deployment or first use records a
versioned hardware capability profile, and each simulation stage automatically
selects the best eligible implementation using hard functional thresholds,
workload requirements, and calibrated performance. A manual override always
takes precedence over automatic policy, but cannot silently bypass correctness,
memory-safety, or required-feature checks; an unsupported explicit selection
fails with an actionable diagnostic.

## Considered options

- Replacing CUDA was rejected because it would regress mature NVIDIA/OptiX
  deployments and make numerical migration harder to validate.
- Implementing Vulkan only inside ViennaPS was rejected because level-set,
  ray-tracing, allocation, and solver ownership already cross ViennaLS,
  ViennaRay, and ViennaCore boundaries.
- Making a dense signed-distance grid canonical was rejected because it changes
  ViennaHRLE memory scaling. Dense device fields remain available for bounded,
  regular-grid workloads such as oxidation.
- Requiring Vulkan ray-tracing extensions was rejected because Vulkan compute
  is broadly available while ray-tracing extensions are optional. Software BVH,
  ray query, and ray-tracing pipeline are separate capability tiers.

## Consequences

The first deliverable is a backend contract, device profiler, and parity suite,
not a direct translation of CUDA kernels. Device and driver changes invalidate
the stored profile and pipeline cache. Every run records its selection decisions
so automatic fallback is observable and reproducible.
