# Development documents

- [Vulkan program intent framework (whitepaper)](design/vulkan-program-intent-framework.md)
- [Vulkan compute acceleration development report](design/vulkan-compute-acceleration-development-report.md)
- [Vulkan compute acceleration implementation status](design/vulkan-compute-acceleration-status.md)
- [P5 Formal Exit execution plan](design/p5-formal-exit-execution-plan.md)
- [P0–P4 CPU-reuse audit Round 1](design/p0-p4-cpu-reuse-audit-round1.md)
- [Build and parallel-development guide](development-build-and-worktree-guide.md)
- [ADR 0001: Add a capability-driven Vulkan compute backend](adr/0001-add-capability-driven-vulkan-backend.md)
- [Compute acceleration language](../CONTEXT.md)

The report and ADR describe the accepted direction and staged acceptance
gates. The implementation status records only slices backed by local validation
evidence; it does not imply completion of the full Vulkan backend. The Round 1
audit records whether P0–P4 reused the CPU path outside compute; follow-ups in
that ledger are not closed merely by recording the audit.

All implementation roles should read the build and parallel-development guide
before claiming a task. It defines the Windows CMake entry point, CPM/ViennaLS/
Embree expectations, worktree isolation, validation evidence, and cleanup
rules.
