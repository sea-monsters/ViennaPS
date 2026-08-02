// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "triangle_hit_device.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &e, const char *m) {
  e = m;
  return false;
}
bool mul(std::size_t a, std::size_t b, std::size_t &out) {
  if (b != 0U && a > std::numeric_limits<std::size_t>::max() / b)
    return false;
  out = a * b;
  return true;
}
bool strictFp32(const float v) {
  return std::isfinite(v) &&
         (v == 0.0F || std::abs(v) >= std::numeric_limits<float>::min());
}
} // namespace

DeviceTriangleHitPrimitive::~DeviceTriangleHitPrimitive() { reset(); }

bool DeviceTriangleHitPrimitive::setup(const std::string_view path,
                                       runtime::ComputeSession *external,
                                       std::string &error) {
  if (path.empty())
    return fail(error, "triangle-hit device SPIR-V path is empty");
  reset();
  if (external != nullptr) {
    if (!external->isValid())
      return fail(error, "external compute session is not initialized");
    session_ = external;
  } else {
    if (!ownedSession_.initialize(error))
      return false;
    session_ = &ownedSession_;
  }
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(path, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 4U> bindings{};
  for (std::uint32_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 2U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1),
                              error) ||
      !descriptorPool_.create(session_->device(), 1U, 4U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    reset();
    return false;
  }
  if (!pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        runtime::ComputePipelineOptions{}, error)) {
    reset();
    return false;
  }
  return true;
}

bool DeviceTriangleHitPrimitive::initialize(const std::string_view path,
                                            std::string &error) {
  error.clear();
  return setup(path, nullptr, error);
}
bool DeviceTriangleHitPrimitive::initialize(runtime::ComputeSession &session,
                                            const std::string_view path,
                                            std::string &error) {
  error.clear();
  return setup(path, &session, error);
}
void DeviceTriangleHitPrimitive::reset() {
  const bool own = session_ == &ownedSession_;
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
  session_ = nullptr;
  if (own)
    ownedSession_.reset();
}
bool DeviceTriangleHitPrimitive::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorPool_.get() != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE &&
         commandBuffer_ != VK_NULL_HANDLE;
}
bool DeviceTriangleHitPrimitive::ready(std::string &error) const {
  return isInitialized()
             ? true
             : fail(error, "triangle-hit device primitive is not initialized");
}
bool DeviceTriangleHitPrimitive::createBytes(const std::size_t bytes,
                                             runtime::DeviceBuffer &buffer,
                                             std::string &error) const {
  if (!ready(error))
    return false;
  return buffer.create(
      *session_, static_cast<VkDeviceSize>(bytes == 0U ? 1U : bytes), error);
}
bool DeviceTriangleHitPrimitive::createRayBuffers(const std::size_t count,
                                                  runtime::DeviceBuffer &o,
                                                  runtime::DeviceBuffer &d,
                                                  std::string &e) const {
  std::size_t bytes{};
  if (!mul(count, 4U * sizeof(float), bytes))
    return fail(e, "ray buffer size overflow");
  return createBytes(bytes, o, e) && createBytes(bytes, d, e);
}
bool DeviceTriangleHitPrimitive::createTriangleBuffer(const std::size_t count,
                                                      runtime::DeviceBuffer &b,
                                                      std::string &e) const {
  std::size_t n{};
  if (!mul(count, 3U, n) || !mul(n, 4U * sizeof(float), n))
    return fail(e, "triangle buffer size overflow");
  return createBytes(n, b, e);
}
bool DeviceTriangleHitPrimitive::createHitBuffer(const std::size_t count,
                                                 runtime::DeviceBuffer &b,
                                                 std::string &e) const {
  std::size_t bytes{};
  if (!mul(count, sizeof(TriangleHit), bytes))
    return fail(e, "hit buffer size overflow");
  return createBytes(bytes, b, e);
}

bool DeviceTriangleHitPrimitive::uploadRays(const std::span<const Ray> rays,
                                            runtime::DeviceBuffer &o,
                                            runtime::DeviceBuffer &d,
                                            std::string &e) const {
  if (!ready(e))
    return false;
  std::vector<std::array<float, 4>> po, pd;
  po.reserve(rays.size());
  pd.reserve(rays.size());
  for (const auto &r : rays) {
    for (const auto v : r.origin)
      if (!strictFp32(v))
        return fail(e, "ray origin contains a non-normal FP32 value");
    for (const auto v : r.direction)
      if (!strictFp32(v))
        return fail(e, "ray direction contains a non-normal FP32 value");
    if (!strictFp32(r.tMin) || !strictFp32(r.tMax) || r.tMin < 0.0F ||
        r.tMax < r.tMin)
      return fail(e, "ray near/far limits are outside the FP32 domain");
    po.push_back({r.origin[0], r.origin[1], r.origin[2], r.tMin});
    pd.push_back({r.direction[0], r.direction[1], r.direction[2], r.tMax});
  }
  const auto bytes = po.size() * sizeof(po[0]);
  return o.upload(*session_, po.data(), bytes, 0U, e) &&
         d.upload(*session_, pd.data(), bytes, 0U, e);
}
bool DeviceTriangleHitPrimitive::uploadTriangles(
    const std::span<const Triangle> ts, runtime::DeviceBuffer &b,
    std::string &e) const {
  if (!ready(e))
    return false;
  std::vector<std::array<float, 4>> v;
  v.reserve(ts.size() * 3U);
  for (const auto &t : ts) {
    for (const auto vertex : {t.a, t.b, t.c})
      for (const auto x : vertex)
        if (!strictFp32(x))
          return fail(e, "triangle vertex contains a non-normal FP32 value");
    v.push_back({t.a[0], t.a[1], t.a[2], 0});
    v.push_back({t.b[0], t.b[1], t.b[2], 0});
    v.push_back({t.c[0], t.c[1], t.c[2], 0});
  }
  return b.upload(*session_, v.data(), v.size() * sizeof(v[0]), 0U, e);
}
bool DeviceTriangleHitPrimitive::downloadHits(const std::size_t count,
                                              const runtime::DeviceBuffer &b,
                                              const std::span<TriangleHit> out,
                                              std::string &e) const {
  if (!ready(e) || out.size() < count || count > b.size() / sizeof(TriangleHit))
    return fail(e, "hit download capacity is invalid");
  return b.download(*session_, out.data(), count * sizeof(TriangleHit), 0U, e);
}

