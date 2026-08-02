// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "triangle_bvh_hit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &e, const char *m) {
  e = m;
  return false;
}
bool finiteFp(const float v) {
  return std::isfinite(v) &&
         (v == 0.0F || std::abs(v) >= std::numeric_limits<float>::min());
}
struct BuildState {
  std::span<const Triangle> tris;
  std::vector<TriangleBvhNode> nodes;
  std::vector<std::uint32_t> order;
};
TriangleBvhNode bounds(const BuildState &s, std::size_t begin,
                       std::size_t end) {
  TriangleBvhNode n{};
  n.minX = n.minY = n.minZ = std::numeric_limits<float>::max();
  n.maxX = n.maxY = n.maxZ = -std::numeric_limits<float>::max();
  for (std::size_t i = begin; i < end; ++i) {
    const auto &t = s.tris[s.order[i]];
    for (const auto &p : {t.a, t.b, t.c}) {
      n.minX = std::min(n.minX, p[0]);
      n.minY = std::min(n.minY, p[1]);
      n.minZ = std::min(n.minZ, p[2]);
      n.maxX = std::max(n.maxX, p[0]);
      n.maxY = std::max(n.maxY, p[1]);
      n.maxZ = std::max(n.maxZ, p[2]);
    }
  }
  n.minX = std::nextafter(n.minX, -std::numeric_limits<float>::infinity());
  n.minY = std::nextafter(n.minY, -std::numeric_limits<float>::infinity());
  n.minZ = std::nextafter(n.minZ, -std::numeric_limits<float>::infinity());
  n.maxX = std::nextafter(n.maxX, std::numeric_limits<float>::infinity());
  n.maxY = std::nextafter(n.maxY, std::numeric_limits<float>::infinity());
  n.maxZ = std::nextafter(n.maxZ, std::numeric_limits<float>::infinity());
  return n;
}
void makeNode(BuildState &s, const std::uint32_t nodeIndex, std::size_t begin,
              std::size_t end) {
  auto n = bounds(s, begin, end);
  const auto count = end - begin;
  if (count <= 4U) {
    n.leftFirst = static_cast<std::uint32_t>(begin);
    n.count = static_cast<std::uint32_t>(count);
    s.nodes[nodeIndex] = n;
    return;
  }
  const float ex = n.maxX - n.minX, ey = n.maxY - n.minY, ez = n.maxZ - n.minZ;
  int axis = ex >= ey && ex >= ez ? 0 : (ey >= ez ? 1 : 2);
  const auto mid = begin + count / 2U;
  std::nth_element(s.order.begin() + static_cast<std::ptrdiff_t>(begin),
                   s.order.begin() + static_cast<std::ptrdiff_t>(mid),
                   s.order.begin() + static_cast<std::ptrdiff_t>(end),
                   [&](const auto a, const auto b) {
                     const auto centroid = [&](const auto &t) {
                       return (t.a[axis] + t.b[axis] + t.c[axis]) / 3.0F;
                     };
                     const auto ca = centroid(s.tris[a]),
                                cb = centroid(s.tris[b]);
                     return ca < cb || (ca == cb && a < b);
                   });
  n.leftFirst = static_cast<std::uint32_t>(s.nodes.size());
  n.count = 0U;
  s.nodes[nodeIndex] = n;
  s.nodes.resize(s.nodes.size() + 2U);
  makeNode(s, n.leftFirst, begin, mid);
  makeNode(s, n.leftFirst + 1U, mid, end);
}
bool validTriangles(std::span<const Triangle> ts, std::string &e) {
  for (const auto &t : ts)
    for (const auto p : {t.a, t.b, t.c})
      for (const auto v : p)
        if (!finiteFp(v))
          return fail(e, "triangle vertex contains a non-normal FP32 value");
  return true;
}
} // namespace

TriangleBvhHitPrimitive::~TriangleBvhHitPrimitive() { reset(); }

