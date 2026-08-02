// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "triangle_hit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace viennaps::vulkan::ray {
namespace {

constexpr float kDeterminantEpsilon = 1.0e-7F;

[[nodiscard]] bool strictFp32(const float value) {
  if (!std::isfinite(value)) {
    return false;
  }
  return value == 0.0F || std::abs(value) >= std::numeric_limits<float>::min();
}

[[nodiscard]] bool overlaps(const void *left, const std::size_t leftBytes,
                            const void *right, const std::size_t rightBytes) {
  if (leftBytes == 0U || rightBytes == 0U || left == nullptr ||
      right == nullptr) {
    return false;
  }
  const auto leftBegin = reinterpret_cast<std::uintptr_t>(left);
  const auto rightBegin = reinterpret_cast<std::uintptr_t>(right);
  return leftBegin < rightBegin + rightBytes &&
         rightBegin < leftBegin + leftBytes;
}

[[nodiscard]] std::array<float, 3> subtract(const std::array<float, 3> &left,
                                            const std::array<float, 3> &right) {
  return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] std::array<float, 3> cross(const std::array<float, 3> &left,
                                         const std::array<float, 3> &right) {
  return {left[1] * right[2] - left[2] * right[1],
          left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] float dot(const std::array<float, 3> &left,
                        const std::array<float, 3> &right) {
  volatile float sum = left[0] * right[0];
  sum = sum + left[1] * right[1];
  sum = sum + left[2] * right[2];
  return sum;
}

[[nodiscard]] bool invalid(std::string &error, const char *message) {
  error = message;
  return false;
}

} // namespace

bool intersectCpu(const std::span<const Ray> rays,
                  const std::span<const Triangle> triangles,
                  const std::span<TriangleHit> output, std::string &error) {
  error.clear();
  if (rays.size() > std::numeric_limits<std::uint32_t>::max() ||
      triangles.size() > std::numeric_limits<std::uint32_t>::max()) {
    return invalid(error, "ray or triangle count exceeds uint32 index range");
  }
  if (output.size() < rays.size()) {
    return invalid(error, "triangle-hit output capacity is insufficient");
  }
  if ((!rays.empty() && rays.data() == nullptr) ||
      (!triangles.empty() && triangles.data() == nullptr) ||
      (!output.empty() && output.data() == nullptr)) {
    return invalid(error, "ray, triangle, or output span has a null pointer");
  }
  if (overlaps(rays.data(), rays.size_bytes(), output.data(),
               output.size_bytes()) ||
      overlaps(triangles.data(), triangles.size_bytes(), output.data(),
               output.size_bytes())) {
    return invalid(error,
                   "triangle-hit input and output buffers must not alias");
  }

  for (const auto &ray : rays) {
    for (const auto value : ray.origin) {
      if (!strictFp32(value)) {
        return invalid(error, "ray origin contains a non-normal FP32 value");
      }
    }
    for (const auto value : ray.direction) {
      if (!strictFp32(value)) {
        return invalid(error, "ray direction contains a non-normal FP32 value");
      }
    }
    if (!strictFp32(ray.tMin) || !strictFp32(ray.tMax) || ray.tMin < 0.0F ||
        ray.tMax < ray.tMin) {
      return invalid(error, "ray near/far limits are outside the FP32 domain");
    }
  }
  for (const auto &triangle : triangles) {
    for (const auto vertex : {triangle.a, triangle.b, triangle.c}) {
      for (const auto value : vertex) {
        if (!strictFp32(value)) {
          return invalid(error,
                         "triangle vertex contains a non-normal FP32 value");
        }
      }
    }
  }

  std::vector<TriangleHit> staged(rays.size(), TriangleHit::miss());
  for (std::size_t rayIndex = 0U; rayIndex < rays.size(); ++rayIndex) {
    const auto &ray = rays[rayIndex];
    auto &best = staged[rayIndex];
    for (std::size_t triangleIndex = 0U; triangleIndex < triangles.size();
         ++triangleIndex) {
      const auto &triangle = triangles[triangleIndex];
      const auto edge1 = subtract(triangle.b, triangle.a);
      const auto edge2 = subtract(triangle.c, triangle.a);
      const auto pvec = cross(ray.direction, edge2);
      const float determinant = dot(edge1, pvec);
      if (!std::isfinite(determinant) ||
          std::abs(determinant) <= kDeterminantEpsilon) {
        continue;
      }
      const float inverseDeterminant = 1.0F / determinant;
      const auto tvec = subtract(ray.origin, triangle.a);
      const float u = dot(tvec, pvec) * inverseDeterminant;
      if (!std::isfinite(u) || u < 0.0F || u > 1.0F) {
        continue;
      }
      const auto qvec = cross(tvec, edge1);
      const float v = dot(ray.direction, qvec) * inverseDeterminant;
      if (!std::isfinite(v) || v < 0.0F || u + v > 1.0F) {
        continue;
      }
      const float t = dot(edge2, qvec) * inverseDeterminant;
      if (!std::isfinite(t) || t < ray.tMin || t > ray.tMax) {
        continue;
      }
      if (best.isMiss() || t < best.t) {
        best = {t, static_cast<std::uint32_t>(triangleIndex), u, v};
      }
    }
  }
  std::copy(staged.begin(), staged.end(), output.begin());
  return true;
}

TriangleHitPrimitive::~TriangleHitPrimitive() { reset(); }

TriangleHitPrimitive::TriangleHitPrimitive(
    TriangleHitPrimitive &&other) noexcept
    : ownedSession_(std::move(other.ownedSession_)), session_(other.session_),
      shaderModule_(std::move(other.shaderModule_)),
      descriptorSetLayout_(std::move(other.descriptorSetLayout_)),
      pipelineLayout_(std::move(other.pipelineLayout_)),
      pipeline_(std::move(other.pipeline_)),
      descriptorPool_(std::move(other.descriptorPool_)),
      commandContext_(std::move(other.commandContext_)),
      fence_(std::move(other.fence_)), descriptorSet_(other.descriptorSet_),
      commandBuffer_(other.commandBuffer_) {
  if (session_ == &other.ownedSession_) {
    session_ = &ownedSession_;
  }
  other.session_ = nullptr;
  other.descriptorSet_ = VK_NULL_HANDLE;
  other.commandBuffer_ = VK_NULL_HANDLE;
}

TriangleHitPrimitive &
TriangleHitPrimitive::operator=(TriangleHitPrimitive &&other) noexcept {
  if (this != &other) {
    reset();
    ownedSession_ = std::move(other.ownedSession_);
    session_ = other.session_ == &other.ownedSession_ ? &ownedSession_
                                                      : other.session_;
    shaderModule_ = std::move(other.shaderModule_);
    descriptorSetLayout_ = std::move(other.descriptorSetLayout_);
    pipelineLayout_ = std::move(other.pipelineLayout_);
    pipeline_ = std::move(other.pipeline_);
    descriptorPool_ = std::move(other.descriptorPool_);
    commandContext_ = std::move(other.commandContext_);
    fence_ = std::move(other.fence_);
    descriptorSet_ = std::exchange(other.descriptorSet_, VK_NULL_HANDLE);
    commandBuffer_ = std::exchange(other.commandBuffer_, VK_NULL_HANDLE);
    other.session_ = nullptr;
  }
  return *this;
}

bool TriangleHitPrimitive::setup(const std::string_view spirvPath,
                                 runtime::ComputeSession *externalSession,
                                 std::string &error) {
  if (spirvPath.empty()) {
    return invalid(error, "triangle-hit SPIR-V path is empty");
  }
  reset();
  if (externalSession != nullptr) {
    if (!externalSession->isValid()) {
      return invalid(error, "external compute session is not initialized");
    }
    session_ = externalSession;
  } else {
    if (!ownedSession_.initialize(error)) {
      return false;
    }
    session_ = &ownedSession_;
  }
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 4U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i) {
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      2U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(
          session_->device(), descriptorSetLayout_.get(),
          std::span<const VkPushConstantRange>(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 4U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    reset();
    return false;
  }
  const runtime::ComputePipelineOptions options{"main", {}, nullptr, 0U};
  if (!pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        options, error)) {
    reset();
    return false;
  }
  return true;
}

bool TriangleHitPrimitive::initialize(const std::string_view spirvPath,
                                      std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}

bool TriangleHitPrimitive::initialize(runtime::ComputeSession &session,
                                      const std::string_view spirvPath,
                                      std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

void TriangleHitPrimitive::reset() {
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
  if (ownsSession) {
    ownedSession_.reset();
  }
}

bool TriangleHitPrimitive::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool TriangleHitPrimitive::ready(std::string &error) const {
  if (!isInitialized()) {
    return invalid(error, "triangle-hit primitive is not initialized");
  }
  return true;
}

bool TriangleHitPrimitive::createBuffer(const std::size_t count,
                                        const std::size_t elementSize,
                                        runtime::HostVisibleBuffer &buffer,
                                        std::string &error) {
  if (!ready(error)) {
    return false;
  }
  const std::size_t allocated = count == 0U ? 1U : count;
  if (allocated > std::numeric_limits<std::size_t>::max() / elementSize) {
    return invalid(error, "triangle-hit buffer size overflows host size");
  }
  return buffer.create(session_->device(),
                       static_cast<VkDeviceSize>(allocated * elementSize),
                       kUsage, kMemory, error);
}

bool TriangleHitPrimitive::createRayBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &origin,
    runtime::HostVisibleBuffer &direction, std::string &error) {
  return createBuffer(count, sizeof(float) * 4U, origin, error) &&
         createBuffer(count, sizeof(float) * 4U, direction, error);
}

bool TriangleHitPrimitive::createTriangleBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (count > std::numeric_limits<std::size_t>::max() / 3U) {
    return invalid(error, "triangle count overflows buffer size");
  }
  return createBuffer(count * 3U, sizeof(float) * 4U, buffer, error);
}

