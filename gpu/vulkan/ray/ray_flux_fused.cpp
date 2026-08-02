// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_flux_fused.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace viennaps::vulkan::ray {
namespace {

bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

bool strictFp32(const float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}

bool validOutput(const RayFluxFusedResult &output) {
  return output.surfaceId.size() == output.weight.size() &&
         (output.surfaceId.empty() || (output.surfaceId.data() != nullptr &&
                                       output.weight.data() != nullptr));
}

bool validateInputs(const std::span<const Ray> rays,
                    const std::span<const Triangle> triangles,
                    const std::span<const float> weights, std::string &error) {
  if (rays.size() != weights.size())
    return fail(error, "rays and weights have different lengths");
  if (rays.size() > std::numeric_limits<std::uint32_t>::max() ||
      triangles.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(error, "ray or triangle count exceeds uint32 index range");
  if ((!rays.empty() && rays.data() == nullptr) ||
      (!triangles.empty() && triangles.data() == nullptr) ||
      (!weights.empty() && weights.data() == nullptr))
    return fail(error, "ray, triangle, or weight span has a null pointer");
  for (const auto &ray : rays) {
    for (const float value : ray.origin)
      if (!strictFp32(value))
        return fail(error, "ray origin contains a non-normal FP32 value");
    for (const float value : ray.direction)
      if (!strictFp32(value))
        return fail(error, "ray direction contains a non-normal FP32 value");
    if (!strictFp32(ray.tMin) || !strictFp32(ray.tMax) || ray.tMin < 0.0F ||
        ray.tMax < ray.tMin)
      return fail(error, "ray near/far limits are outside the FP32 domain");
  }
  for (const auto &triangle : triangles)
    for (const auto vertex : {triangle.a, triangle.b, triangle.c})
      for (const float value : vertex)
        if (!strictFp32(value))
          return fail(error,
                      "triangle vertex contains a non-normal FP32 value");
  for (const float value : weights)
    if (!strictFp32(value))
      return fail(error,
                  "ray weights must be finite normal FP32 values or zero");
  return true;
}

std::size_t checkedBytes(const std::size_t elements, const std::size_t size) {
  if (elements > (std::numeric_limits<std::size_t>::max() / size))
    return 0U;
  const auto bytes = elements * size;
  return bytes == 0U ? size : bytes;
}

bool exactEqual(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs);
}

} // namespace

FusedRayFluxPrimitive::~FusedRayFluxPrimitive() { reset(); }

bool FusedRayFluxPrimitive::initialize(const std::string_view spirvPath,
                                       std::string &error) {
  error.clear();
  reset();
  if (spirvPath.empty())
    return fail(error, "fused ray-flux SPIR-V path is empty");
  if (!ownedSession_.initialize(error))
    return false;
  session_ = &ownedSession_;
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 8U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      4U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(
          session_->device(), descriptorSetLayout_.get(),
          std::span<const VkPushConstantRange>(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 8U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error) ||
      !pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        error)) {
    reset();
    return false;
  }
  return true;
}

bool FusedRayFluxPrimitive::initialize(runtime::ComputeSession &session,
                                       const std::string_view spirvPath,
                                       std::string &error) {
  error.clear();
  reset();
  if (spirvPath.empty())
    return fail(error, "fused ray-flux SPIR-V path is empty");
  if (!session.isValid())
    return fail(error, "external compute session is not initialized");
  session_ = &session;
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 8U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      4U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(
          session_->device(), descriptorSetLayout_.get(),
          std::span<const VkPushConstantRange>(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 8U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error) ||
      !pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        error)) {
    reset();
    return false;
  }
  return true;
}

void FusedRayFluxPrimitive::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
  session_ = nullptr;
  if (ownsSession)
    ownedSession_.reset();
}

bool FusedRayFluxPrimitive::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

const runtime::VulkanDevice &FusedRayFluxPrimitive::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

