// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Minimal reusable Vulkan compute runtime scaffolding for ViennaPS P2 runtime
// work. Scope: reusable RAII helpers for host-visible buffers, shader modules,
// descriptor/pipeline setup, command recording, and fence synchronization.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace viennaps::vulkan::runtime {

struct ComputeQueueSpec {
  std::uint32_t familyIndex = 0;
  bool dedicatedQueue = false;
};

struct SpirvProgram {
  std::vector<std::uint32_t> words;
};

[[nodiscard]] const char *vkResultToString(VkResult result);

[[nodiscard]] std::string_view vkDeviceTypeName(VkPhysicalDeviceType type);

[[nodiscard]] bool readSpirv(std::string_view path, SpirvProgram &program,
                             std::string &error);

[[nodiscard]] bool selectComputeQueueFamily(VkPhysicalDevice device,
                                            ComputeQueueSpec &spec,
                                            std::string &error);

struct ComputeDeviceSelection {
  VkPhysicalDevice handle = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  ComputeQueueSpec queue{};
};

[[nodiscard]] bool pickFirstComputeDevice(VkInstance instance,
                                          ComputeDeviceSelection &selection,
                                          std::string &error);

class VulkanInstance {
public:
  VulkanInstance() = default;
  ~VulkanInstance();
  VulkanInstance(const VulkanInstance &) = delete;
  VulkanInstance &operator=(const VulkanInstance &) = delete;

  VulkanInstance(VulkanInstance &&other) noexcept;
  VulkanInstance &operator=(VulkanInstance &&other) noexcept;

  [[nodiscard]] bool create(std::string &error);
  void reset();
  [[nodiscard]] bool isValid() const;
  [[nodiscard]] VkInstance get() const;

private:
  VkInstance instance_{VK_NULL_HANDLE};
};

class VulkanDevice {
public:
  VulkanDevice() = default;
  ~VulkanDevice();
  VulkanDevice(const VulkanDevice &) = delete;
  VulkanDevice &operator=(const VulkanDevice &) = delete;

  VulkanDevice(VulkanDevice &&other) noexcept;
  VulkanDevice &operator=(VulkanDevice &&other) noexcept;

  [[nodiscard]] bool create(const ComputeDeviceSelection &selection,
                            std::string &error);
  void reset();

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] VkDevice get() const;
  [[nodiscard]] VkQueue computeQueue() const;
  [[nodiscard]] std::uint32_t computeQueueFamily() const;
  [[nodiscard]] VkPhysicalDevice physical() const;
  [[nodiscard]] const ComputeDeviceSelection &selection() const;

private:
  VkPhysicalDevice physical_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue computeQueue_{VK_NULL_HANDLE};
  std::uint32_t queueFamily_ = 0;
  ComputeDeviceSelection selection_{};
};

class HostVisibleBuffer {
public:
  HostVisibleBuffer() = default;
  ~HostVisibleBuffer();
  HostVisibleBuffer(const HostVisibleBuffer &) = delete;
  HostVisibleBuffer &operator=(const HostVisibleBuffer &) = delete;

  HostVisibleBuffer(HostVisibleBuffer &&other) noexcept;
  HostVisibleBuffer &operator=(HostVisibleBuffer &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device, VkDeviceSize bytes,
                            VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags required, std::string &error);
  void reset();
  [[nodiscard]] bool map(std::string &error);
  void unmap();
  [[nodiscard]] bool write(const void *data, VkDeviceSize bytes,
                           VkDeviceSize offset, std::string &error);
  [[nodiscard]] bool read(void *data, VkDeviceSize bytes, VkDeviceSize offset,
                          std::string &error);
  [[nodiscard]] bool flush(std::string &error);
  [[nodiscard]] bool invalidate(std::string &error);

  [[nodiscard]] bool isValid() const;
  [[nodiscard]] VkBuffer handle() const;
  [[nodiscard]] VkDeviceMemory memory() const;
  [[nodiscard]] VkDeviceSize size() const;
  [[nodiscard]] void *mappedPtr() const;
  [[nodiscard]] bool hostCoherent() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkDevice deviceForDestroy_{VK_NULL_HANDLE};
  VkBuffer buffer_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkDeviceSize bytes_{0};
  void *mapped_{nullptr};
  bool hostCoherent_{false};
  bool beginMap(std::string &error);
};

class ShaderModule {
public:
  ShaderModule() = default;
  ~ShaderModule();
  ShaderModule(const ShaderModule &) = delete;
  ShaderModule &operator=(const ShaderModule &) = delete;

  ShaderModule(ShaderModule &&other) noexcept;
  ShaderModule &operator=(ShaderModule &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device, const SpirvProgram &program,
                            std::string &error);
  void reset();
  [[nodiscard]] VkShaderModule get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkShaderModule module_{VK_NULL_HANDLE};
};