bool TriangleHitPrimitive::createHitBuffer(const std::size_t count,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error) {
  return createBuffer(count, sizeof(TriangleHit), buffer, error);
}

bool TriangleHitPrimitive::intersect(
    runtime::HostVisibleBuffer &origin, runtime::HostVisibleBuffer &direction,
    runtime::HostVisibleBuffer &triangles, const std::size_t rayCount,
    const std::size_t triangleCount, runtime::HostVisibleBuffer &hits,
    const std::size_t outputCapacity, std::string &error) {
  error.clear();
  if (!ready(error)) {
    return false;
  }
  if (rayCount > std::numeric_limits<std::uint32_t>::max() ||
      triangleCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < rayCount ||
      rayCount > origin.size() / (4U * sizeof(float)) ||
      rayCount > direction.size() / (4U * sizeof(float)) ||
      triangleCount > triangles.size() / (12U * sizeof(float)) ||
      outputCapacity > hits.size() / sizeof(TriangleHit)) {
    return invalid(error,
                   "triangle-hit buffer capacity or index range is invalid");
  }
  const std::array<runtime::HostVisibleBuffer *, 4U> buffers = {
      &origin, &direction, &triangles, &hits};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (buffers[i]->ownerDevice() != session_->device().get()) {
      return invalid(error,
                     "triangle-hit buffer belongs to another Vulkan device");
    }
    for (std::size_t j = i + 1U; j < buffers.size(); ++j) {
      if (buffers[i]->handle() == buffers[j]->handle()) {
        return invalid(error,
                       "triangle-hit input/output buffers must not alias");
      }
    }
  }
  if (rayCount == 0U) {
    return true;
  }
  if (!origin.map(error) || !direction.map(error) || !triangles.map(error) ||
      !hits.map(error) || !origin.flush(error) || !direction.flush(error) ||
      !triangles.flush(error)) {
    return false;
  }
  const auto *origins = static_cast<const float *>(origin.mappedPtr());
  const auto *directions = static_cast<const float *>(direction.mappedPtr());
  const auto *vertices = static_cast<const float *>(triangles.mappedPtr());
  for (std::size_t i = 0U; i < rayCount * 4U; ++i) {
    if (!strictFp32(origins[i]) || !strictFp32(directions[i])) {
      return invalid(error, "ray buffer contains a non-normal FP32 value");
    }
  }
  for (std::size_t i = 0U; i < triangleCount * 12U; ++i) {
    if (!strictFp32(vertices[i])) {
      return invalid(error, "triangle buffer contains a non-normal FP32 value");
    }
  }
  for (std::size_t i = 0U; i < rayCount; ++i) {
    if (origins[4U * i + 3U] < 0.0F ||
        directions[4U * i + 3U] < origins[4U * i + 3U]) {
      return invalid(error, "ray near limit exceeds far limit");
    }
  }
  runtime::HostVisibleBuffer temporary{};
  if (!createHitBuffer(rayCount, temporary, error) || !temporary.map(error)) {
    return false;
  }
  const std::array<VkBuffer, 4U> handles = {origin.handle(), direction.handle(),
                                            triangles.handle(),
                                            temporary.handle()};
  const std::array<VkDeviceSize, 4U> ranges = {
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) * 16U),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) * 16U),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, triangleCount) * 48U),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) *
                                sizeof(TriangleHit))};
  std::array<VkDescriptorBufferInfo, 4U> infos{};
  std::array<VkWriteDescriptorSet, 4U> writes{};
  for (std::uint32_t i = 0U; i < writes.size(); ++i) {
    infos[i] = {handles[i], 0U, ranges[i]};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 descriptorSet_,
                 i,
                 0U,
                 1U,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  std::array<VkBufferMemoryBarrier, 4U> pre{};
  for (std::uint32_t i = 0U; i < pre.size(); ++i) {
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_HOST_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              handles[i],
              0U,
              ranges[i]};
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS) {
    return invalid(error, "failed to begin triangle-hit command buffer");
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const std::array<std::uint32_t, 2U> push = {
      static_cast<std::uint32_t>(rayCount),
      static_cast<std::uint32_t>(triangleCount)};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push),
                     push.data());
  vkCmdDispatch(commandBuffer_,
                static_cast<std::uint32_t>((rayCount + 63U) / 64U), 1U, 1U);
  const VkBufferMemoryBarrier post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                   nullptr,
                                   VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_HOST_READ_BIT,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   temporary.handle(),
                                   0U,
                                   ranges[3]};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U, &post,
                       0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return invalid(error, "failed to end triangle-hit command buffer");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();
  if (!temporary.invalidate(error)) {
    return false;
  }
  const auto *staged = static_cast<const TriangleHit *>(temporary.mappedPtr());
  for (std::size_t i = 0U; i < rayCount; ++i) {
    const auto &hit = staged[i];
    if (hit.isMiss()) {
      if (hit.t != std::numeric_limits<float>::max() || hit.u != 0.0F ||
          hit.v != 0.0F) {
        return invalid(error, "shader produced an invalid miss sentinel");
      }
      continue;
    }
    if (hit.triangleIndex >= triangleCount || !strictFp32(hit.t) ||
        !strictFp32(hit.u) || !strictFp32(hit.v) || hit.t < origins[4U * i] ||
        hit.t > directions[4U * i + 3U] || hit.u < 0.0F || hit.u > 1.0F ||
        hit.v < 0.0F || hit.v > 1.0F || hit.u + hit.v > 1.0F) {
      return invalid(error, "shader produced an invalid triangle hit");
    }
  }
  return hits.write(temporary.mappedPtr(), rayCount * sizeof(TriangleHit), 0U,
                    error);
}

const runtime::VulkanDevice &TriangleHitPrimitive::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

} // namespace viennaps::vulkan::ray
