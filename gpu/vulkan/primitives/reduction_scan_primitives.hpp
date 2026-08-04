// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class ReductionScanOperation : std::uint32_t {
  reduceFloatBlocks = 0u,
  exclusiveScanIntBlocks = 1u,
  exclusiveScanIntAddOffsets = 2u,
  normalizeFlags = 3u,
  writeCompactionCount = 4u,
  compactFloat = 5u,
  compactUInt32 = 6u,
};

struct ReductionScanStats {
  float sum{};
  float minValue{};
  float maxValue{};
};

struct ReductionScanOptions {
  bool allowInPlaceScan = false;
};

class ReductionScanPrimitives {
public:
  // Device scratch retained by record-only operations until the caller's
  // terminal submission has completed.
  struct DeviceScanScratch {
    std::vector<runtime::DeviceBuffer> blockSums{};
    std::vector<runtime::DeviceBuffer> blockOffsets{};

    void reset() {
      blockSums.clear();
      blockOffsets.clear();
    }
  };

  ReductionScanPrimitives() = default;
  ~ReductionScanPrimitives();

  ReductionScanPrimitives(const ReductionScanPrimitives &) = delete;
  ReductionScanPrimitives &operator=(const ReductionScanPrimitives &) = delete;
  ReductionScanPrimitives(ReductionScanPrimitives &&other) noexcept;
  ReductionScanPrimitives &operator=(ReductionScanPrimitives &&other) noexcept;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  // The bound session object must outlive the primitives. Reset the primitives
  // before resetting, reinitializing, or destroying that session. Moving the
  // session makes the primitives stale; reset them while the moved-to session
  // still owns the Vulkan device before reuse.
  // Reset waits for pending record-only work before destroying the primitive's
  // descriptor pool, layouts, and pipelines.
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] std::uint64_t boundSessionGeneration() const;

  [[nodiscard]] bool createFloatBuffer(std::size_t elementCount,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error);
  [[nodiscard]] bool createIntBuffer(std::size_t elementCount,
                                     runtime::HostVisibleBuffer &buffer,
                                     std::string &error);

  [[nodiscard]] bool reduceSumMinMax(runtime::HostVisibleBuffer &input,
                                     std::size_t elementCount,
                                     ReductionScanStats &stats,
                                     std::string &error);
  [[nodiscard]] bool exclusiveScanInt(runtime::HostVisibleBuffer &input,
                                      std::size_t inputElementCount,
                                      runtime::HostVisibleBuffer &output,
                                      std::size_t outputElementCount,
                                      std::string &error,
                                      ReductionScanOptions options = {});

  // Device-resident variant. Input/output and all recursive scratch buffers
  // remain on the active session's device; no host visibility is required.
  [[nodiscard]] bool exclusiveScanInt(runtime::DeviceBuffer &input,
                                      std::size_t inputElementCount,
                                      runtime::DeviceBuffer &output,
                                      std::size_t outputElementCount,
                                      std::string &error,
                                      ReductionScanOptions options = {});

  // Record-only device variants. The command buffer must already be in the
  // recording state and remains owned by the caller; these methods never
  // reset, begin, end, submit, wait, or perform host transfers.
  [[nodiscard]] bool recordExclusiveScanInt(
      VkCommandBuffer commandBuffer, runtime::DeviceBuffer &input,
      std::size_t inputElementCount, runtime::DeviceBuffer &output,
      std::size_t outputElementCount, DeviceScanScratch &scratch,
      std::string &error, ReductionScanOptions options = {});

  [[nodiscard]] bool recordWriteCompactionCount(VkCommandBuffer commandBuffer,
                                                runtime::DeviceBuffer &flags,
                                                runtime::DeviceBuffer &offsets,
                                                std::size_t elementCount,
                                                runtime::DeviceBuffer &count,
                                                std::string &error);
  [[nodiscard]] bool recordWriteCompactionCount(VkCommandBuffer commandBuffer,
                                                runtime::DeviceBuffer &flags,
                                                runtime::DeviceBuffer &offsets,
                                                std::size_t elementCount,
                                                runtime::DeviceBuffer &count,
                                                DeviceScanScratch &scratch,
                                                std::string &error);

  // Associates the active record lease with the fence that will be passed to
  // the caller-owned terminal vkQueueSubmit. Call this before that submission.
  [[nodiscard]] bool registerRecordTerminalSubmission(
      DeviceScanScratch &scratch, VkFence terminalFence, std::string &error);

  // Recycles descriptor leases only after the registered terminal fence has
  // signaled. The scratch object must not be reset or reused before this call.
  [[nodiscard]] bool reclaimRecordDescriptorSets(DeviceScanScratch &scratch,
                                                 VkFence terminalFence,
                                                 std::string &error);

  // Cancels a registered but unsubmitted terminal lease after the caller has
  // reset or freed its command buffer following a failed vkQueueSubmit.
  [[nodiscard]] bool cancelRecordTerminalSubmission(
      DeviceScanScratch &scratch, VkFence terminalFence, std::string &error);

  [[nodiscard]] bool hasRecordDescriptorLease(
      const DeviceScanScratch &scratch) const;

  // Cancels a record-only lease after the caller has discarded, rather than
  // submitted, its command buffer. The caller must reset or free that command
  // buffer before calling this method. A lease with a registered terminal
  // fence cannot be cancelled.
  [[nodiscard]] bool discardRecordDescriptorSets(DeviceScanScratch &scratch,
                                                 std::string &error);

  [[nodiscard]] bool writeCompactionCount(runtime::DeviceBuffer &flags,
                                          runtime::DeviceBuffer &offsets,
                                          std::size_t elementCount,
                                          runtime::DeviceBuffer &count,
                                          std::string &error);

  [[nodiscard]] bool stableCompactFloat(
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &flags, std::size_t flagElementCount,
      runtime::HostVisibleBuffer &output, std::size_t outputElementCapacity,
      std::size_t &selectedCount, std::string &error);
  [[nodiscard]] bool stableCompactUInt32(
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &flags, std::size_t flagElementCount,
      runtime::HostVisibleBuffer &output, std::size_t outputElementCapacity,
      std::size_t &selectedCount, std::string &error);

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr VkBufferUsageFlags kBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t inputIsTriples;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool validateDeviceLimits(std::string &error) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                    ReductionScanOperation operation,
                                    std::string &error);
  [[nodiscard]] bool createBuffer(std::size_t elementCount,
                                  std::size_t elementSize,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::string &error);
  [[nodiscard]] bool
  validateFloatLength(std::string_view label,
                      const runtime::HostVisibleBuffer &buffer,
                      std::size_t elementCount, std::string &error) const;
  [[nodiscard]] bool validateIntLength(std::string_view label,
                                       const runtime::HostVisibleBuffer &buffer,
                                       std::size_t elementCount,
                                       std::string &error) const;
  [[nodiscard]] bool validateAlias(std::string_view label,
                                   const runtime::HostVisibleBuffer &input,
                                   const runtime::HostVisibleBuffer &output,
                                   bool allowInPlace, std::string &error) const;
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool ensureDummyBuffers(std::string &error);
  [[nodiscard]] bool updateDescriptors(runtime::HostVisibleBuffer &floatInput,
                                       runtime::HostVisibleBuffer &floatOutput,
                                       runtime::HostVisibleBuffer &intInput,
                                       runtime::HostVisibleBuffer &intOutput,
                                       runtime::HostVisibleBuffer &intAux,
                                       std::string &error);
  [[nodiscard]] bool
  dispatchKernel(runtime::ComputePipeline &pipeline, std::size_t dispatchX,
                 const PushConstants &constants,
                 std::span<const VkBufferMemoryBarrier> preBarriers,
                 std::span<const VkBufferMemoryBarrier> postBarriers,
                 std::string &error);
  [[nodiscard]] bool dispatchReduce(runtime::HostVisibleBuffer &input,
                                    runtime::HostVisibleBuffer &output,
                                    std::size_t elementCount,
                                    bool inputIsTriples, std::string &error);
  [[nodiscard]] bool dispatchScanBlocks(runtime::HostVisibleBuffer &input,
                                        runtime::HostVisibleBuffer &output,
                                        runtime::HostVisibleBuffer &blockSums,
                                        std::size_t elementCount,
                                        std::string &error);
  [[nodiscard]] bool dispatchDeviceScanBlocks(runtime::DeviceBuffer &input,
                                              runtime::DeviceBuffer &output,
                                              runtime::DeviceBuffer &blockSums,
                                              std::size_t elementCount,
                                              std::string &error);
  [[nodiscard]] bool
  dispatchScanAddOffsets(runtime::HostVisibleBuffer &output,
                         runtime::HostVisibleBuffer &blockOffsets,
                         std::size_t elementCount, std::string &error);
  [[nodiscard]] bool
  dispatchDeviceScanAddOffsets(runtime::DeviceBuffer &output,
                               runtime::DeviceBuffer &blockOffsets,
                               std::size_t elementCount, std::string &error);
  [[nodiscard]] bool
  dispatchNormalizeFlags(runtime::HostVisibleBuffer &flags,
                         runtime::HostVisibleBuffer &normalizedFlags,
                         std::size_t elementCount, std::string &error);
  [[nodiscard]] bool dispatchCompactionCount(
      runtime::HostVisibleBuffer &normalizedFlags,
      runtime::HostVisibleBuffer &offsets, runtime::HostVisibleBuffer &count,
      std::size_t elementCount, std::size_t &selectedCount, std::string &error);
  [[nodiscard]] bool
  dispatchDeviceCompactionCount(runtime::DeviceBuffer &flags,
                                runtime::DeviceBuffer &offsets,
                                runtime::DeviceBuffer &count,
                                std::size_t elementCount, std::string &error);
  [[nodiscard]] bool recordDeviceScanBlocks(VkCommandBuffer commandBuffer,
                                            runtime::DeviceBuffer &input,
                                            runtime::DeviceBuffer &output,
                                            runtime::DeviceBuffer &blockSums,
                                            std::size_t elementCount,
                                            DeviceScanScratch &scratch,
                                            std::string &error);
  [[nodiscard]] bool
  recordDeviceScanAddOffsets(VkCommandBuffer commandBuffer,
                             runtime::DeviceBuffer &output,
                             runtime::DeviceBuffer &blockOffsets,
                             std::size_t elementCount,
                             DeviceScanScratch &scratch, std::string &error);
  [[nodiscard]] bool recordDeviceCompactionCount(
      VkCommandBuffer commandBuffer, runtime::DeviceBuffer &flags,
      runtime::DeviceBuffer &offsets, runtime::DeviceBuffer &count,
      std::size_t elementCount, DeviceScanScratch &scratch,
      std::string &error);
  [[nodiscard]] bool recordScanIntRecursive(
      VkCommandBuffer commandBuffer, runtime::DeviceBuffer &input,
      std::size_t elementCount, runtime::DeviceBuffer &output,
      DeviceScanScratch &scratch, std::size_t level, std::string &error);
  [[nodiscard]] bool dispatchCompactionScatter(
      runtime::HostVisibleBuffer &input, runtime::HostVisibleBuffer &output,
      runtime::HostVisibleBuffer &normalizedFlags,
      runtime::HostVisibleBuffer &offsets, std::size_t elementCount,
      bool inputIsFloat, std::string &error);
  [[nodiscard]] bool stableCompact(
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &flags, std::size_t flagElementCount,
      runtime::HostVisibleBuffer &output, std::size_t outputElementCapacity,
      std::size_t &selectedCount, bool inputIsFloat, std::string &error);
  [[nodiscard]] bool scanIntRecursive(runtime::HostVisibleBuffer &input,
                                      std::size_t elementCount,
                                      runtime::HostVisibleBuffer &output,
                                      std::string &error);
  [[nodiscard]] bool scanIntRecursive(runtime::DeviceBuffer &input,
                                      std::size_t elementCount,
                                      runtime::DeviceBuffer &output,
                                      std::string &error);
  [[nodiscard]] bool
  validateDeviceIntLength(std::string_view label,
                          const runtime::DeviceBuffer &buffer,
                          std::size_t elementCount, std::string &error) const;
  [[nodiscard]] bool validateDeviceAlias(std::string_view label,
                                         const runtime::DeviceBuffer &input,
                                         const runtime::DeviceBuffer &output,
                                         bool allowInPlace,
                                         std::string &error) const;
  [[nodiscard]] bool updateDeviceDescriptors(std::array<VkBuffer, 5u> buffers,
                                             std::string &error);
  [[nodiscard]] bool registerRecordScratch(VkCommandBuffer commandBuffer,
                                          DeviceScanScratch &scratch,
                                          std::string &error);
  [[nodiscard]] bool
  allocateRecordDescriptorSet(DeviceScanScratch &scratch,
                              VkDescriptorSet &descriptorSet,
                              std::string &error);
  [[nodiscard]] bool
  updateRecordDeviceDescriptors(VkDescriptorSet descriptorSet,
                                std::array<VkBuffer, 5u> buffers,
                                std::string &error);
  [[nodiscard]] bool
  dispatchDeviceKernel(runtime::ComputePipeline &pipeline,
                       std::size_t dispatchX, const PushConstants &constants,
                       std::span<const VkBufferMemoryBarrier> preBarriers,
                       std::span<const VkBufferMemoryBarrier> postBarriers,
                       std::string &error);
  [[nodiscard]] bool recordDeviceKernel(
      VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
      runtime::ComputePipeline &pipeline, std::size_t dispatchX,
      const PushConstants &constants,
      std::span<const VkBufferMemoryBarrier> preBarriers,
      std::span<const VkBufferMemoryBarrier> postBarriers, std::string &error);

  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline reducePipeline_{};
  runtime::ComputePipeline scanBlocksPipeline_{};
  runtime::ComputePipeline scanAddOffsetsPipeline_{};
  runtime::ComputePipeline normalizeFlagsPipeline_{};
  runtime::ComputePipeline compactionCountPipeline_{};
  runtime::ComputePipeline compactFloatPipeline_{};
  runtime::ComputePipeline compactUInt32Pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  runtime::HostVisibleBuffer dummyFloat_{};
  runtime::HostVisibleBuffer dummyInt_{};
  std::unique_ptr<runtime::ComputeSession> ownedSession_{};
  runtime::ComputeSession *activeSession_{nullptr};
  std::uint64_t sessionGeneration_{0};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> reusableRecordDescriptorSets_{};
  struct RecordScratchLease {
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    DeviceScanScratch *scratch{nullptr};
    VkFence terminalFence{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets{};
  };

  std::vector<RecordScratchLease> recordScratchLeases_{};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