class DescriptorSetLayout {
public:
  DescriptorSetLayout() = default;
  ~DescriptorSetLayout();
  DescriptorSetLayout(const DescriptorSetLayout &) = delete;
  DescriptorSetLayout &operator=(const DescriptorSetLayout &) = delete;

  DescriptorSetLayout(DescriptorSetLayout &&other) noexcept;
  DescriptorSetLayout &operator=(DescriptorSetLayout &&other) noexcept;

  [[nodiscard]] bool
  create(VulkanDevice &device,
         std::span<const VkDescriptorSetLayoutBinding> bindings,
         std::string &error);
  void reset();
  [[nodiscard]] VkDescriptorSetLayout get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
};

class PipelineLayout {
public:
  PipelineLayout() = default;
  ~PipelineLayout();
  PipelineLayout(const PipelineLayout &) = delete;
  PipelineLayout &operator=(const PipelineLayout &) = delete;

  PipelineLayout(PipelineLayout &&other) noexcept;
  PipelineLayout &operator=(PipelineLayout &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device,
                            VkDescriptorSetLayout descriptorSetLayout,
                            std::string &error);
  [[nodiscard]] bool
  create(VulkanDevice &device, VkDescriptorSetLayout descriptorSetLayout,
         std::span<const VkPushConstantRange> pushConstantRanges,
         std::string &error);
  void reset();
  [[nodiscard]] VkPipelineLayout get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
};

struct ComputePipelineOptions {
  std::string_view entryPoint = "main";
  std::span<const VkSpecializationMapEntry> specializationEntries{};
  const void *specializationData = nullptr;
  std::size_t specializationDataSize = 0;
};

class ComputePipeline {
public:
  ComputePipeline() = default;
  ~ComputePipeline();
  ComputePipeline(const ComputePipeline &) = delete;
  ComputePipeline &operator=(const ComputePipeline &) = delete;

  ComputePipeline(ComputePipeline &&other) noexcept;
  ComputePipeline &operator=(ComputePipeline &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device, const ShaderModule &shader,
                            const PipelineLayout &layout, std::string &error);
  [[nodiscard]] bool create(VulkanDevice &device, const ShaderModule &shader,
                            const PipelineLayout &layout,
                            const ComputePipelineOptions &options,
                            std::string &error);
  void reset();
  [[nodiscard]] VkPipeline get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
};

class DescriptorPool {
public:
  DescriptorPool() = default;
  ~DescriptorPool();
  DescriptorPool(const DescriptorPool &) = delete;
  DescriptorPool &operator=(const DescriptorPool &) = delete;

  DescriptorPool(DescriptorPool &&other) noexcept;
  DescriptorPool &operator=(DescriptorPool &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device,
                            std::uint32_t descriptorSetCount,
                            VkDescriptorType descriptorType,
                            std::string &error);
  [[nodiscard]] bool create(VulkanDevice &device,
                            std::uint32_t descriptorSetCount,
                            std::uint32_t descriptorsPerSet,
                            VkDescriptorType descriptorType,
                            std::string &error);
  [[nodiscard]] bool allocate(VkDescriptorSetLayout layout,
                              VkDescriptorSet &set, std::string &error);
  void reset();
  [[nodiscard]] VkDescriptorPool get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorPool pool_{VK_NULL_HANDLE};
};

class CommandContext {
public:
  CommandContext() = default;
  ~CommandContext();
  CommandContext(const CommandContext &) = delete;
  CommandContext &operator=(const CommandContext &) = delete;

  CommandContext(CommandContext &&other) noexcept;
  CommandContext &operator=(CommandContext &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device,
                            std::uint32_t queueFamilyIndex, std::string &error);
  void reset();
  [[nodiscard]] bool allocatePrimary(VkCommandBuffer &commandBuffer,
                                     std::string &error);
  [[nodiscard]] VkCommandPool pool() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkCommandPool pool_{VK_NULL_HANDLE};
};

class Fence {
public:
  Fence() = default;
  ~Fence();
  Fence(const Fence &) = delete;
  Fence &operator=(const Fence &) = delete;

  Fence(Fence &&other) noexcept;
  Fence &operator=(Fence &&other) noexcept;

  [[nodiscard]] bool create(VulkanDevice &device, std::string &error);
  [[nodiscard]] bool wait(std::uint64_t timeoutNs, std::string &error) const;
  void reset() const;
  void reset(VulkanDevice &device);
  void destroy();

  [[nodiscard]] VkFence get() const;

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkFence fence_{VK_NULL_HANDLE};
};

[[nodiscard]] std::uint32_t orderedFloatBits(float value);
[[nodiscard]] bool exactlyEqualFloat(float lhs, float rhs);

} // namespace viennaps::vulkan::runtime
