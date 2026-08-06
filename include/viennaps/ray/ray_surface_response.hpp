#pragma once

#include "../materials/psMaterialMap.hpp"
#include "../process/psSurfaceModel.hpp"

#include <vcPointData.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace viennaps::ray {

using namespace viennacore;

/// Surface response helper for the Vulkan ray-flux route.
///
/// This is a CPU-side contract class only: it documents and validates how the
/// P5 device route will map surface coverages, material IDs, and mask
/// materials to particle event weights. It intentionally reuses the existing
/// `SurfaceModel` and `MaterialMap` CPU contracts; no physics is duplicated.
template <typename NumericType>
class SurfaceResponse {
public:
  explicit SurfaceResponse(
      std::optional<Material> maskMaterial = std::nullopt)
      : maskMaterials_(
            maskMaterial ? std::vector<Material>{*maskMaterial}
                         : std::vector<Material>{}) {}

  explicit SurfaceResponse(const std::vector<Material> &maskMaterials)
      : maskMaterials_(maskMaterials) {}

  explicit SurfaceResponse(std::initializer_list<Material> maskMaterials)
      : maskMaterials_(maskMaterials) {}

  /// Returns the event weight for a ray hitting a point with the given
  /// material ID. Masked materials produce zero weight; unmasked points keep
  /// the incoming weight.
  [[nodiscard]] NumericType
  applyMaterialMask(NumericType rayWeight, int materialId) const {
    if (isMasked(materialId)) {
      return NumericType(0);
    }
    return rayWeight;
  }

  /// Predicate for masked points.
  [[nodiscard]] bool isMasked(int materialId) const {
    return !maskMaterials_.empty() &&
           MaterialMap::isMaterial(materialId, maskMaterials_);
  }

  /// Reads a coverage scalar from the surface model. Returns nullopt when the
  /// model has no coverages or the requested label is absent.
  [[nodiscard]] std::optional<NumericType>
  getCoverage(SmartPointer<SurfaceModel<NumericType>> surfaceModel,
              const std::string &label, std::size_t pointIndex) const {
    if (!surfaceModel || !surfaceModel->getCoverages()) {
      return std::nullopt;
    }
    const auto cov = surfaceModel->getCoverages()->getScalarData(label, true);
    if (!cov || pointIndex >= cov->size()) {
      return std::nullopt;
    }
    return (*cov)[pointIndex];
  }

private:
  std::vector<Material> maskMaterials_;
};

} // namespace viennaps::ray