bool TriangleBvhHitPrimitive::setup(const std::string_view path,
                                    runtime::ComputeSession *external,
                                    std::string &error) {
  if (path.empty())
    return fail(error, "triangle BVH SPIR-V path is empty");
  reset();
  if (external) {
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
  std::string refitPath(path);
  const auto hitName = refitPath.find("triangle_bvh_hit");
  if (hitName != std::string::npos)
    refitPath.replace(hitName, std::string("triangle_bvh_hit").size(),
                      "triangle_bvh_refit");
  runtime::SpirvProgram refitProgram{};
  if (hitName != std::string::npos &&
      (!runtime::readSpirv(refitPath, refitProgram, error) ||
       !refitShaderModule_.create(session_->device(), refitProgram, error))) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 6U> bs{};
  for (std::uint32_t i = 0; i < bs.size(); ++i)
    bs[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 4U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bs, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1),
                              error) ||
      !descriptorPool_.create(session_->device(), 1U, 6U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error) ||
      !pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        runtime::ComputePipelineOptions{}, error)) {
    reset();
    return false;
  }
  if (!refitProgram.words.empty()) {
    std::array<VkDescriptorSetLayoutBinding, 4U> refitBindings{};
    for (std::uint32_t i = 0; i < refitBindings.size(); ++i)
      refitBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                          VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    if (!refitDescriptorSetLayout_.create(session_->device(), refitBindings,
                                          error) ||
        !refitPipelineLayout_.create(
            session_->device(), refitDescriptorSetLayout_.get(),
            std::span<const VkPushConstantRange>(&push, 1), error) ||
        !refitDescriptorPool_.create(session_->device(), 1U, 4U,
                                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     error) ||
        !refitDescriptorPool_.allocate(refitDescriptorSetLayout_.get(),
                                       refitDescriptorSet_, error) ||
        !refitPipeline_.create(session_->device(), refitShaderModule_,
                               refitPipelineLayout_,
                               runtime::ComputePipelineOptions{}, error)) {
      reset();
      return false;
    }
  }
  return true;
}
bool TriangleBvhHitPrimitive::initialize(std::string_view p, std::string &e) {
  e.clear();
  return setup(p, nullptr, e);
}
bool TriangleBvhHitPrimitive::initialize(runtime::ComputeSession &s,
                                         std::string_view p, std::string &e) {
  e.clear();
  return setup(p, &s, e);
}
void TriangleBvhHitPrimitive::reset() {
  nodes_.reset();
  triangles_.reset();
  indices_.reset();
  refitNodeIds_.reset();
  refitNodeIdsHost_.clear();
  refitRanges_.clear();
  fence_.destroy();
  pipeline_.reset();
  refitPipeline_.reset();
  descriptorPool_.reset();
  refitDescriptorPool_.reset();
  pipelineLayout_.reset();
  refitPipelineLayout_.reset();
  descriptorSetLayout_.reset();
  refitDescriptorSetLayout_.reset();
  shaderModule_.reset();
  refitShaderModule_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  refitDescriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
  triangleCount_ = 0U;
  built_ = false;
  const bool own = session_ == &ownedSession_;
  session_ = nullptr;
  if (own)
    ownedSession_.reset();
}
bool TriangleBvhHitPrimitive::isInitialized() const {
  return session_ && session_->isValid() && pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}
bool TriangleBvhHitPrimitive::ready(std::string &e) const {
  return isInitialized() ? true
                         : fail(e, "triangle BVH primitive is not initialized");
}
bool TriangleBvhHitPrimitive::createBytes(std::size_t bytes,
                                          runtime::DeviceBuffer &b,
                                          std::string &e) const {
  if (!ready(e))
    return false;
  return b.create(*session_, static_cast<VkDeviceSize>(bytes ? bytes : 1U), e);
}

