#pragma once

#include "../materials/psMaterialMap.hpp"
#include "../process/psProcessModel.hpp"
#include "psWetEtchingVelocityExecutor.hpp"

#include <vcVectorType.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace viennaps {

using namespace viennacore;

namespace impl {

template <class NumericType, int D>
class WetEtchingVelocityField : public VelocityField<NumericType, D> {
  static Vec3D<NumericType> ScaleImpl(const NumericType pF,
                                      const Vec3D<NumericType> &pT) {
    return Vec3D<NumericType>{pF * pT[0], pF * pT[1], pF * pT[2]};
  }

  Vec3D<Vec3D<NumericType>> directions;
  const NumericType r100;
  const NumericType r110;
  const NumericType r111;
  const NumericType r311;
  const std::vector<std::pair<Material, NumericType>> &materials;
  std::array<NumericType, 3> direction100Input{};
  std::array<NumericType, 3> direction010Input{};
  WetEtchVelocityExecutor<NumericType> executor_{};

public:
  WetEtchingVelocityField(
      const Vec3D<NumericType> &direction100,
      const Vec3D<NumericType> &direction010, const NumericType passedR100,
      const NumericType passedR110, const NumericType passedR111,
      const NumericType passedR311,
      const std::vector<std::pair<Material, NumericType>> &passedmaterials)
      : r100(passedR100), r110(passedR110), r111(passedR111), r311(passedR311),
        materials(passedmaterials), direction100Input{direction100[0],
                                                       direction100[1],
                                                       direction100[2]},
        direction010Input{direction010[0], direction010[1], direction010[2]} {

    directions[0] = Normalize(direction100);
    directions[1] = Normalize(direction010);

    directions[1] =
        directions[1] -
        ScaleImpl(DotProduct(directions[0], directions[1]), directions[0]);
    directions[2] = CrossProduct(directions[0], directions[1]);
  }

  void setExecutor(WetEtchVelocityExecutor<NumericType> executor) {
    executor_ = std::move(executor);
  }

  NumericType getScalarVelocity(const Vec3D<NumericType> &coordinate,
                                int material, const Vec3D<NumericType> &nv,
                                unsigned long pointID) override {
    for (auto const &etchingMaterial : materials) {
      if (MaterialMap::isMaterial(material, etchingMaterial.first)) {
        if (std::abs(Norm(nv) - 1.) > 1e-4)
          return 0.;

        Vec3D<NumericType> normalVector;
        normalVector[0] = nv[0];
        normalVector[1] = nv[1];
        if (D == 3) {
          normalVector[2] = nv[2];
        } else {
          normalVector[2] = 0;
        }
        Normalize(normalVector);

        Vec3D<NumericType> N;
        for (int i = 0; i < 3; i++) {
          N[i] = std::fabs(DotProduct(directions[i], normalVector));
        }
        std::sort(N.begin(), N.end(), std::greater<NumericType>());

        NumericType velocity;
        if (DotProduct(N, Vec3D<NumericType>{-1., 1., 2.}) < 0) {
          velocity = (r100 * (N[0] - N[1] - 2 * N[2]) + r110 * (N[1] - N[2]) +
                      3 * r311 * N[2]) /
                     N[0];
        } else {
          velocity = (r111 * ((N[1] - N[0]) * 0.5 + N[2]) +
                      r110 * (N[1] - N[2]) + 1.5 * r311 * (N[0] - N[1])) /
                     N[0];
        }

        const NumericType cpuVelocity = velocity * etchingMaterial.second;
        if (!executor_ || !std::isfinite(static_cast<double>(cpuVelocity)))
          return cpuVelocity;

        // The CPU formula above is authoritative.  The optional executor is
        // only a numeric candidate for this already selected point; a failed
        // or incomplete transaction falls back to the exact CPU value.
        const std::array<NumericType, 3> coordinateValue{
            coordinate[0], coordinate[1], coordinate[2]};
        const std::array<NumericType, 3> normalValue{nv[0], nv[1], nv[2]};
        const std::int32_t materialId = static_cast<std::int32_t>(
            etchingMaterial.first.legacyId());
        std::vector<WetEtchMaterialRate<NumericType>> rateTable;
        rateTable.reserve(materials.size());
        for (const auto &entry : materials) {
          rateTable.push_back({static_cast<std::int32_t>(entry.first.legacyId()),
                               entry.second});
        }
        const std::array<NumericType, 1> cpuOracle{cpuVelocity};
        std::array<NumericType, 1> output{cpuVelocity};
        WetEtchVelocityParameters<NumericType> parameters;
        parameters.direction100 = direction100Input;
        parameters.direction010 = direction010Input;
        parameters.r100 = r100;
        parameters.r110 = r110;
        parameters.r111 = r111;
        parameters.r311 = r311;
        parameters.materialRates = rateTable;
        WetEtchVelocityWork<NumericType> work;
        work.coordinates = std::span<const std::array<NumericType, 3>>(
            &coordinateValue, 1U);
        work.normals =
            std::span<const std::array<NumericType, 3>>(&normalValue, 1U);
        work.materialIds = std::span<const std::int32_t>(&materialId, 1U);
        work.cpuOracle = std::span<const NumericType>(cpuOracle.data(), 1U);
        work.output = std::span<NumericType>(output.data(), 1U);
        work.parameters = parameters;
        std::string error;
        if (executor_(work, error) && work.complete && work.writtenCount == 1U &&
            std::isfinite(static_cast<double>(output[0])))
          return output[0];
        return cpuVelocity;
      }
    }

    // not an etching material
    return 0.;
  }
};
} // namespace impl

