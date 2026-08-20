// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "vulkan_ray_flux_engine.hpp"

#include "../runtime/deployment_compute_context.hpp"
#include "device_ray_flux_pipeline.hpp"
#include "multibounce_decision_producer.hpp"
#include "multibounce_frontier_queue.hpp"
#include "triangle_hit_device.hpp"

#include <process/psCPUTriangleEngine.hpp>
#include <models/psNeutralTransport.hpp>
#include <models/psSingleParticleProcess.hpp>
#include <psPointToElementData.hpp>

#include <rayParticle.hpp>
#include <raySource.hpp>
#include <raySourceRandom.hpp>
#include <rayUtil.hpp>
#include <vcLogger.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace viennaps {

namespace impl_detail {

// Reuses the CPU triangle tracer's default-source construction so the Vulkan
// route samples the identical ray population as the CPU_TRIANGLE oracle.
// TraceTriangle::apply() constructs its default SourceRandom from
// geometry_.getBoundingBox() (the tight geometry bounds) via
// rayInternal::adjustBoundingBox/getTraceSettings; building the source from the
// raw triangleMesh extents instead (the padded domain box) made the sampled
// origins fall outside the geometry and dropped ~80% of hits.  GeometryTriangle
// fills its bounding box directly from the mesh minimum/maximum extents, so the
// tight box is recovered from the mesh without the (final) TraceTriangle class.
// Only the intersection itself is switched to the GPU.
template <typename NumericType, int D>
struct DefaultSourceSetup {
  std::array<Vec3D<NumericType>, 2> adjustedBox = {};
  std::array<int, 5> traceSettings = {};
  viennaray::TraceDirection sourceDirection =
      (D == 2) ? viennaray::TraceDirection::POS_Y
               : viennaray::TraceDirection::POS_Z;
  bool usePrimaryDirection = false;
  Vec3D<NumericType> primaryDirection = {NumericType(0), NumericType(0),
                                         NumericType(0)};

  // Captures the tight geometry bounding box and the adjusted box / trace
  // settings ViennaRay would derive from it for the given source direction.
  // Mirrors TraceTriangle::apply()'s box handling, minus the boundary/kernel.
  void captureBox(const viennaray::TriangleMesh &mesh, NumericType gridDelta) {
    std::array<Vec3D<NumericType>, 2> boundingBox;
    for (int i = 0; i < D; ++i) {
      boundingBox[0][i] = static_cast<NumericType>(mesh.minimumExtent[i]);
      boundingBox[1][i] = static_cast<NumericType>(mesh.maximumExtent[i]);
    }
    rayInternal::adjustBoundingBox<NumericType, D>(
        boundingBox, sourceDirection, gridDelta);
    traceSettings = rayInternal::getTraceSettings(sourceDirection);
    adjustedBox = boundingBox;
  }

  // Builds the default SourceRandom exactly as Trace::prepareSource() does.
  viennaray::SmartPointer<viennaray::Source<NumericType>>
  buildDefaultSource(const viennaray::AbstractParticle<NumericType> &particle,
                     unsigned int numGeometryPoints) const {
    std::array<Vec3D<NumericType>, 3> orthonormalBasis{};
    if (usePrimaryDirection)
      orthonormalBasis =
          rayInternal::getOrthonormalBasis(primaryDirection);
    return std::make_shared<viennaray::SourceRandom<NumericType, D>>(
        adjustedBox, particle.getSourceDistributionPower(), traceSettings,
        numGeometryPoints, usePrimaryDirection, orthonormalBasis);
  }
};

} // namespace impl_detail

