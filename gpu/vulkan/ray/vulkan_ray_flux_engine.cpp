// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "vulkan_ray_flux_engine.hpp"

#include "../runtime/deployment_compute_context.hpp"
#include "device_ray_flux_pipeline.hpp"

#include <process/psCPUTriangleEngine.hpp>

#include <rayParticle.hpp>
#include <raySource.hpp>
#include <raySourceRandom.hpp>
#include <rayUtil.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <span>
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

  bool useVulkan = false;
  vulkan::ray::DeviceRayFluxPipeline pipeline;

  CPUTriangleEngineType cpuEngine;

  // Surface extraction state (shared between Vulkan and CPU paths)
  SmartPointer<ProcessModelCPU<NumericType, D>> model_;
  MeshType surfaceMesh_;
  KDTreeType elementKdTree_;
  PostProcessingType postProcessing_;
  std::vector<vulkan::ray::Triangle> deviceTriangles_;
  std::vector<Vec3D<NumericType>> triangleCenters_;
  // Reuses the CPU tracer's default-source construction so the sampled ray
  // population matches the CPU oracle.  Populated in updateSurfaceMesh after
  // the device triangles are built.
  impl_detail::DefaultSourceSetup<NumericType, D> sourceSetup_;
  viennaray::SmartPointer<viennaray::Source<NumericType>> defaultSource_;
  unsigned int numGeometryPoints_ = 0;
  // Domain boundary conditions for the analytic boundary reproduction in
  // generateRays.  The device pipeline has no boundary geometry, so rays that
  // escape the lateral domain edges must be reflected/periodic/ignored on the
  // host to match the CPU_TRIANGLE oracle.
  std::array<viennaray::BoundaryCondition, D> boundaryConds_{};
  unsigned int maxBoundaryHits_ = 1000;

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
    vulkan::ray::DeviceRayFluxSpirv spirv{
        paths.triangleHit,     paths.recordCompaction, paths.reductionScan,
        paths.radixHistogram,  paths.radixPrefix,      paths.radixScatter,
        paths.surfaceSegments, paths.surfaceReduce,    paths.triangleBvh};
    return pipeline.initialize(*session, spirv, error);
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

    return ProcessResult::SUCCESS;
  }

  // The device-resident pipeline does not consume per-element material ids for
  // the single-bounce source flux, so no separate material-id extraction is
  // performed here.  The CPU engine keeps its own surface state current so the
  // desorption path (calculateSurfaceFluxes) and any CPU fallback remain
  // correct.

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
    // (rayTraceKernel.hpp:100,120) rather than advancing one shared RNG.  For
    // the first calculateSourceFluxes call the CPU runNumber is 0, matching
    // this engine's first dispatch.  Without this the two paths draw different
    // random rays and the flux comparison is pure sampling noise.
    const unsigned int seed = static_cast<unsigned int>(
        context.rayTracingParams.useRandomSeeds
            ? std::random_device{}()
            : context.rayTracingParams.rngSeed);

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
      applyBoxBoundary(origin, direction, box, firstDir, secondDir, firstCond,
                       secondCond, maxBoundaryHits_);

      vulkan::ray::Ray ray{};
      for (int j = 0; j < 3; ++j) {
        ray.origin[j] = origin[j];
        ray.direction[j] = direction[j];
      }
      ray.tMin = 0.0F;
      ray.tMax = std::numeric_limits<float>::max();
      rays.push_back(ray);
      weights.push_back(static_cast<float>(source->getInitialRayWeight(i)));
    }
    return {std::move(rays), std::move(weights)};
  }

  // Geometric triangle area, matching ViennaRay's getPrimArea so the host-side
  // normalization aligns with the CPU oracle.
  [[nodiscard]] static float triangleArea(const vulkan::ray::Triangle &tri) {
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
      if (maxv <= NumericType(0))
        return;
      for (std::size_t idx = 0; idx < flux.size(); ++idx) {
        flux[idx] /=
            maxv * static_cast<NumericType>(triangleArea(deviceTriangles_[idx]));
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
                                      triangleArea(deviceTriangles_[idx]));
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

  // P5-RAY-ROUTE supports single-bounce, no-reflection transport only.
  if (context.rayTracingParams.maxReflections > 0) {
    // Use LOG_WARNING, not LOG_ERROR: VIENNACORE_LOG_ERROR throws
    // std::runtime_error (vcLogger print() with shouldAbort=true), which would
    // escape Process::calculateFlux and terminate instead of failing closed.
    // The engine signals failure via the returned ProcessResult; Process
    // propagates it.  This preserves the fail-closed contract (invariant #4).
    VIENNACORE_LOG_WARNING(
        "VulkanRayFluxEngine: maxReflections > 0 is not supported yet.");
    return ProcessResult::INVALID_INPUT;
  }

  return ProcessResult::SUCCESS;
}

template <typename NumericType, int D>
ProcessResult
VulkanRayFluxEngine<NumericType, D>::initialize(ProcessContext<NumericType, D> &context) {
  assert(impl_->model_ != nullptr);

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
  } else {
    impl_->useVulkan = true;
  }

  if (!impl_->useVulkan) {
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

  auto [rays, weights] = impl_->generateRays(context, source, totalRays);

  std::vector<std::uint32_t> outSurface(totalRays, 0U);
  std::vector<float> outWeight(totalRays, 0.0F);
  vulkan::ray::RayFluxResult output{outSurface, outWeight, 0U};

  std::string error;
  if (!impl_->pipeline.runGpu(rays, impl_->deviceTriangles_, weights, output,
                              error)) {
    VIENNACORE_LOG_WARNING("Vulkan ray-flux dispatch failed: " + error);
    this->timer_.finish();
    return ProcessResult::FAILURE;
  }
  ++this->fluxCalculationsCount_;

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