bool DeviceTriangleHitPrimitive::dispatch(
    runtime::DeviceBuffer &o, runtime::DeviceBuffer &d,
    runtime::DeviceBuffer &t, const std::size_t rays, const std::size_t tris,
    runtime::DeviceBuffer &h, const std::size_t capacity, std::string &e) {
  e.clear();
  if (!ready(e))
    return false;
  if (rays > std::numeric_limits<std::uint32_t>::max() ||
      tris > std::numeric_limits<std::uint32_t>::max() || capacity < rays ||
      rays > o.size() / 16U || rays > d.size() / 16U || tris > t.size() / 48U ||
      capacity > h.size() / sizeof(TriangleHit))
    return fail(
        e, "triangle-hit device buffer capacity or index range is invalid");
  const std::array<runtime::DeviceBuffer *, 4> bs{&o, &d, &t, &h};
  for (std::size_t i = 0; i < bs.size(); ++i) {
    if (!bs[i]->isValid() || bs[i]->ownerDevice() != session_->device().get() ||
        bs[i]->ownerSessionGeneration() != session_->generation())
      return fail(e,
                  "triangle-hit buffer has wrong device, session, or validity");
    for (std::size_t j = i + 1; j < bs.size(); ++j)
      if (bs[i]->handle() == bs[j]->handle())
        return fail(e, "triangle-hit buffers must not alias");
  }
  if (rays == 0U)
    return true;
  if (vkResetCommandBuffer(commandBuffer_, 0) != VK_SUCCESS)
    return fail(e, "failed to reset triangle-hit command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(e, "failed to begin triangle-hit command buffer");
  if (!recordDispatch(commandBuffer_, o, d, t, rays, tris, h, capacity, e))
    return false;
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(e, "failed to end triangle-hit command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, e))
    return false;
  fence_.reset();
  return true;
}

bool DeviceTriangleHitPrimitive::recordDispatch(
    const VkCommandBuffer commandBuffer, runtime::DeviceBuffer &o,
    runtime::DeviceBuffer &d, runtime::DeviceBuffer &t, const std::size_t rays,
    const std::size_t tris, runtime::DeviceBuffer &h,
    const std::size_t capacity, std::string &e) {
  e.clear();
  if (!ready(e))
    return false;
  if (commandBuffer == VK_NULL_HANDLE)
    return fail(e, "triangle-hit command buffer is invalid");
  if (rays > std::numeric_limits<std::uint32_t>::max() ||
      tris > std::numeric_limits<std::uint32_t>::max() || capacity < rays ||
      rays > o.size() / 16U || rays > d.size() / 16U || tris > t.size() / 48U ||
      capacity > h.size() / sizeof(TriangleHit))
    return fail(
        e, "triangle-hit device buffer capacity or index range is invalid");
  const std::array<runtime::DeviceBuffer *, 4> bs{&o, &d, &t, &h};
  for (std::size_t i = 0; i < bs.size(); ++i) {
    if (!bs[i]->isValid() || bs[i]->ownerDevice() != session_->device().get() ||
        bs[i]->ownerSessionGeneration() != session_->generation())
      return fail(e,
                  "triangle-hit buffer has wrong device, session, or validity");
    for (std::size_t j = i + 1; j < bs.size(); ++j)
      if (bs[i]->handle() == bs[j]->handle())
        return fail(e, "triangle-hit buffers must not alias");
  }
  if (rays == 0U)
    return true;
  const std::array<VkBuffer, 4> handles{o.handle(), d.handle(), t.handle(),
                                        h.handle()};
  std::array<VkDescriptorBufferInfo, 4> infos{};
  std::array<VkWriteDescriptorSet, 4> writes{};
  for (std::uint32_t i = 0; i < 4; ++i) {
    infos[i] = {handles[i], 0, VK_WHOLE_SIZE};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 descriptorSet_,
                 i,
                 0,
                 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(), 4, writes.data(), 0,
                         nullptr);
  std::array<VkBufferMemoryBarrier, 4> pre{};
  for (std::size_t i = 0; i < 4; ++i)
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              handles[i],
              0,
              VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 4,
                       pre.data(), 0, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0, 1, &descriptorSet_, 0,
                          nullptr);
  const std::array<std::uint32_t, 2> pc{static_cast<std::uint32_t>(rays),
                                        static_cast<std::uint32_t>(tris)};
  vkCmdPushConstants(commandBuffer, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc.data());
  vkCmdDispatch(commandBuffer, static_cast<std::uint32_t>((rays + 63U) / 64U),
                1, 1);
  const VkBufferMemoryBarrier post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                   nullptr,
                                   VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT |
                                       VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   h.handle(),
                                   0,
                                   VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                       &post, 0, nullptr);
  return true;
}
const runtime::VulkanDevice &DeviceTriangleHitPrimitive::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ ? session_->device() : empty;
}
} // namespace viennaps::vulkan::ray