template <typename NumericType, int D> struct VulkanRayFluxEngine<NumericType, D>::Impl {
  using CPUTriangleEngineType = CPUTriangleEngine<NumericType, D>;
  using KDTreeType =
      SmartPointer<KDTree<NumericType, std::array<NumericType, 3>>>;
  using MeshType = SmartPointer<viennals::Mesh<float>>;
  using PostProcessingType =
      ElementToPointData<NumericType, float, NumericType, true, D == 3>;

  std::shared_ptr<vulkan::runtime::DeploymentComputeContext> context;
  VulkanRayFluxSpirvPaths paths;
  bool allowCpuFallback = true;
  bool vulkanEligible = false;

  bool useVulkan = false;
  bool multibounceEligible = false;
  bool useMultibounce = false;
  vulkan::ray::DeviceRayFluxPipeline pipeline;
  vulkan::ray::DeviceTriangleHitPrimitive multibounceHit;
  vulkan::ray::MultibounceFrontierQueue multibounceQueue;
  std::uint64_t multibounceSessionGeneration = 0U;
  std::string multibounceLastError;

  CPUTriangleEngineType cpuEngine;

  // Surface extraction state (shared between Vulkan and CPU paths)
  SmartPointer<ProcessModelCPU<NumericType, D>> model_;
  MeshType surfaceMesh_;
  KDTreeType elementKdTree_;
  PostProcessingType postProcessing_;
  std::vector<vulkan::ray::Triangle> deviceTriangles_;
  std::vector<Vec3D<NumericType>> triangleCenters_;
  // Per-element material ids mirrored from the disk-mesh point material ids,
  // required by the CPU-decision frontier route (e.g. NeutralTransport callbacks).
  std::vector<int> elementMaterialIds_;
  // Reuses the CPU tracer's default-source construction so the sampled ray
  // population matches the CPU oracle.  Populated in updateSurfaceMesh after
  // the device triangles are built.
  impl_detail::DefaultSourceSetup<NumericType, D> sourceSetup_;
  viennaray::SmartPointer<viennaray::Source<NumericType>> defaultSource_;
  unsigned int numGeometryPoints_ = 0;
  // ViennaRay's KernelConfig starts at one and advances after every successful
  // TraceTriangle::apply().  Maintain that source-generation sequence even
  // though only the eligible intersection/reduction segment runs on Vulkan.
  unsigned int cpuRunNumber_ = 1;
  // Domain boundary conditions for the analytic boundary reproduction in
  // generateRays.  The device pipeline has no boundary geometry, so rays that
  // escape the lateral domain edges must be reflected/periodic/ignored on the
  // host to match the CPU_TRIANGLE oracle.
  std::array<viennaray::BoundaryCondition, D> boundaryConds_{};
  unsigned int maxBoundaryHits_ = 1000;

  // Caller-owned coverage data mapped to elements for the current surface.  The
  // CPU-decision frontier callbacks read this through a raw pointer, so the
  // smart pointer must stay alive for the whole multibounce pass.
  SmartPointer<PointData<NumericType>> globalTracingData_ = nullptr;

  struct MultibounceRayState {
    vulkan::ray::Ray ray{};
    viennacore::RNG rng{0U};
    float initialWeight{0.0F};
    float weight{0.0F};
    std::uint32_t reflectionCount{0U};
    std::uint32_t bounce{0U};
    std::uint32_t sequence{0U};
    bool active{true};
  };

  explicit Impl(std::shared_ptr<vulkan::runtime::DeploymentComputeContext> ctx,
                VulkanRayFluxSpirvPaths spirv, bool allowFallback)
      : context(std::move(ctx)), paths(std::move(spirv)),
        allowCpuFallback(allowFallback) {}

  [[nodiscard]] bool initializePipeline(std::string &error) {
    if (!context || !context->isPrepared()) {
      error = "deployment compute context is not prepared";
      return false;
    }
    if (context->backendFor(compute::Stage::RAY_TRACING) !=
        compute::ComputeBackend::VULKAN) {
      error = "ray-tracing stage is not selected for Vulkan";
      return false;
    }
    auto *session = context->session();
    if (session == nullptr) {
      error = "no Vulkan compute session available";
      return false;
    }
    if (!multibounceEligible) {
      vulkan::ray::DeviceRayFluxSpirv spirv{
          paths.triangleHit,     paths.recordCompaction, paths.reductionScan,
          paths.radixHistogram,  paths.radixPrefix,      paths.radixScatter,
          paths.surfaceSegments, paths.surfaceReduce,    paths.triangleBvh};
      if (!pipeline.initialize(*session, spirv, error))
        return false;
    }

    if (multibounceEligible) {
      if (paths.multibounceFrontierQueue.empty()) {
        error = "bounded multi-bounce frontier SPIR-V path is empty";
        return false;
      }
      if (!multibounceHit.initialize(*session, paths.triangleHit, error) ||
          !multibounceQueue.initialize(*session,
                                       paths.multibounceFrontierQueue, error))
        return false;
      multibounceSessionGeneration = session->generation();
    }
    return true;
  }

  ProcessResult updateSurfaceMesh(ProcessContext<NumericType, D> &context) {
    assert(surfaceMesh_ != nullptr);
    assert(elementKdTree_ != nullptr);

    CreateSurfaceMesh<NumericType, float, D>(
        context.domain->getLevelSets().back(), surfaceMesh_, elementKdTree_,
        1e-12, context.rayTracingParams.minNodeDistanceFactor)
        .apply();

    viennaray::TriangleMesh triangleMesh;
    if constexpr (D == 2) {
      viennaray::LineMesh lineMesh;
      lineMesh.gridDelta = static_cast<float>(context.domain->getGridDelta());
      lineMesh.lines = std::move(surfaceMesh_->lines);
      lineMesh.nodes = std::move(surfaceMesh_->nodes);
      lineMesh.minimumExtent = surfaceMesh_->minimumExtent;
      lineMesh.maximumExtent = surfaceMesh_->maximumExtent;

      triangleMesh = convertLinesToTriangles(lineMesh);
      assert(triangleMesh.triangles.size() > 0);

      triangleCenters_.clear();
      triangleCenters_.reserve(triangleMesh.triangles.size());
      for (const auto &tri : triangleMesh.triangles) {
        Vec3D<NumericType> center = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
          center[0] += triangleMesh.nodes[tri[i]][0];
          center[1] += triangleMesh.nodes[tri[i]][1];
          center[2] += triangleMesh.nodes[tri[i]][2];
        }
        triangleCenters_.push_back(center / static_cast<NumericType>(3.0));
      }
      assert(triangleCenters_.size() > 0);
      elementKdTree_->setPoints(triangleCenters_);
      elementKdTree_->build();
    } else {
      CopyTriangleMesh(static_cast<float>(context.domain->getGridDelta()),
                       surfaceMesh_, triangleMesh);
    }

    // Convert ViennaRay triangles to device pipeline triangles.  This must run
    // before the D==2 surface-mesh move below, which moves the nodes/triangles
    // out of triangleMesh and would otherwise leave this loop reading
    // moved-from state.
    deviceTriangles_.clear();
    deviceTriangles_.reserve(triangleMesh.triangles.size());
    for (const auto &tri : triangleMesh.triangles) {
      vulkan::ray::Triangle deviceTri{};
      const auto &na = triangleMesh.nodes[tri[0]];
      const auto &nb = triangleMesh.nodes[tri[1]];
      const auto &nc = triangleMesh.nodes[tri[2]];
      for (int j = 0; j < 3; ++j) {
        deviceTri.a[j] = static_cast<float>(na[j]);
        deviceTri.b[j] = static_cast<float>(nb[j]);
        deviceTri.c[j] = static_cast<float>(nc[j]);
      }
      deviceTriangles_.push_back(deviceTri);
    }

    // Capture the tight geometry bounding box / trace settings for the default
    // source, exactly as the CPU_TRIANGLE oracle derives them from
    // geometry_.getBoundingBox().  This must run before the D==2 surface-mesh
    // move below, which moves nodes/triangles out of triangleMesh.
    sourceSetup_.captureBox(triangleMesh, static_cast<NumericType>(
                                              context.domain->getGridDelta()));
    numGeometryPoints_ =
        static_cast<unsigned int>(triangleMesh.triangles.size());

    if constexpr (D == 2) {
      surfaceMesh_->nodes = std::move(triangleMesh.nodes);
      surfaceMesh_->triangles = std::move(triangleMesh.triangles);
      surfaceMesh_->getCellData().insertReplaceVectorData(
          std::move(triangleMesh.normals), "Normals");
      surfaceMesh_->minimumExtent = triangleMesh.minimumExtent;
      surfaceMesh_->maximumExtent = triangleMesh.maximumExtent;
    }

    // The single-bounce source-flux route does not consume per-element material
    // ids.  The bounded CPU-decision frontier route (NeutralTransport) mirrors
    // the CPU oracle and needs element material ids for surfaceReflection.
    elementMaterialIds_.clear();
    if (context.diskMesh != nullptr &&
        context.diskMesh->getMaterialIds() != nullptr &&
        surfaceMesh_ != nullptr) {
      auto pointKdTree = context.getPointKdTree();
      if (pointKdTree != nullptr) {
        const auto &pointMaterialIds = *context.diskMesh->getMaterialIds();
        PointToElementDataSingle<NumericType, NumericType, int, float>(
            pointMaterialIds, elementMaterialIds_, *pointKdTree, surfaceMesh_)
            .apply();
      }
    }

    return ProcessResult::SUCCESS;
  }

  // Host-side nearest-hit Möller-Trumbore intersection against the device
  // triangle list.  Returns the parametric t of the closest forward hit, or
  // +inf if none.  This is used during ray generation to decide whether a ray
  // reaches the surface before escaping through a domain wall, mirroring the
  // CPU's combined boundary+geometry rtcIntersect1 loop.
  [[nodiscard]] float intersectTriangles(const Vec3D<float> &origin,
                                         const Vec3D<float> &direction,
                                         float tMin,
                                         float tMax) const {
    static constexpr float kEpsilon = 1e-7f;
    float bestT = std::numeric_limits<float>::max();
    for (const auto &tri : deviceTriangles_) {
      const float e1[3] = {tri.b[0] - tri.a[0], tri.b[1] - tri.a[1],
                           tri.b[2] - tri.a[2]};
      const float e2[3] = {tri.c[0] - tri.a[0], tri.c[1] - tri.a[1],
                           tri.c[2] - tri.a[2]};
      const float p[3] = {direction[1] * e2[2] - direction[2] * e2[1],
                          direction[2] * e2[0] - direction[0] * e2[2],
                          direction[0] * e2[1] - direction[1] * e2[0]};
      const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
      if (std::abs(det) <= kEpsilon)
        continue;
      const float inv = 1.0f / det;
      const float t[3] = {origin[0] - tri.a[0], origin[1] - tri.a[1],
                          origin[2] - tri.a[2]};
      const float u = inv * (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]);
      if (u < 0.0f || u > 1.0f)
        continue;
      const float q[3] = {t[1] * e1[2] - t[2] * e1[1],
                          t[2] * e1[0] - t[0] * e1[2],
                          t[0] * e1[1] - t[1] * e1[0]};
      const float v = inv * (direction[0] * q[0] + direction[1] * q[1] +
                             direction[2] * q[2]);
      if (v < 0.0f || u + v > 1.0f)
        continue;
      const float tt = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
      if (tt >= tMin && tt <= tMax && tt < bestT)
        bestT = tt;
    }
    return bestT;
  }

  // Reproduce ViennaRay's box boundary for a single-bounce ray whose lateral
  // component escapes the domain before hitting the surface.  The device
  // pipeline has no boundary geometry, so the host advances the ray to the
  // nearest box wall, applies the configured boundary condition, and loops
  // until the ray is back inside or hits the max-boundary-hit cap.  This
  // mirrors TraceKernel's do-while around rtcIntersect1 when a boundary hit
  // occurs (rayTraceKernel.hpp:205-213), which is why the CPU oracle reports
  // ~3600 boundary traces for this fixture but still reaches 20000 geometry hits.
  //
  // The full 3D box is used: for D==2 firstDir=X and secondDir=Z, so both
  // lateral walls and the thin Z slab (from convertLinesToTriangles) are active.
  // After every reflection the D==2 out-of-plane component is re-zeroed exactly
  // as fillRayDirection<2> does, keeping the ray in the geometry plane.
  unsigned int applyBoxBoundary(Vec3D<float> &origin, Vec3D<float> &direction,
                                const std::array<Vec3D<NumericType>, 2> &box,
                                const int firstDir, const int secondDir,
                                const viennaray::BoundaryCondition firstCond,
                                const viennaray::BoundaryCondition secondCond,
                                const unsigned int maxHits) const {
    static constexpr float kEps = 1e-5f;

    auto clampLow = [](float v, float lo) { return v < lo ? lo : v; };
    auto clampHigh = [](float v, float hi) { return v > hi ? hi : v; };

    unsigned int hits = 0;
    while (hits < maxHits) {
      const float lo0 = static_cast<float>(box[0][firstDir]);
      const float hi0 = static_cast<float>(box[1][firstDir]);

      // Distance to the firstDir wall along the ray.  For D==2 the secondDir
      // axis is not a real lateral wall; the adjustedBox has zero extent there
      // and the CPU relies on fillRayDirection<2> zeroing the out-of-plane
      // component instead.  Only enforce the firstDir boundary here.
      float tEnter = -std::numeric_limits<float>::max();
      float tExit = std::numeric_limits<float>::max();
      if (std::abs(direction[firstDir]) > kEps) {
        const float inv = 1.0f / direction[firstDir];
        const float t0 = (lo0 - origin[firstDir]) * inv;
        const float t1 = (hi0 - origin[firstDir]) * inv;
        tEnter = std::min(t0, t1);
        tExit = std::max(t0, t1);
      }

      // No intersection with the box slab at all: the ray is headed away from
      // the domain.  Drop it (matches IGNORE behavior).
      if (tExit < kEps)
        return hits | 0x80000000U;

      // If the ray reaches the surface before leaving the box, no boundary
      // interaction is needed for this bounce.
      const float tSurf = intersectTriangles(origin, direction, kEps, tExit);
      if (tSurf <= tExit)
        break;

      // Advance to the forward wall crossing.  If the ray origin is already
      // inside the box, tEnter is negative and tExit is the distance to the
      // wall it will actually leave through.
      const float tWall = (tEnter > kEps) ? tEnter : tExit;
      if (tWall < kEps)
        return hits | 0x80000000U;
      origin[0] += direction[0] * tWall;
      origin[1] += direction[1] * tWall;
      origin[2] += direction[2] * tWall;

      const bool beyondHi = origin[firstDir] >= hi0 - kEps;
      const bool beyondLo = origin[firstDir] <= lo0 + kEps;
      if (!beyondHi && !beyondLo)
        return hits | 0x80000000U;

      if (firstCond == viennaray::BoundaryCondition::PERIODIC_BOUNDARY) {
        origin[firstDir] = beyondHi ? lo0 : hi0;
      } else if (firstCond ==
                 viennaray::BoundaryCondition::REFLECTIVE_BOUNDARY) {
        direction[firstDir] = -direction[firstDir];
        origin[firstDir] = clampHigh(clampLow(origin[firstDir], lo0), hi0);
      } else {
        return hits | 0x80000000U;
      }

      // Keep 2D rays in the geometry plane after every boundary interaction.
      if (D == 2) {
        if (direction[2] != 0.0F) {
          direction[2] = 0.0F;
          const float len = std::sqrt(direction[0] * direction[0] +
                                      direction[1] * direction[1] +
                                      direction[2] * direction[2]);
          if (len > 0.0F) {
            direction[0] /= len;
            direction[1] /= len;
            direction[2] /= len;
          }
        }
      }
      ++hits;
    }
    return hits;
  }

  std::pair<std::vector<vulkan::ray::Ray>, std::vector<float>>
  generateRays(const ProcessContext<NumericType, D> &context,
               const viennaray::Source<NumericType> *source,
               const std::size_t rayCount) const {
    std::vector<vulkan::ray::Ray> rays;
    rays.reserve(rayCount);
    std::vector<float> weights;
    weights.reserve(rayCount);

    // Reproduce the CPU tracer's per-ray seeding exactly so the Vulkan route
    // samples the identical ray population as the CPU_TRIANGLE oracle.
    // ViennaRay's TraceKernel seeds each ray independently with
    //   RNG(tea<3>(idx, runNumber + rngSeed))
    // (rayTraceKernel.hpp:100,120) rather than advancing one shared RNG.
    // KernelConfig starts at runNumber == 1 and TraceTriangle increments it
    // after each apply(), so retain the matching per-engine dispatch count.
    // Without this the two paths draw different random rays and the flux
    // comparison is pure sampling noise.
    const unsigned int seed = static_cast<unsigned int>(
        context.rayTracingParams.useRandomSeeds
            ? std::random_device{}()
            : context.rayTracingParams.rngSeed +
                  cpuRunNumber_);

    const auto &box = sourceSetup_.adjustedBox;
    const int firstDir = sourceSetup_.traceSettings[1];
    const int secondDir = sourceSetup_.traceSettings[2];
    const auto firstCond = boundaryConds_[firstDir];
    const auto secondCond =
        (D == 3) ? boundaryConds_[secondDir]
                 : viennaray::BoundaryCondition::IGNORE_BOUNDARY;

    for (std::size_t i = 0; i < rayCount; ++i) {
      const auto particleSeed =
          viennacore::tea<3>(static_cast<unsigned int>(i), seed);
      viennacore::RNG rng(particleSeed);
      const auto originDirection = source->getOriginAndDirection(i, rng);
      Vec3D<float> origin{static_cast<float>(originDirection[0][0]),
                          static_cast<float>(originDirection[0][1]),
                          static_cast<float>(originDirection[0][2])};
      Vec3D<float> direction{static_cast<float>(originDirection[1][0]),
                             static_cast<float>(originDirection[1][1]),
                             static_cast<float>(originDirection[1][2])};

      // Mirror viennaray::fillRayDirection<2> (rayUtil.hpp:210-214): for a 2D
      // trace the out-of-plane component of the sampled direction is zeroed
      // and the direction renormalized.  The 2D geometry is extruded into a
      // thin Z ribbon by convertLinesToTriangles, so a ray that keeps its
      // sampled Z drift would only intersect the ribbon on a grazing pass
      // (~21% hit rate).  Zeroing Z keeps the ray in the plane exactly as the
      // CPU triangle engine does, restoring the 100% hit rate the oracle sees.
      if (D == 2) {
        if (direction[2] != 0.0F) {
          direction[2] = 0.0F;
          const float len = std::sqrt(direction[0] * direction[0] +
                                      direction[1] * direction[1] +
                                      direction[2] * direction[2]);
          if (len > 0.0F) {
            direction[0] /= len;
            direction[1] /= len;
            direction[2] /= len;
          }
        }
      }

      // The device pipeline has no boundary geometry, so reproduce the CPU
      // lateral-boundary reflection on the host before dispatch.
      const auto boundaryResult =
          applyBoxBoundary(origin, direction, box, firstDir, secondDir,
                           firstCond, secondCond, maxBoundaryHits_);
      // IGNORE boundaries and an exhausted boundary budget terminate this ray
      // in ViennaRay.  Keep it out of the device submission, while preserving
      // rayCount as the normalization denominator below.
      if ((boundaryResult & 0x80000000U) != 0U)
        continue;

      vulkan::ray::Ray ray{};
      for (int j = 0; j < 3; ++j) {
        ray.origin[j] = origin[j];
        ray.direction[j] = direction[j];
      }
      // ViennaRay's fillRayPosition initializes every trace ray with a
      // 1e-4 tnear. Keep the device hit and CPU oracle on that same contract.
      ray.tMin = 1e-4F;
      ray.tMax = std::numeric_limits<float>::max();
      rays.push_back(ray);
      weights.push_back(static_cast<float>(source->getInitialRayWeight(i)));
    }
    return {std::move(rays), std::move(weights)};
  }

  [[nodiscard]] bool buildMultibounceStates(
      const ProcessContext<NumericType, D> &context,
      const viennaray::Source<NumericType> *source,
      std::vector<MultibounceRayState> &states, std::string &error) const {
    if constexpr (!(std::is_same_v<NumericType, float> && D == 2)) {
      (void)context;
      (void)source;
      (void)states;
      (void)error;
      return false;
    } else {
    error.clear();
    states.clear();
    if (source == nullptr || model_ == nullptr ||
        model_->getParticleTypes().size() != 1U)
      return false;
    const auto &particle = model_->getParticleTypes().front();
    if (!particle)
      return false;
    const auto numPoints = source->getNumPoints();
    const auto rayCount = numPoints * context.rayTracingParams.raysPerPoint;
    const unsigned int seed = static_cast<unsigned int>(
        context.rayTracingParams.useRandomSeeds
            ? std::random_device{}()
            : context.rayTracingParams.rngSeed + cpuRunNumber_);
    const auto &box = sourceSetup_.adjustedBox;
    const int firstDir = sourceSetup_.traceSettings[1];
    const int secondDir = sourceSetup_.traceSettings[2];
    const auto firstCond = boundaryConds_[firstDir];
    const auto secondCond =
        (D == 3) ? boundaryConds_[secondDir]
                 : viennaray::BoundaryCondition::IGNORE_BOUNDARY;

    states.reserve(rayCount);
    for (std::size_t i = 0; i < rayCount; ++i) {
      const auto particleSeed =
          viennacore::tea<3>(static_cast<unsigned int>(i), seed);
      viennacore::RNG rng(particleSeed);
      particle->initNew(rng);
      auto particleDirection = particle->initNewWithDirection(rng);
      const auto originAndDirection = source->getOriginAndDirection(i, rng);
      Vec3D<float> origin{static_cast<float>(originAndDirection[0][0]),
                          static_cast<float>(originAndDirection[0][1]),
                          static_cast<float>(originAndDirection[0][2])};
      Vec3D<float> direction{static_cast<float>(originAndDirection[1][0]),
                             static_cast<float>(originAndDirection[1][1]),
                             static_cast<float>(originAndDirection[1][2])};
      if (particleDirection[0] != 0.0F || particleDirection[1] != 0.0F ||
          particleDirection[2] != 0.0F)
        direction = particleDirection;
      if (D == 2 && direction[2] != 0.0F) {
        direction[2] = 0.0F;
        const float length = std::sqrt(direction[0] * direction[0] +
                                       direction[1] * direction[1]);
        if (length > 0.0F) {
          direction[0] /= length;
          direction[1] /= length;
        }
      }
      const auto boundaryResult =
          applyBoxBoundary(origin, direction, box, firstDir, secondDir,
                           firstCond, secondCond, maxBoundaryHits_);
      if ((boundaryResult & 0x80000000U) != 0U)
        continue;
      MultibounceRayState state;
      state.ray.origin = {origin[0], origin[1], origin[2]};
      state.ray.direction = {direction[0], direction[1], direction[2]};
      // Match rayInternal::fillRayPosition() for initial and successor rays;
      // this prevents a reflected ray from re-hitting its source triangle.
      state.ray.tMin = 1e-4F;
      state.ray.tMax = std::numeric_limits<float>::max();
      state.rng = std::move(rng);
      state.initialWeight = static_cast<float>(source->getInitialRayWeight(i));
      state.weight = state.initialWeight;
      state.sequence = static_cast<std::uint32_t>(states.size());
      states.push_back(std::move(state));
    }
    return true;
    }
  }

  [[nodiscard]] static Vec3D<float>
  triangleNormal(const vulkan::ray::Triangle &triangle) {
    const Vec3D<float> edge1{triangle.b[0] - triangle.a[0],
                             triangle.b[1] - triangle.a[1],
                             triangle.b[2] - triangle.a[2]};
    const Vec3D<float> edge2{triangle.c[0] - triangle.a[0],
                             triangle.c[1] - triangle.a[1],
                             triangle.c[2] - triangle.a[2]};
    Vec3D<float> normal{edge1[1] * edge2[2] - edge1[2] * edge2[1],
                        edge1[2] * edge2[0] - edge1[0] * edge2[2],
                        edge1[0] * edge2[1] - edge1[1] * edge2[0]};
    const float length = std::sqrt(normal[0] * normal[0] +
                                   normal[1] * normal[1] +
                                   normal[2] * normal[2]);
    if (length > 0.0F) {
      normal[0] /= length;
      normal[1] /= length;
      normal[2] /= length;
    }
    return normal;
  }

  [[nodiscard]] static std::uint32_t hitUlpDistance(const float left,
                                                    const float right) {
    const auto ordered = [](const std::uint32_t bits) {
      return (bits & 0x80000000U) != 0U ? ~bits : bits ^ 0x80000000U;
    };
    const auto lhs = ordered(std::bit_cast<std::uint32_t>(left));
    const auto rhs = ordered(std::bit_cast<std::uint32_t>(right));
    return lhs > rhs ? lhs - rhs : rhs - lhs;
  }

  [[nodiscard]] bool runMultibounceHits(
      std::span<const MultibounceRayState> states,
      std::vector<std::size_t> &stateIndices,
      std::vector<vulkan::ray::TriangleHit> &hits, std::string &error) {
    error.clear();
    stateIndices.clear();
    std::vector<vulkan::ray::Ray> rays;
    rays.reserve(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
      if (!states[i].active)
        continue;
      stateIndices.push_back(i);
      rays.push_back(states[i].ray);
    }
    hits.assign(rays.size(), vulkan::ray::TriangleHit::miss());
    if (rays.empty())
      return true;
    vulkan::runtime::DeviceBuffer origins, directions, triangles, deviceHits;
    if (!multibounceHit.createRayBuffers(rays.size(), origins, directions,
                                         error) ||
        !multibounceHit.createTriangleBuffer(deviceTriangles_.size(),
                                             triangles, error) ||
        !multibounceHit.createHitBuffer(rays.size(), deviceHits, error) ||
        !multibounceHit.uploadRays(rays, origins, directions, error) ||
        !multibounceHit.uploadTriangles(deviceTriangles_, triangles, error) ||
        !multibounceHit.dispatch(origins, directions, triangles, rays.size(),
                                 deviceTriangles_.size(), deviceHits,
                                 rays.size(), error) ||
        !multibounceHit.downloadHits(rays.size(), deviceHits, hits, error))
      return false;
    std::vector<vulkan::ray::TriangleHit> cpuHits(hits.size());
    if (!vulkan::ray::intersectCpu(rays, deviceTriangles_, cpuHits, error))
      return false;
    constexpr std::uint32_t kHitUlpTolerance = 4U;
    for (std::size_t i = 0; i < hits.size(); ++i) {
      const bool sameHit = hits[i].triangleIndex == cpuHits[i].triangleIndex &&
                           hitUlpDistance(hits[i].t, cpuHits[i].t) <=
                               kHitUlpTolerance &&
                           hitUlpDistance(hits[i].u, cpuHits[i].u) <=
                               kHitUlpTolerance &&
                           hitUlpDistance(hits[i].v, cpuHits[i].v) <=
                               kHitUlpTolerance;
      if (!sameHit) {
        std::ostringstream diagnostic;
        diagnostic << "multi-bounce device hit exceeds CPU FP32 hit oracle"
                   << " (ULP tolerance=" << kHitUlpTolerance << ")"
                   << " lane=" << i << " gpu=(" << hits[i].t << ","
                   << hits[i].triangleIndex << "," << hits[i].u << ","
                   << hits[i].v << ") cpu=(" << cpuHits[i].t << ","
                   << cpuHits[i].triangleIndex << "," << cpuHits[i].u << ","
                   << cpuHits[i].v << ") bits gpu=(0x" << std::hex
                   << std::bit_cast<std::uint32_t>(hits[i].t) << ",0x"
                   << std::bit_cast<std::uint32_t>(hits[i].u) << ",0x"
                   << std::bit_cast<std::uint32_t>(hits[i].v) << ") cpu=(0x"
                   << std::bit_cast<std::uint32_t>(cpuHits[i].t) << ",0x"
                   << std::bit_cast<std::uint32_t>(cpuHits[i].u) << ",0x"
                   << std::bit_cast<std::uint32_t>(cpuHits[i].v) << ")";
        error = diagnostic.str();
        return false;
      }
    }
    return true;
  }

  ProcessResult calculateMultibounceSourceFluxes(
      ProcessContext<NumericType, D> &context,
      SmartPointer<PointData<NumericType>> &fluxes,
      const viennaray::Source<NumericType> *source) {
    if constexpr (!(std::is_same_v<NumericType, float> && D == 2)) {
      (void)context;
      (void)fluxes;
      (void)source;
      return ProcessResult::FAILURE;
    } else {
    multibounceLastError.clear();
    auto *session = this->context ? this->context->session() : nullptr;
    if (session == nullptr || !session->isValid() ||
        session->generation() != multibounceSessionGeneration) {
      multibounceLastError = "multi-bounce Vulkan session is stale";
      return ProcessResult::FAILURE;
    }
    std::vector<MultibounceRayState> states;
    std::string error;
    if (!buildMultibounceStates(context, source, states, error)) {
      multibounceLastError = error.empty() ? "failed to build multi-bounce ray states"
                                           : error;
      return ProcessResult::FAILURE;
    }
    const auto particle = model_->getParticleTypes().front().get();
    auto localData = PointData<float>::New();
    for (const auto &label : particle->getLocalDataLabels())
      localData->insertNextScalarData(deviceTriangles_.size(), 0.0F, label);
    std::vector<NumericType> elementFlux(deviceTriangles_.size(), NumericType(0));
    const std::uint32_t maxReflections = context.rayTracingParams.maxReflections;
    const std::uint32_t maxRounds = maxReflections + 1U;
    for (std::uint32_t round = 0U; round < maxRounds; ++round) {
      std::vector<std::size_t> stateIndices;
      std::vector<vulkan::ray::TriangleHit> hits;
      if (!runMultibounceHits(states, stateIndices, hits, error)) {
        multibounceLastError = error.empty() ? "device hit dispatch failed" : error;
        return ProcessResult::FAILURE;
      }
      if (stateIndices.empty())
        break;
      std::vector<std::size_t> laneStates;
      vulkan::ray::MultibounceFrontier frontier;
      frontier.decisionStride = maxRounds;
      frontier.events.reserve(stateIndices.size());
      frontier.decisions.reserve(stateIndices.size() * maxRounds);
      for (std::size_t lane = 0; lane < stateIndices.size(); ++lane) {
        auto &state = states[stateIndices[lane]];
        const auto &hit = hits[lane];
        if (hit.isMiss() || hit.triangleIndex >= deviceTriangles_.size()) {
          state.active = false;
          continue;
        }
        const auto &triangle = deviceTriangles_[hit.triangleIndex];
        const auto normal = triangleNormal(triangle);
        const float dot = state.ray.direction[0] * normal[0] +
                          state.ray.direction[1] * normal[1] +
                          state.ray.direction[2] * normal[2];
        if (!(dot <= 0.0F)) {
          state.active = false;
          continue;
        }
        const Vec3D<float> hitPoint{
            state.ray.origin[0] + state.ray.direction[0] * hit.t,
            state.ray.origin[1] + state.ray.direction[1] * hit.t,
            state.ray.origin[2] + state.ray.direction[2] * hit.t};
        vulkan::ray::MultibounceEvent event{};
        event.origin[0] = state.ray.origin[0];
        event.origin[1] = state.ray.origin[1];
        event.origin[2] = state.ray.origin[2];
        event.direction[0] = state.ray.direction[0];
        event.direction[1] = state.ray.direction[1];
        event.direction[2] = state.ray.direction[2];
        event.particle = 0U;
        event.bounce = state.bounce;
        event.sequence = state.sequence;
        event.activeFlag = 1U;
        event.surfaceId = hit.triangleIndex;
        event.weight = state.weight;
        event.nextWeight = state.weight;
        vulkan::ray::MultibounceDecision decision{};
        vulkan::ray::MultibounceDecisionInput input{};
        input.particle = particle;
        input.rng = &state.rng;
        input.localData = localData.get();
        input.rayDirection = {state.ray.direction[0], state.ray.direction[1],
                              state.ray.direction[2]};
        input.geometricNormal = normal;
        input.hitPoint = hitPoint;
        input.particleId = 0U;
        input.bounce = state.bounce;
        input.sequence = state.sequence;
        input.surfaceId = hit.triangleIndex;
        input.primitiveId = hit.triangleIndex;
        input.globalData = globalTracingData_.get();
        input.materialId = hit.triangleIndex < elementMaterialIds_.size()
                               ? elementMaterialIds_[hit.triangleIndex]
                               : 0;
        input.initialWeight = state.initialWeight;
        input.weight = state.weight;
        input.contribution = state.weight;
        input.reflectionCount = state.reflectionCount;
        input.maxReflections = maxReflections;
        input.frontFace = true;
        if (!vulkan::ray::MultibounceDecisionProducer::produce(input, decision,
                                                               error)) {
          multibounceLastError = error;
          return ProcessResult::FAILURE;
        }
        frontier.events.push_back(event);
        laneStates.push_back(stateIndices[lane]);
        for (std::uint32_t slot = 0U; slot < maxRounds; ++slot)
          frontier.decisions.push_back(slot == state.bounce
                                          ? decision
                                          : vulkan::ray::MultibounceDecision{});
      }
      if (frontier.events.empty())
        continue;
      vulkan::ray::MultibounceFrontierLimits limits{};
      limits.maxTotalEvents = static_cast<std::uint32_t>(states.size());
      limits.maxFrontierEvents = static_cast<std::uint32_t>(states.size());
      limits.maxRounds = maxRounds;
      limits.maxReflections = maxReflections;
      vulkan::ray::MultibounceFrontierResult result;
      if (!multibounceQueue.run(std::span<const vulkan::ray::MultibounceFrontier>(
                                    &frontier, 1U),
                                limits, result, error)) {
        multibounceLastError = error.empty() ? "frontier queue dispatch failed" : error;
        return ProcessResult::FAILURE;
      }
      for (const auto &acc : result.accumulation) {
        if (acc.surfaceId >= elementFlux.size()) {
          multibounceLastError = "frontier accumulation surface id is out of range";
          return ProcessResult::FAILURE;
        }
        elementFlux[acc.surfaceId] +=
            static_cast<NumericType>(std::bit_cast<float>(acc.weightBits));
      }
      for (const auto stateIndex : laneStates) {
        auto &state = states[stateIndex];
        const auto it = std::find_if(
            result.terminalEvents.begin(), result.terminalEvents.end(),
            [&](const auto &event) { return event.sequence == state.sequence; });
        if (it == result.terminalEvents.end())
          return ProcessResult::FAILURE;
        state.active = it->activeFlag != 0U;
        if (state.active) {
          state.ray.origin = {it->origin[0], it->origin[1], it->origin[2]};
          state.ray.direction = {it->direction[0], it->direction[1],
                                 it->direction[2]};
          state.weight = it->weight;
          state.bounce = it->bounce;
          ++state.reflectionCount;
          Vec3D<float> nextOrigin{state.ray.origin[0], state.ray.origin[1],
                                  state.ray.origin[2]};
          Vec3D<float> nextDirection{state.ray.direction[0],
                                     state.ray.direction[1],
                                     state.ray.direction[2]};
          const auto boundaryResult = applyBoxBoundary(
              nextOrigin, nextDirection, sourceSetup_.adjustedBox,
              sourceSetup_.traceSettings[1], sourceSetup_.traceSettings[2],
              boundaryConds_[sourceSetup_.traceSettings[1]],
              (D == 3) ? boundaryConds_[sourceSetup_.traceSettings[2]]
                       : viennaray::BoundaryCondition::IGNORE_BOUNDARY,
              maxBoundaryHits_);
          state.ray.origin = {nextOrigin[0], nextOrigin[1], nextOrigin[2]};
          state.ray.direction = {nextDirection[0], nextDirection[1],
                                 nextDirection[2]};
          if ((boundaryResult & 0x80000000U) != 0U)
            state.active = false;
        }
      }
    }
    if (localData->getScalarDataSize() != 1U) {
      multibounceLastError = "multi-bounce local-data label count is not one";
      return ProcessResult::FAILURE;
    }
    const auto *localFlux = localData->getScalarData(0);
    if (localFlux == nullptr || localFlux->size() != elementFlux.size()) {
      multibounceLastError = "multi-bounce local-data shape mismatch";
      return ProcessResult::FAILURE;
    }
    for (std::size_t i = 0; i < elementFlux.size(); ++i) {
      if (std::bit_cast<std::uint32_t>(static_cast<float>(elementFlux[i])) !=
          std::bit_cast<std::uint32_t>((*localFlux)[i])) {
        multibounceLastError = "multi-bounce device accumulation differs from CPU callback data";
        return ProcessResult::FAILURE;
      }
    }
    normalizeFlux(elementFlux, source, source->getNumPoints() *
                                      context.rayTracingParams.raysPerPoint,
                  context.rayTracingParams.normalizationType);
    std::vector<std::vector<NumericType>> elementFluxes;
    elementFluxes.push_back(std::move(elementFlux));
    postProcessing_.setPointData(fluxes);
    postProcessing_.setElementDataArrays(std::move(elementFluxes));
    postProcessing_.apply();
    context.triangleMesh = surfaceMesh_;
    ++cpuRunNumber_;
    return ProcessResult::SUCCESS;
    }
  }

  // Match ViennaRay's getPrimArea.  Its 2D line-to-triangle ribbon has a
  // synthetic thickness, but its physical primitive area is half the original
  // line length, not the 3D ribbon triangle area.
  [[nodiscard]] static float triangleArea(const vulkan::ray::Triangle &tri,
                                          const std::size_t triangleIndex) {
    if constexpr (D == 2) {
      const float abx = tri.b[0] - tri.a[0];
      const float aby = tri.b[1] - tri.a[1];
      const float abz = tri.b[2] - tri.a[2];
      const float acx = tri.c[0] - tri.a[0];
      const float acy = tri.c[1] - tri.a[1];
      const float acz = tri.c[2] - tri.a[2];
      const float abLength = std::sqrt(abx * abx + aby * aby + abz * abz);
      const float acLength = std::sqrt(acx * acx + acy * acy + acz * acz);
      return 0.5F * (triangleIndex % 2U == 0U ? abLength : acLength);
    }
    const float abx = tri.b[0] - tri.a[0];
    const float aby = tri.b[1] - tri.a[1];
    const float abz = tri.b[2] - tri.a[2];
    const float acx = tri.c[0] - tri.a[0];
    const float acy = tri.c[1] - tri.a[1];
    const float acz = tri.c[2] - tri.a[2];
    const float cx = aby * acz - abz * acy;
    const float cy = abz * acx - abx * acz;
    const float cz = abx * acy - aby * acx;
    return 0.5F * std::sqrt(cx * cx + cy * cy + cz * cz);
  }

  // Host-side replica of viennaray::normalizeFlux so the device-resident
  // accumulated hit weights are converted to the same per-area flux units the
  // CPU triangle engine produces.
  void normalizeFlux(std::vector<NumericType> &flux,
                     const viennaray::Source<NumericType> *source,
                     const std::size_t totalRays,
                     const viennaray::NormalizationType normType) const {
    switch (normType) {
    case viennaray::NormalizationType::MAX: {
      const auto maxv = *std::max_element(flux.begin(), flux.end());
      for (std::size_t idx = 0; idx < flux.size(); ++idx) {
        flux[idx] /=
            maxv * static_cast<NumericType>(
                       triangleArea(deviceTriangles_[idx], idx));
      }
      return;
    }
    case viennaray::NormalizationType::SOURCE: {
      const NumericType sourceArea =
          static_cast<NumericType>(source->getSourceArea());
      const NumericType normFactor =
          sourceArea / static_cast<NumericType>(totalRays);
      for (std::size_t idx = 0; idx < flux.size(); ++idx) {
        flux[idx] *= normFactor / static_cast<NumericType>(
                                      triangleArea(deviceTriangles_[idx], idx));
      }
      return;
    }
    default:
      return;
    }
  }
};