bool TriangleBvhHitPrimitive::build(std::span<const Triangle> ts,
                                    std::string &e) {
  e.clear();
  if (!ready(e) || !validTriangles(ts, e))
    return false;
  if (ts.size() > std::numeric_limits<std::uint32_t>::max() ||
      ts.size() > (std::numeric_limits<std::size_t>::max() / 3U) ||
      ts.size() * 3U > std::numeric_limits<std::uint32_t>::max())
    return fail(e, "triangle BVH count exceeds uint32/index range");
  nodes_.reset();
  triangles_.reset();
  indices_.reset();
  refitNodeIds_.reset();
  refitNodeIdsHost_.clear();
  refitRanges_.clear();
  triangleCount_ = 0U;
  built_ = false;
  BuildState s{ts, {}, {}};
  s.order.resize(ts.size());
  std::iota(s.order.begin(), s.order.end(), 0U);
  s.nodes.push_back({});
  if (!ts.empty())
    makeNode(s, 0U, 0U, ts.size());
  if (!ts.empty()) {
    std::vector<std::vector<std::uint32_t>> levels;
    std::function<void(std::uint32_t, std::uint32_t)> collect =
        [&](const std::uint32_t node, const std::uint32_t depth) {
          if (s.nodes[node].count != 0U)
            return;
          if (levels.size() <= depth)
            levels.resize(depth + 1U);
          levels[depth].push_back(node);
          collect(s.nodes[node].leftFirst, depth + 1U);
          collect(s.nodes[node].leftFirst + 1U, depth + 1U);
        };
    std::vector<std::uint32_t> leaves;
    std::function<void(std::uint32_t)> collectLeaves =
        [&](const std::uint32_t node) {
          if (s.nodes[node].count != 0U) {
            leaves.push_back(node);
            return;
          }
          collectLeaves(s.nodes[node].leftFirst);
          collectLeaves(s.nodes[node].leftFirst + 1U);
        };
    collect(0U, 0U);
    collectLeaves(0U);
    refitNodeIdsHost_ = leaves;
    refitRanges_.push_back({0U, static_cast<std::uint32_t>(leaves.size())});
    for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
      const auto offset = static_cast<std::uint32_t>(refitNodeIdsHost_.size());
      refitNodeIdsHost_.insert(refitNodeIdsHost_.end(), level->begin(),
                               level->end());
      refitRanges_.push_back(
          {offset, static_cast<std::uint32_t>(level->size())});
    }
  }
  std::vector<std::array<float, 4>> packed;
  packed.reserve(ts.size() * 3U);
  for (const auto &t : ts) {
    packed.push_back({t.a[0], t.a[1], t.a[2], 0});
    packed.push_back({t.b[0], t.b[1], t.b[2], 0});
    packed.push_back({t.c[0], t.c[1], t.c[2], 0});
  }
  if (!createBytes(s.nodes.size() * sizeof(TriangleBvhNode), nodes_, e) ||
      !createBytes(std::max<std::size_t>(1U, packed.size()) * sizeof(packed[0]),
                   triangles_, e) ||
      !createBytes(std::max<std::size_t>(1U, s.order.size()) *
                       sizeof(std::uint32_t),
                   indices_, e) ||
      !createBytes(std::max<std::size_t>(1U, refitNodeIdsHost_.size()) *
                       sizeof(std::uint32_t),
                   refitNodeIds_, e))
    return false;
  if (!s.nodes.empty() &&
      !nodes_.upload(*session_, s.nodes.data(),
                     s.nodes.size() * sizeof(TriangleBvhNode), 0U, e))
    return false;
  if (!packed.empty() &&
      !triangles_.upload(*session_, packed.data(),
                         packed.size() * sizeof(packed[0]), 0U, e))
    return false;
  if (!s.order.empty() &&
      !indices_.upload(*session_, s.order.data(),
                       s.order.size() * sizeof(std::uint32_t), 0U, e))
    return false;
  if (!refitNodeIdsHost_.empty() &&
      !refitNodeIds_.upload(*session_, refitNodeIdsHost_.data(),
                            refitNodeIdsHost_.size() * sizeof(std::uint32_t),
                            0U, e))
    return false;
  triangleCount_ = ts.size();
  built_ = true;
  return true;
}

