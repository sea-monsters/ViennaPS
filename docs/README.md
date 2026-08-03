# Development documents

- [Vulkan compute acceleration development report](design/vulkan-compute-acceleration-development-report.md)
- [Vulkan compute acceleration implementation status](design/vulkan-compute-acceleration-status.md)
- [Build and parallel-development guide](development-build-and-worktree-guide.md)
- [ADR 0001: Add a capability-driven Vulkan compute backend](adr/0001-add-capability-driven-vulkan-backend.md)
- [Compute acceleration language](../CONTEXT.md)

The report and ADR describe the accepted direction and staged acceptance
gates. The implementation status records only slices backed by local validation
evidence; it does not imply completion of the full Vulkan backend.

All implementation roles should read the build and parallel-development guide
before claiming a task. It defines the Windows CMake entry point, CPM/ViennaLS/
Embree expectations, worktree isolation, validation evidence, and cleanup
rules.