// Model for a wet etching process.
template <typename NumericType, int D>
class WetEtching : public ProcessModelCPU<NumericType, D> {
public:
  using VelocityExecutor = WetEtchVelocityExecutor<NumericType>;

  // The constructor expects the materials where etching is allowed including
  // the corresponding rates.
  WetEtching(const std::vector<std::pair<Material, NumericType>> materialRates)
      : materials(materialRates) {
    if constexpr (D == 2) {
      direction100 = Vec3D<NumericType>{0., 1., 0.};
      direction010 = Vec3D<NumericType>{1., 0., -1.};
    } else {
      direction100 = Vec3D<NumericType>{0.707106781187, 0.707106781187, 0};
      direction010 = Vec3D<NumericType>{-0.707106781187, 0.707106781187, 0.};
    }
    initialize();
  }

  WetEtching(const Vec3D<NumericType> &passedDir100,
             const Vec3D<NumericType> &passedDir010, NumericType rate100,
             NumericType rate110, NumericType rate111, NumericType rate311,
             const std::vector<std::pair<Material, NumericType>> materialRates)
      : direction100(passedDir100), direction010(passedDir010), r100(rate100),
        r110(rate110), r111(rate111), r311(rate311), materials(materialRates) {
    initialize();
  }

  /// Installs an optional numeric executor for the crystal velocity operation.
  /// CPU formula evaluation and Process/LevelSet ordering remain authoritative;
  /// an executor failure is intentionally handled as CPU fallback.
  void setVelocityExecutor(VelocityExecutor executor) {
    velocityExecutor_ = std::move(executor);
    auto field = std::dynamic_pointer_cast<
        impl::WetEtchingVelocityField<NumericType, D>>(this->getVelocityField());
    if (field)
      field->setExecutor(velocityExecutor_);
  }

  void clearVelocityExecutor() { setVelocityExecutor({}); }

  [[nodiscard]] bool hasVelocityExecutor() const {
    return static_cast<bool>(velocityExecutor_);
  }

private:
  void initialize() {
    // default surface model
    auto surfModel = SmartPointer<SurfaceModel<NumericType>>::New();

    // velocity field
    auto velField =
        SmartPointer<impl::WetEtchingVelocityField<NumericType, D>>::New(
            direction100, direction010, r100, r110, r111, r311, materials);
    velField->setExecutor(velocityExecutor_);

    this->setSurfaceModel(surfModel);
    this->setVelocityField(velField);
    this->setProcessName("WetEtching");

    // store process data
    processMetaData["r100"] = {r100};
    processMetaData["r110"] = {r110};
    processMetaData["r111"] = {r111};
    processMetaData["r311"] = {r311};
    processMetaData["Direction100"] = {direction100[0], direction100[1],
                                       direction100[2]};
    processMetaData["Direction010"] = {direction010[0], direction010[1],
                                       direction010[2]};
    for (const auto &material : materials) {
      processMetaData[MaterialMap::toString(material.first) + " Rate"] =
          std::vector<double>{material.second};
    }
  }

  // crystal surface direction
  Vec3D<NumericType> direction100;
  Vec3D<NumericType> direction010;

  // rates for crystal directions in um / s
  NumericType r100 = 0.0166666666667;
  NumericType r110 = 0.0309166666667;
  NumericType r111 = 0.000121666666667;
  NumericType r311 = 0.0300166666667;

  std::vector<std::pair<Material, NumericType>> materials;
  VelocityExecutor velocityExecutor_{};
  using ProcessModelCPU<NumericType, D>::processMetaData;
};

PS_PRECOMPILE_PRECISION_DIMENSION(WetEtching)

} // namespace viennaps