bool TriangleBvhHitPrimitive::recordRefit(
    const VkCommandBuffer commandBuffer, runtime::DeviceBuffer &dynamicVertices,
    const std::span<const Triangle> hostMirror, std::string &e) {
  e.clear();
  if (!ready(e) || !built_ || refitPipeline_.get() == VK_NULL_HANDLE ||
      refitDescriptorSet_ == VK_NULL_HANDLE)
    return fail(e, "triangle BVH refit is not initialized");
  if (commandBuffer == VK_NULL_HANDLE || hostMirror.size() != triangleCount_)
    return fail(e, "triangle BVH refit command or topology count is invalid");
  constexpr std::size_t packedVertexBytes = 3U * sizeof(float) * 4U;
  if (triangleCount_ >
      std::numeric_limits<std::size_t>::max() / packedVertexBytes)
    return fail(e, "triangle BVH refit vertex size overflows size_t");
  const std::size_t vertexBytes = triangleCount_ * packedVertexBytes;
  if (!dynamicVertices.isValid() || dynamicVertices.ownerDevice() !=
                                      session_->device().get() ||
      dynamicVertices.ownerSessionGeneration() != session_->generation() ||
      dynamicVertices.size() < (vertexBytes == 0U ? 1U : vertexBytes) ||
      dynamicVertices.handle() == triangles_.handle() ||
      dynamicVertices.handle() == nodes_.handle() ||
      dynamicVertices.handle() == indices_.handle() ||
      dynamicVertices.handle() == refitNodeIds_.handle())
    return fail(e, "triangle BVH refit vertex buffer is stale or invalid");
  const std::array<runtime::DeviceBuffer *, 4U> internalBuffers = {
      &nodes_, &triangles_, &indices_, &refitNodeIds_};
  for (const auto *buffer : internalBuffers)
    if (!buffer->isValid() ||
        buffer->ownerDevice() != session_->device().get() ||
        buffer->ownerSessionGeneration() != session_->generation())
      return fail(e, "triangle BVH refit geometry buffers are stale");
  for (const auto &triangle : hostMirror)
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      for (const float value : point)
        if (!finiteFp(value))
          return fail(e, "triangle BVH refit vertex is outside FP32 domain");
  if (triangleCount_ == 0U)
    return true;

  const VkBufferMemoryBarrier trianglesBeforeCopy{
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      nullptr,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      triangles_.handle(),
      0U,
      VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 1U,
                       &trianglesBeforeCopy, 0U, nullptr);
  const VkBufferCopy copy{0U, 0U, static_cast<VkDeviceSize>(vertexBytes)};
  vkCmdCopyBuffer(commandBuffer, dynamicVertices.handle(), triangles_.handle(),
                  1U, &copy);
  std::array<VkBufferMemoryBarrier, 4U> pre{};
  pre[0] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            triangles_.handle(),
            0U,
            VK_WHOLE_SIZE};
  pre[1] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            dynamicVertices.handle(),
            0U,
            VK_WHOLE_SIZE};
  pre[2] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            nodes_.handle(),
            0U,
            VK_WHOLE_SIZE};
  pre[3] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            refitNodeIds_.handle(),
            0U,
            VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT |
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);

  const std::array<VkBuffer, 4U> handles = {
      triangles_.handle(), nodes_.handle(), indices_.handle(),
      refitNodeIds_.handle()};
  std::array<VkDescriptorBufferInfo, 4U> infos{};
  std::array<VkWriteDescriptorSet, 4U> writes{};
  for (std::uint32_t i = 0U; i < writes.size(); ++i) {
    infos[i] = {handles[i], 0U, VK_WHOLE_SIZE};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 refitDescriptorSet_,
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
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    refitPipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          refitPipelineLayout_.get(), 0U, 1U,
                          &refitDescriptorSet_, 0U, nullptr);
  for (std::size_t rangeIndex = 0U; rangeIndex < refitRanges_.size();
       ++rangeIndex) {
    const auto [offset, count] = refitRanges_[rangeIndex];
    if (count == 0U)
      continue;
    const std::array<std::uint32_t, 4U> pc = {
        rangeIndex == 0U ? 0U : 1U, offset, count,
        static_cast<std::uint32_t>(triangleCount_)};
    vkCmdPushConstants(commandBuffer, refitPipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(pc), pc.data());
    vkCmdDispatch(commandBuffer, (count + 63U) / 64U, 1U, 1U);
    const VkBufferMemoryBarrier nodesBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        nodes_.handle(),
        0U,
        VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U,
                         nullptr, 1U, &nodesBarrier, 0U, nullptr);
  }
  return true;
}