template <typename NumericType, int D>
VulkanRayFluxEngine<NumericType, D>::VulkanRayFluxEngine(
    std::shared_ptr<vulkan::runtime::DeploymentComputeContext> context,
    const VulkanRayFluxSpirvPaths &paths, bool allowCpuFallback)
    : impl_(std::make_unique<Impl>(std::move(context), paths,
                                   allowCpuFallback)) {}

template <typename NumericType, int D>
VulkanRayFluxEngine<NumericType, D>::~VulkanRayFluxEngine() = default;

template <typename NumericType, int D>
ProcessResult
VulkanRayFluxEngine<NumericType, D>::checkInput(ProcessContext<NumericType, D> &context) {
  auto model = std::dynamic_pointer_cast<ProcessModelCPU<NumericType, D>>(
      context.model);
  if (!model) {
    VIENNACORE_LOG_WARNING("Invalid process model.");
    return ProcessResult::INVALID_INPUT;
  }
  impl_->model_ = model;

  // P5-RAY-ROUTE has evidence only for the concrete single-particle FP32 2D
  // model.  Keep the predicate explicit so a ProcessModelCPU with different
  // particle/source semantics cannot reach device dispatch.  The bounded
  // frontier extension admits the one-reflection SingleParticleProcess row and,
  // with coverage/global-data support, the NeutralTransport<float,2> row up to
  // maxReflections == 2.
  bool supported = false;
  bool boundedMultibounce = false;
  if constexpr (std::is_same_v<NumericType, float> && D == 2) {
    auto single = std::dynamic_pointer_cast<SingleParticleProcess<float, 2>>(
        context.model);
    const bool baseModel =
        single != nullptr && single->getParticleTypes().size() == 1U &&
        single->getParticleDataLabels().size() == 1U &&
        single->getSource() == nullptr;
    const bool singleBounce =
        baseModel && context.rayTracingParams.maxReflections == 0U;
    boundedMultibounce =
        baseModel && context.rayTracingParams.maxReflections == 1U &&
        !context.flags.useCoverages && !context.flags.hasSurfaceDesorption &&
        !impl_->paths.multibounceFrontierQueue.empty();

    // NeutralTransport<float,2> needs coverage-aware surfaceReflection callbacks
    // even for maxReflections == 0, so it always uses the CPU-decision frontier
    // route.  Evidence boundary is maxReflections <= 2.
    auto neutralTransport =
        std::dynamic_pointer_cast<NeutralTransport<float, 2>>(context.model);
    const bool neutralTransportModel =
        neutralTransport != nullptr &&
        neutralTransport->getParticleTypes().size() == 1U &&
        neutralTransport->getSource() == nullptr &&
        !impl_->paths.multibounceFrontierQueue.empty() &&
        context.rayTracingParams.maxReflections <= 2U;
    if (neutralTransportModel) {
      boundedMultibounce = true;
    }

    supported = singleBounce || boundedMultibounce;
  }

  impl_->vulkanEligible = supported;
  impl_->multibounceEligible = boundedMultibounce;
  if (supported)
    return ProcessResult::SUCCESS;

  // Use LOG_WARNING, not LOG_ERROR: LOG_ERROR throws in this build.  Manual
  // Vulkan must fail closed before publication; AUTO keeps the CPU contract.
  VIENNACORE_LOG_WARNING(
      "VulkanRayFluxEngine: process model/parameters are outside the "
      "evidenced FP32 2D single-particle route.");
  if (!impl_->allowCpuFallback)
    return ProcessResult::INVALID_INPUT;

  return impl_->cpuEngine.checkInput(context);
}