bool FusedRayFluxPrimitive::runCpu(const std::span<const Ray> rays,
                                   const std::span<const Triangle> triangles,
                                   const std::span<const float> weights,
                                   RayFluxFusedResult &output,
                                   std::string &error) const {
  error.clear();
  if (!validOutput(output) ||
      !validateInputs(rays, triangles, weights, error)) {
    if (error.empty())
      error = "ray-flux output columns have different lengths";
    return false;
  }
  if (rays.empty())
    return true;
  std::vector<TriangleHit> hits(rays.size(), TriangleHit::miss());
  if (!intersectCpu(rays, triangles, hits, error))
    return false;
  std::vector<std::uint32_t> ids(rays.size());
  std::vector<std::uint32_t> surfaces(rays.size());
  std::vector<float> compactWeights(rays.size());
  RayHitBatch batch{ids, surfaces, compactWeights, 0U};
  if (!compactCpu(hits, weights, static_cast<std::uint32_t>(triangles.size()),
                  batch, error))
    return false;
  RayRecordSoA records{
      std::span<const std::uint32_t>(ids.data(), batch.count),
      std::span<const std::uint32_t>(surfaces.data(), batch.count),
      std::span<const float>(compactWeights.data(), batch.count),
      {}};
  std::vector<std::uint32_t> stagedSurface(output.surfaceId.size());
  std::vector<float> stagedWeight(output.weight.size());
  RayReduction reduction{stagedSurface, stagedWeight, 0U};
  if (!reduceCpu(records, static_cast<std::uint32_t>(triangles.size()),
                 reduction, error))
    return false;
  std::copy_n(stagedSurface.data(), reduction.count, output.surfaceId.data());
  std::copy_n(stagedWeight.data(), reduction.count, output.weight.data());
  output.count = reduction.count;
  return true;
}

