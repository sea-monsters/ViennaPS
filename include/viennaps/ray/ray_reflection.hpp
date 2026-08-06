#pragma once

#include <array>

#include <rayReflection.hpp>
#include <rayUtil.hpp>
#include <vcRNG.hpp>
#include <vcVectorType.hpp>

namespace viennaps::ray {

using namespace viennacore;

/// CPU-side reflection kernels that reuse ViennaRay's authoritative functions.
/// These wrappers are thin because the physics must stay identical to the CPU
/// path in `rayTraceKernel.hpp`; they exist so the P5 Vulkan route can include
/// the same contract without depending on ViennaRay internals directly.

template <typename NumericType, int D = 3>
[[nodiscard]] Vec3D<NumericType>
reflectSpecular(const Vec3D<NumericType> &rayDir,
                const Vec3D<NumericType> &geomNormal) {
  return viennaray::ReflectionSpecular<NumericType, D>(rayDir, geomNormal);
}

template <typename NumericType, int D = 3>
[[nodiscard]] Vec3D<NumericType>
reflectDiffuse(const Vec3D<NumericType> &geomNormal, RNG &rng) {
  return viennaray::ReflectionDiffuse<NumericType, D>(geomNormal, rng);
}

template <typename NumericType, int D = 3>
[[nodiscard]] Vec3D<NumericType>
reflectConedCosine(const Vec3D<NumericType> &rayDir,
                   const Vec3D<NumericType> &geomNormal, RNG &rng,
                   const NumericType maxConeAngle) {
  return viennaray::ReflectionConedCosine<NumericType, D>(rayDir, geomNormal,
                                                          rng, maxConeAngle);
}

/// Returns a stable orthonormal basis for the given vector. Reuses
/// `rayInternal::getOrthonormalBasis`.
template <typename NumericType>
[[nodiscard]] std::array<Vec3D<NumericType>, 3>
getOrthonormalBasis(const Vec3D<NumericType> &vec) {
  return rayInternal::getOrthonormalBasis<NumericType>(vec);
}

} // namespace viennaps::ray