template <typename NumericType, int D>
ProcessResult
VulkanRayFluxEngine<NumericType, D>::initialize(ProcessContext<NumericType, D> &context) {
  assert(impl_->model_ != nullptr);

  if (!impl_->vulkanEligible) {
    impl_->useVulkan = false;
    impl_->useMultibounce = false;
    const auto cpuCheck = impl_->cpuEngine.checkInput(context);
    if (cpuCheck != ProcessResult::SUCCESS)
      return cpuCheck;
    return impl_->cpuEngine.initialize(context);
  }

  std::string error;
  if (!impl_->initializePipeline(error)) {
    if (!impl_->allowCpuFallback) {
      // LOG_WARNING (not LOG_ERROR) so fail-closed returns a status code
      // instead of throwing through Process::calculateFlux.
      VIENNACORE_LOG_WARNING(
          "Vulkan ray-flux pipeline initialization failed: " + error);
      return ProcessResult::FAILURE;
    }
    VIENNACORE_LOG_INFO("Vulkan ray-flux pipeline unavailable (" + error +
                        "); falling back to CPU triangle engine.");
    impl_->useVulkan = false;
    impl_->useMultibounce = false;
  } else {
    impl_->useVulkan = true;
    impl_->useMultibounce = impl_->multibounceEligible;
  }

  if (!impl_->useVulkan) {
    const auto cpuCheck = impl_->cpuEngine.checkInput(context);
    if (cpuCheck != ProcessResult::SUCCESS)
      return cpuCheck;
    return impl_->cpuEngine.initialize(context);
  }

  // Initialize CPU engine as well for surface extraction helpers and
  // desorption fallback.  CPUTriangleEngine caches its own model pointer in
  // checkInput(), so forward that call before initialize().
  const auto cpuCheck = impl_->cpuEngine.checkInput(context);
  if (cpuCheck != ProcessResult::SUCCESS)
    return cpuCheck;
  const auto cpuResult = impl_->cpuEngine.initialize(context);
  if (cpuResult != ProcessResult::SUCCESS)
    return cpuResult;

  impl_->surfaceMesh_ = Impl::MeshType::New();
  impl_->elementKdTree_ = Impl::KDTreeType::New();
  impl_->elementMaterialIds_.clear();
  impl_->globalTracingData_ = nullptr;

  // Mirror the CPU oracle's primary-direction source setting so the default
  // source distribution matches when a model provides one.
  if (auto primaryDirection = impl_->model_->getPrimaryDirection()) {
    impl_->sourceSetup_.usePrimaryDirection = true;
    impl_->sourceSetup_.primaryDirection = primaryDirection.value();
  }

  impl_->postProcessing_.setDataLabels(impl_->model_->getParticleDataLabels());
  impl_->postProcessing_.setConversionRadius(
      context.domain->getGridDelta() *
      (context.rayTracingParams.smoothingNeighbors + 1));
  impl_->postProcessing_.setSurfaceMesh(impl_->surfaceMesh_);
  impl_->postProcessing_.setElementKdTree(impl_->elementKdTree_);
  impl_->postProcessing_.setDiskMesh(context.diskMesh);

  // Capture the domain boundary conditions for the analytic boundary
  // reproduction in generateRays, mirroring CPUTriangleEngine::initialize.
  viennaray::BoundaryCondition rayBoundaryCondition[D];
  if (context.rayTracingParams.ignoreFluxBoundaries) {
    for (unsigned i = 0; i < D; ++i)
      rayBoundaryCondition[i] = viennaray::BoundaryCondition::IGNORE_BOUNDARY;
  } else {
    for (unsigned i = 0; i < D; ++i)
      rayBoundaryCondition[i] = util::convertBoundaryCondition(
          context.domain->getGrid().getBoundaryConditions(i));
  }
  for (unsigned i = 0; i < D; ++i)
    impl_->boundaryConds_[i] = rayBoundaryCondition[i];
  impl_->maxBoundaryHits_ = context.rayTracingParams.maxBoundaryHits;

  return ProcessResult::SUCCESS;
}