bool FusedRayFluxPrimitive::runGpu(const std::span<const Ray> rays,
                                   const std::span<const Triangle> triangles,
                                   const std::span<const float> weights,
                                   RayFluxFusedResult &output,
                                   std::string &error) {
  error.clear();
  if (!isInitialized())
    return fail(error, "fused ray-flux primitive is not initialized");
  if (!validOutput(output) ||
      !validateInputs(rays, triangles, weights, error)) {
    if (error.empty())
      error = "ray-flux output columns have different lengths";
    return false;
  }
  if (rays.empty())
    return true;
  if (output.surfaceId.size() >= std::numeric_limits<std::uint32_t>::max())
    return fail(error, "ray-flux output capacity exceeds uint32 range");

  // Build an independent oracle before touching caller storage; this also
  // performs the capacity check transactionally.
  std::vector<std::uint32_t> expectedSurface(output.surfaceId.size());
  std::vector<float> expectedWeight(output.weight.size());
  RayFluxFusedResult expected{expectedSurface, expectedWeight, 0U};
  if (!runCpu(rays, triangles, weights, expected, error))
    return false;

  runtime::DeviceBuffer origins, directions, triangleBuffer, weightBuffer, used,
      outputSurface, outputWeight, outputCount;
  const auto originBytes =
      checkedBytes(rays.size(), sizeof(std::array<float, 4>));
  const auto triangleBytes =
      checkedBytes(triangles.size() * 3U, sizeof(std::array<float, 4>));
  const auto weightBytes = checkedBytes(weights.size(), sizeof(float));
  const auto outputIdBytes =
      checkedBytes(output.surfaceId.size(), sizeof(std::uint32_t));
  const auto outputWeightBytes =
      checkedBytes(output.weight.size(), sizeof(float));
  if (originBytes == 0U || triangleBytes == 0U || weightBytes == 0U ||
      outputIdBytes == 0U || outputWeightBytes == 0U ||
      rays.size() > std::numeric_limits<std::uint32_t>::max() ||
      triangles.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(error, "fused ray-flux buffer size overflow");
  if (!origins.create(*session_, originBytes, error) ||
      !directions.create(*session_, originBytes, error) ||
      !triangleBuffer.create(*session_, triangleBytes, error) ||
      !weightBuffer.create(*session_, weightBytes, error) ||
      !used.create(*session_, checkedBytes(rays.size(), sizeof(std::uint32_t)),
                   error) ||
      !outputSurface.create(*session_, outputIdBytes, error) ||
      !outputWeight.create(*session_, outputWeightBytes, error) ||
      !outputCount.create(*session_, sizeof(std::uint32_t), error))
    return false;

  std::vector<std::array<float, 4>> packedOrigin, packedDirection,
      packedTriangles;
  packedOrigin.reserve(rays.size());
  packedDirection.reserve(rays.size());
  packedTriangles.reserve(triangles.size() * 3U);
  for (const auto &ray : rays) {
    packedOrigin.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    packedDirection.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  for (const auto &triangle : triangles) {
    packedTriangles.push_back(
        {triangle.a[0], triangle.a[1], triangle.a[2], 0.0F});
    packedTriangles.push_back(
        {triangle.b[0], triangle.b[1], triangle.b[2], 0.0F});
    packedTriangles.push_back(
        {triangle.c[0], triangle.c[1], triangle.c[2], 0.0F});
  }
  std::vector<std::uint32_t> zeroUsed(rays.size(), 0U);
  const std::uint32_t zero = 0U;
  if (!origins.upload(*session_, packedOrigin.data(), originBytes, 0U, error) ||
      !directions.upload(*session_, packedDirection.data(), originBytes, 0U,
                         error) ||
      !triangleBuffer.upload(*session_, packedTriangles.data(), triangleBytes,
                             0U, error) ||
      !weightBuffer.upload(*session_, weights.data(), weightBytes, 0U, error) ||
      !used.upload(*session_, zeroUsed.data(),
                   zeroUsed.size() * sizeof(std::uint32_t), 0U, error) ||
      !outputCount.upload(*session_, &zero, sizeof(zero), 0U, error))
    return false;

  const std::array<VkBuffer, 8U> buffers = {
      origins.handle(),      directions.handle(), triangleBuffer.handle(),
      weightBuffer.handle(), used.handle(),       outputSurface.handle(),
      outputWeight.handle(), outputCount.handle()};
  std::array<VkDescriptorBufferInfo, 8U> infos{};
  std::array<VkWriteDescriptorSet, 8U> writes{};
  for (std::uint32_t i = 0U; i < buffers.size(); ++i) {
    infos[i] = {buffers[i], 0U, VK_WHOLE_SIZE};
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptorSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1U;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);

  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "vkResetCommandBuffer failed");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "vkBeginCommandBuffer failed");
  std::array<VkBufferMemoryBarrier, 8U> pre{}, post{};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              buffers[i],
              0U,
              VK_WHOLE_SIZE};
    post[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
               nullptr,
               VK_ACCESS_SHADER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT,
               VK_QUEUE_FAMILY_IGNORED,
               VK_QUEUE_FAMILY_IGNORED,
               buffers[i],
               0U,
               VK_WHOLE_SIZE};
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const std::array<std::uint32_t, 4U> params = {
      static_cast<std::uint32_t>(rays.size()),
      static_cast<std::uint32_t>(triangles.size()),
      static_cast<std::uint32_t>(output.surfaceId.size()), 0U};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(params),
                     params.data());
  vkCmdDispatch(commandBuffer_, 1U, 1U, 1U);
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(post.size()), post.data(), 0U,
                       nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "vkEndCommandBuffer failed");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS)
    return fail(error, "vkQueueSubmit failed");
  if (!fence_.wait(std::numeric_limits<std::uint64_t>::max(), error))
    return false;
  fence_.reset();

  std::uint32_t gpuCount = 0U;
  std::vector<std::uint32_t> gpuSurface(expected.count);
  std::vector<float> gpuWeight(expected.count);
  if (!outputCount.download(*session_, &gpuCount, sizeof(gpuCount), 0U,
                            error) ||
      gpuCount != expected.count ||
      (gpuCount != 0U &&
       (!outputSurface.download(*session_, gpuSurface.data(),
                                gpuCount * sizeof(std::uint32_t), 0U, error) ||
        !outputWeight.download(*session_, gpuWeight.data(),
                               gpuCount * sizeof(float), 0U, error))))
    return gpuCount != expected.count
               ? fail(error, "fused ray-flux GPU count differs from CPU oracle")
               : false;
  for (std::size_t i = 0U; i < gpuCount; ++i)
    if (gpuSurface[i] != expected.surfaceId[i] ||
        !exactEqual(gpuWeight[i], expected.weight[i]))
      return fail(error,
                  "fused ray-flux GPU result differs bitwise from CPU oracle");
  std::copy_n(gpuSurface.data(), gpuCount, output.surfaceId.data());
  std::copy_n(gpuWeight.data(), gpuCount, output.weight.data());
  output.count = gpuCount;
  return true;
}

} // namespace viennaps::vulkan::ray