bool TriangleBvhHitPrimitive::snapshotNodes(
    std::vector<TriangleBvhNode> &output, std::string &e) const {
  e.clear();
  if (!ready(e) || !built_)
    return fail(e, "triangle BVH geometry is not built");
  const std::size_t count = nodes_.size() / sizeof(TriangleBvhNode);
  output.resize(count);
  if (count == 0U)
    return true;
  return nodes_.download(*session_, output.data(),
                         count * sizeof(TriangleBvhNode), 0U, e);
}

bool TriangleBvhHitPrimitive::intersect(std::span<const Ray> rays,
                                        std::span<TriangleHit> out,
                                        std::string &e) {
  e.clear();
  if (!ready(e) || !built_)
    return fail(e, "triangle BVH geometry is not built");
  if (out.size() < rays.size())
    return fail(e, "triangle BVH output capacity is insufficient");
  for (const auto &r : rays) {
    for (auto v : r.origin)
      if (!finiteFp(v))
        return fail(e, "ray origin contains a non-normal FP32 value");
    for (auto v : r.direction)
      if (!finiteFp(v))
        return fail(e, "ray direction contains a non-normal FP32 value");
    if (!finiteFp(r.tMin) || !finiteFp(r.tMax) || r.tMin < 0 || r.tMax < r.tMin)
      return fail(e, "ray near/far limits are outside the FP32 domain");
  }
  if (rays.empty())
    return true;
  std::vector<std::array<float, 4>> o, d;
  o.reserve(rays.size());
  d.reserve(rays.size());
  for (const auto &r : rays) {
    o.push_back({r.origin[0], r.origin[1], r.origin[2], r.tMin});
    d.push_back({r.direction[0], r.direction[1], r.direction[2], r.tMax});
  }
  runtime::DeviceBuffer ob, db, hb;
  if (!createBytes(o.size() * 16U, ob, e) ||
      !createBytes(d.size() * 16U, db, e) ||
      !createBytes(rays.size() * sizeof(TriangleHit), hb, e) ||
      !ob.upload(*session_, o.data(), o.size() * 16U, 0, e) ||
      !db.upload(*session_, d.data(), d.size() * 16U, 0, e))
    return false;
  if (vkResetCommandBuffer(commandBuffer_, 0) != VK_SUCCESS)
    return fail(e, "failed to reset triangle BVH command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(e, "failed to begin triangle BVH command buffer");
  std::array<VkBuffer, 6> handles{ob.handle(),       db.handle(),
                                  nodes_.handle(),   triangles_.handle(),
                                  indices_.handle(), hb.handle()};
  std::array<VkDescriptorBufferInfo, 6> infos{};
  std::array<VkWriteDescriptorSet, 6> writes{};
  for (std::uint32_t i = 0; i < 6; ++i) {
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
  vkUpdateDescriptorSets(session_->device().get(), 6, writes.data(), 0,
                         nullptr);
  std::array<VkBufferMemoryBarrier, 5> pre{};
  for (std::size_t i = 0; i < 5; ++i)
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              handles[i],
              0,
              VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 5,
                       pre.data(), 0, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0, 1, &descriptorSet_, 0,
                          nullptr);
  const std::array<std::uint32_t, 4> pc{
      static_cast<std::uint32_t>(rays.size()),
      static_cast<std::uint32_t>(nodes_.size() / sizeof(TriangleBvhNode)),
      static_cast<std::uint32_t>(triangleCount_),
      static_cast<std::uint32_t>(indices_.size() / sizeof(std::uint32_t))};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc.data());
  vkCmdDispatch(commandBuffer_,
                static_cast<std::uint32_t>((rays.size() + 63U) / 64U), 1, 1);
  const VkBufferMemoryBarrier post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                   nullptr,
                                   VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   hb.handle(),
                                   0,
                                   VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &post,
                       0, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(e, "failed to end triangle BVH command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, e))
    return false;
  fence_.reset();
  std::vector<TriangleHit> staged(rays.size());
  if (!hb.download(*session_, staged.data(),
                   staged.size() * sizeof(TriangleHit), 0, e))
    return false;
  std::copy(staged.begin(), staged.end(), out.begin());
  return true;
}
bool TriangleBvhHitPrimitive::recordDispatch(
    const VkCommandBuffer commandBuffer, runtime::DeviceBuffer &origins,
    runtime::DeviceBuffer &directions, const std::size_t rayCount,
    runtime::DeviceBuffer &hits, const std::size_t outputCapacity,
    std::string &e) {
  e.clear();
  if (!ready(e) || !built_)
    return fail(e, "triangle BVH geometry is not built");
  if (commandBuffer == VK_NULL_HANDLE ||
      rayCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < rayCount || rayCount > origins.size() / 16U ||
      rayCount > directions.size() / 16U ||
      outputCapacity > hits.size() / sizeof(TriangleHit))
    return fail(e, "triangle BVH record buffer capacity is invalid");
  const std::array<runtime::DeviceBuffer *, 6U> buffers = {
      &origins, &directions, &nodes_, &triangles_, &indices_, &hits};
  for (std::size_t i = 0; i < buffers.size(); ++i) {
    if (!buffers[i]->isValid() ||
        buffers[i]->ownerDevice() != session_->device().get() ||
        buffers[i]->ownerSessionGeneration() != session_->generation())
      return fail(e, "triangle BVH record buffer has wrong device or session");
    for (std::size_t j = i + 1U; j < buffers.size(); ++j)
      if (buffers[i]->handle() == buffers[j]->handle())
        return fail(e, "triangle BVH record buffers must not alias");
  }
  if (rayCount == 0U)
    return true;
  const std::array<VkBuffer, 6U> handles = {
      origins.handle(),    directions.handle(), nodes_.handle(),
      triangles_.handle(), indices_.handle(),   hits.handle()};
  std::array<VkDescriptorBufferInfo, 6U> infos{};
  std::array<VkWriteDescriptorSet, 6U> writes{};
  for (std::uint32_t i = 0; i < writes.size(); ++i) {
    infos[i] = {handles[i], 0U, VK_WHOLE_SIZE};
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
  std::array<VkBufferMemoryBarrier, 5U> pre{};
  for (std::size_t i = 0; i < pre.size(); ++i)
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              handles[i],
              0U,
              VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const std::array<std::uint32_t, 4U> pc = {
      static_cast<std::uint32_t>(rayCount),
      static_cast<std::uint32_t>(nodes_.size() / sizeof(TriangleBvhNode)),
      static_cast<std::uint32_t>(triangleCount_),
      static_cast<std::uint32_t>(indices_.size() / sizeof(std::uint32_t))};
  vkCmdPushConstants(commandBuffer, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(pc), pc.data());
  vkCmdDispatch(commandBuffer,
                static_cast<std::uint32_t>((rayCount + 63U) / 64U), 1U, 1U);
  const VkBufferMemoryBarrier post = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                      nullptr,
                                      VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      hits.handle(),
                                      0U,
                                      VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       1U, &post, 0U, nullptr);
  return true;
}

const runtime::VulkanDevice &TriangleBvhHitPrimitive::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ ? session_->device() : empty;
}
} // namespace viennaps::vulkan::ray