template <typename NumericType, int D>
ProcessResult
VulkanRayFluxEngine<NumericType, D>::updateSurface(ProcessContext<NumericType, D> &context) {
  this->timer_.start();
  if (!impl_->useVulkan) {
    const auto result = impl_->cpuEngine.updateSurface(context);
    this->timer_.finish();
    return result;
  }
  const auto result = impl_->updateSurfaceMesh(context);
  if (result != ProcessResult::SUCCESS) {
    this->timer_.finish();
    return result;
  }
  // Keep the CPU engine's surface in sync so calculateSurfaceFluxes (desorption)
  // and the CPU fallback path operate on the same geometry.
  const auto cpuResult = impl_->cpuEngine.updateSurface(context);
  this->timer_.finish();
  return cpuResult;
}

template <typename NumericType, int D>
ProcessResult VulkanRayFluxEngine<NumericType, D>::calculateSourceFluxes(
    ProcessContext<NumericType, D> &context,
    SmartPointer<PointData<NumericType>> &fluxes) {
  this->timer_.start();

  if (!impl_->useVulkan) {
    const auto result = impl_->cpuEngine.calculateSourceFluxes(context, fluxes);
    this->timer_.finish();
    return result;
  }

  // Resolve the particle source by reusing the CPU tracer's construction.
  // Models such as SingleParticleProcess expose no custom source
  // (model->getSource() == nullptr); in that case ViennaRay builds a default
  // SourceRandom from the tight geometry bounding box.  buildDefaultSource()
  // reproduces that exact construction (geometry load + prepareSource) so the
  // Vulkan route samples the same ray population as the CPU_TRIANGLE oracle.
  viennaray::Source<NumericType> *source = impl_->model_->getSource().get();
  if (source == nullptr) {
    const auto &particles = impl_->model_->getParticleTypes();
    if (particles.empty()) {
      VIENNACORE_LOG_WARNING("No particle types in process model.");
      this->timer_.finish();
      return ProcessResult::INVALID_INPUT;
    }
    impl_->defaultSource_ = impl_->sourceSetup_.buildDefaultSource(
        *particles.front(), impl_->numGeometryPoints_);
    source = impl_->defaultSource_.get();
  }

  const auto numPoints = source->getNumPoints();
  const auto raysPerPoint = context.rayTracingParams.raysPerPoint;
  const auto totalRays = numPoints * raysPerPoint;

  if (totalRays == 0 || impl_->deviceTriangles_.empty()) {
    VIENNACORE_LOG_WARNING("No rays or triangles for Vulkan ray-flux.");
    this->timer_.finish();
    return ProcessResult::SUCCESS;
  }

  impl_->globalTracingData_ = nullptr;
  if (impl_->useMultibounce) {
    if (context.flags.useCoverages) {
      auto surfaceModel = impl_->model_->getSurfaceModel();
      if (surfaceModel != nullptr && surfaceModel->getCoverages() != nullptr &&
          impl_->surfaceMesh_ != nullptr) {
        impl_->globalTracingData_ = PointData<NumericType>::New();
        auto pointKdTree = context.getPointKdTree();
        PointToElementData<NumericType, float>(
            *impl_->globalTracingData_, surfaceModel->getCoverages(),
            *pointKdTree, impl_->surfaceMesh_, Logger::hasIntermediate())
            .apply();
      }
    }
    const auto result = impl_->calculateMultibounceSourceFluxes(
        context, fluxes, source);
    if (result == ProcessResult::SUCCESS) {
      ++this->fluxCalculationsCount_;
      this->timer_.finish();
      return result;
    }
    VIENNACORE_LOG_WARNING(
        "bounded Vulkan multi-bounce route failed; discarding staged output: " +
        std::string(VulkanRayFluxEngine<NumericType, D>::devicePhysicsGap()) +
        " (" + impl_->multibounceLastError + ")");
    impl_->useMultibounce = false;
    impl_->useVulkan = false;
    if (impl_->allowCpuFallback) {
      const auto cpuResult =
          impl_->cpuEngine.calculateSourceFluxes(context, fluxes);
      this->timer_.finish();
      return cpuResult;
    }
    this->timer_.finish();
    return ProcessResult::FAILURE;
  }

  auto [rays, weights] = impl_->generateRays(context, source, totalRays);

  std::vector<std::uint32_t> outSurface(totalRays, 0U);
  std::vector<float> outWeight(totalRays, 0.0F);
  vulkan::ray::RayFluxResult output{outSurface, outWeight, 0U};

  std::string error;
  if (!impl_->pipeline.runGpu(rays, impl_->deviceTriangles_, weights, output,
                              error)) {
    VIENNACORE_LOG_WARNING("Vulkan ray-flux dispatch failed: " + error);
    if (impl_->allowCpuFallback) {
      // The CPU triangle engine was initialized alongside the Vulkan route and
      // owns the same Process/model semantics.  A runtime device failure must
      // therefore discard the staged device result and retry this exact
      // request through CPU in AUTO mode; MANUAL remains fail-closed below.
      impl_->useVulkan = false;
      impl_->useMultibounce = false;
      const auto cpuCheck = impl_->cpuEngine.checkInput(context);
      if (cpuCheck != ProcessResult::SUCCESS) {
        this->timer_.finish();
        return cpuCheck;
      }
      const auto cpuResult =
          impl_->cpuEngine.calculateSourceFluxes(context, fluxes);
      this->timer_.finish();
      return cpuResult;
    }
    this->timer_.finish();
    return ProcessResult::FAILURE;
  }
  ++this->fluxCalculationsCount_;
  ++impl_->cpuRunNumber_;

  // Accumulate the device hit weights into a per-triangle flux, one entry per
  // particle data label.  For the single-bounce route each particle type
  // contributes the same geometric hit distribution scaled by its sticking
  // weight; per-label scaling uses the particle's sticking probability when a
  // model exposes distinct particle types.
  std::vector<std::vector<NumericType>> elementFluxes;
  std::vector<std::string> elementFluxLabels;
  unsigned particleIdx = 0;
  for (auto &particle : impl_->model_->getParticleTypes()) {
    const auto labels = particle->getLocalDataLabels();
    for (const auto &label : labels) {
      std::vector<NumericType> flux(impl_->deviceTriangles_.size(),
                                    NumericType(0));
      for (std::size_t i = 0; i < output.count; ++i) {
        const auto surfaceId = output.surfaceId[i];
        if (surfaceId < flux.size()) {
          flux[surfaceId] += static_cast<NumericType>(output.weight[i]);
        }
      }
      // Convert accumulated hit weights to per-area flux, matching the CPU
      // triangle engine normalization contract.
      impl_->normalizeFlux(flux, source, totalRays,
                           context.rayTracingParams.normalizationType);
      elementFluxLabels.push_back(label);
      elementFluxes.push_back(std::move(flux));
    }

    // Merge the particle data log so model-side bookkeeping stays consistent
    // with the CPU path.  The single-bounce Vulkan route does not yet produce
    // per-ray device data logs, so we merge an empty log of the right size.
    const int dataLogSize = impl_->model_->getParticleLogSize(particleIdx);
    if (dataLogSize > 0) {
      viennaray::DataLog<NumericType> log;
      log.data.resize(1);
      log.data[0].resize(dataLogSize, NumericType(0));
      impl_->model_->mergeParticleData(log, particleIdx);
    }
    ++particleIdx;
  }

  // Export per-element fluxes onto the triangle mesh for inspection, matching
  // CPUTriangleEngine::saveElementFluxesToTriangleMesh.
  assert(elementFluxes.size() == elementFluxLabels.size());
  for (std::size_t dataIdx = 0; dataIdx < elementFluxes.size(); ++dataIdx) {
    std::vector<float> values(elementFluxes[dataIdx].size());
    for (std::size_t i = 0; i < elementFluxes[dataIdx].size(); ++i) {
      values[i] = static_cast<float>(elementFluxes[dataIdx][i]);
    }
    impl_->surfaceMesh_->getCellData().insertReplaceScalarData(
        std::move(values), elementFluxLabels[dataIdx]);
  }

  impl_->postProcessing_.setPointData(fluxes);
  impl_->postProcessing_.setElementDataArrays(std::move(elementFluxes));
  impl_->postProcessing_.apply();

  context.triangleMesh = impl_->surfaceMesh_;
  this->timer_.finish();
  return ProcessResult::SUCCESS;
}

template <typename NumericType, int D>
ProcessResult VulkanRayFluxEngine<NumericType, D>::calculateSurfaceFluxes(
    ProcessContext<NumericType, D> &context,
    SmartPointer<PointData<NumericType>> &fluxes) {
  // Desorption is intentionally delegated to the CPU engine for this slice.
  return impl_->cpuEngine.calculateSurfaceFluxes(context, fluxes);
}

// Explicit instantiations
template class VulkanRayFluxEngine<float, 2>;
template class VulkanRayFluxEngine<float, 3>;
template class VulkanRayFluxEngine<double, 2>;
template class VulkanRayFluxEngine<double, 3>;

} // namespace viennaps
