#include <ray/ray_event_queue.hpp>
#include <ray/ray_reflection.hpp>
#include <ray/ray_roulette.hpp>
#include <ray/ray_surface_response.hpp>

#include <materials/psMaterialMap.hpp>

#include <vcPointData.hpp>
#include <vcRNG.hpp>
#include <vcTestAsserts.hpp>
#include <vcVectorType.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using namespace viennacore;
using namespace viennaps;

namespace {

constexpr float kPi = 3.14159265358979323846f;

class TestSurfaceModel : public SurfaceModel<float> {
public:
  explicit TestSurfaceModel(unsigned numPoints = 0) {
    if (numPoints > 0) {
      coverages = SmartPointer<PointData<float>>::New();
      coverages->insertNextScalarData(numPoints, 0.0f, "coverageA");
    }
  }

  void setCoverageValue(std::size_t index, float value) {
    auto data = coverages->getScalarData("coverageA");
    VC_TEST_ASSERT(data != nullptr);
    VC_TEST_ASSERT(index < data->size());
    (*data)[index] = value;
  }
};

void TestDiffuseNormalOrthogonality() {
  constexpr int N = 65536;
  RNG rng(42);
  Vec3D<float> normal{0.0f, 0.0f, 1.0f};
  float sumDot = 0.0f;
  for (int i = 0; i < N; ++i) {
    auto dir = viennaps::ray::reflectDiffuse<float, 3>(normal, rng);
    VC_TEST_ASSERT(std::abs(Norm(dir) - 1.0f) < 1e-5f);
    const float dot = DotProduct(dir, normal);
    VC_TEST_ASSERT(dot >= -1e-5f);
    sumDot += dot;
  }
  // The uniform-sphere-plus-normal normalization method used by ViennaRay
  // produces a cosine-weighted hemisphere with mean dot = 2/3.
  VC_TEST_ASSERT_ISCLOSE(sumDot / N, 2.0f / 3.0f, 0.02f);
}

void TestSpecularMirrorLaw() {
  Vec3D<float> normal{0.0f, 0.0f, 1.0f};
  Vec3D<float> incident{0.5f, 0.0f, -std::sqrt(0.75f)};
  Normalize(incident);
  auto reflected = viennaps::ray::reflectSpecular<float, 3>(incident, normal);
  VC_TEST_ASSERT_ISCLOSE(reflected[0], incident[0], 1e-5f);
  VC_TEST_ASSERT_ISCLOSE(reflected[1], incident[1], 1e-5f);
  VC_TEST_ASSERT_ISCLOSE(reflected[2], -incident[2], 1e-5f);
  VC_TEST_ASSERT_ISCLOSE(Norm(reflected), 1.0f, 1e-5f);
}

void TestConedCosineDegenerates() {
  RNG rng(123);
  Vec3D<float> normal{0.0f, 0.0f, 1.0f};
  Vec3D<float> incident{0.0f, 0.0f, -1.0f};

  // maxConeAngle <= 0 -> specular.
  auto specularLike =
      viennaps::ray::reflectConedCosine<float, 3>(incident, normal, rng, 0.0f);
  VC_TEST_ASSERT_ISCLOSE(specularLike[2], 1.0f, 1e-5f);

  // maxConeAngle >= pi/2 -> diffuse.
  auto diffuseLike =
      viennaps::ray::reflectConedCosine<float, 3>(incident, normal, rng, kPi);
  VC_TEST_ASSERT(diffuseLike[2] > -0.1f);
  VC_TEST_ASSERT_ISCLOSE(Norm(diffuseLike), 1.0f, 1e-5f);
}

void TestOrthonormalBasisProperties() {
  Vec3D<float> axis{1.0f, 2.0f, 3.0f};
  Normalize(axis);
  auto basis = viennaps::ray::getOrthonormalBasis(axis);
  for (int i = 0; i < 3; ++i) {
    VC_TEST_ASSERT_ISCLOSE(Norm(basis[i]), 1.0f, 1e-5f);
  }
  VC_TEST_ASSERT_ISCLOSE(std::abs(DotProduct(basis[0], basis[1])), 0.0f, 1e-5f);
  VC_TEST_ASSERT_ISCLOSE(std::abs(DotProduct(basis[0], basis[2])), 0.0f, 1e-5f);
  VC_TEST_ASSERT_ISCLOSE(std::abs(DotProduct(basis[1], basis[2])), 0.0f, 1e-5f);
}

void TestRouletteSurvivalRateMatchesSticking() {
  constexpr int N = 50000;
  RNG rng(7);
  const float initialWeight = 1.0f;
  const float sticking = 0.05f; // below lowerThreshold = 0.1 * initialWeight
  int survive = 0;
  for (int i = 0; i < N; ++i) {
    float weight = sticking;
    if (viennaps::ray::russianRoulette(weight, initialWeight, rng)) {
      ++survive;
    }
  }
  // Unbiased survival probability for weight w is w/renewWeight.
  const float expected = sticking / (0.3f * initialWeight);
  VC_TEST_ASSERT_ISCLOSE(static_cast<float>(survive) / N, expected, 0.02f);
}

void TestRouletteHighWeightAlwaysContinues() {
  RNG rng(9);
  float weight = 0.5f;
  VC_TEST_ASSERT(viennaps::ray::russianRoulette(weight, 1.0f, rng));
  VC_TEST_ASSERT(weight == 0.5f);
}

void TestRouletteZeroInitialWeightKills() {
  RNG rng(11);
  float weight = 0.1f;
  VC_TEST_ASSERT(!viennaps::ray::russianRoulette(weight, 0.0f, rng));
}

void TestEventQueueDeterministicOrdering() {
  viennaps::ray::EventQueue queue;
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 1.0f, 2, 1});
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 1.0f, 0, 5});
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 1.0f, 1, 0});
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 1.0f, 2, 0});

  VC_TEST_ASSERT(queue.size() == 4u);
  VC_TEST_ASSERT(queue.pop().particle == 0u);
  VC_TEST_ASSERT(queue.pop().particle == 1u);
  VC_TEST_ASSERT(queue.pop().particle == 2u);
  VC_TEST_ASSERT(queue.pop().bounce == 1u);
  VC_TEST_ASSERT(queue.empty());
}

void TestEventQueueTieBreakingByInsertionOrder() {
  viennaps::ray::EventQueue queue;
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 1.0f, 0, 0});
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 2.0f, 0, 0});
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 3.0f, 0, 0});

  VC_TEST_ASSERT(queue.pop().weight == 1.0f);
  VC_TEST_ASSERT(queue.pop().weight == 2.0f);
  VC_TEST_ASSERT(queue.pop().weight == 3.0f);

  queue.clear();
  queue.push(viennaps::ray::RayEvent{{0, 0, 0}, {0, 0, 1}, 4.0f, 0, 0});
  VC_TEST_ASSERT(queue.pop().weight == 4.0f);
}

void TestSurfaceResponseMaskMaterialZerosWeight() {
  viennaps::ray::SurfaceResponse<float> response(Material::Mask);
  VC_TEST_ASSERT_ISCLOSE(
      response.applyMaterialMask(1.0f, static_cast<int>(Material::Mask)),
      0.0f, 1e-6f);
  VC_TEST_ASSERT_ISCLOSE(
      response.applyMaterialMask(1.0f, static_cast<int>(Material::Si)),
      1.0f, 1e-6f);
}

void TestSurfaceResponseMultipleMaskMaterials() {
  viennaps::ray::SurfaceResponse<float> response(
      {Material::Mask, Material::SiO2});
  VC_TEST_ASSERT_ISCLOSE(
      response.applyMaterialMask(1.0f, static_cast<int>(Material::Mask)),
      0.0f, 1e-6f);
  VC_TEST_ASSERT_ISCLOSE(
      response.applyMaterialMask(1.0f, static_cast<int>(Material::SiO2)),
      0.0f, 1e-6f);
  VC_TEST_ASSERT_ISCLOSE(
      response.applyMaterialMask(1.0f, static_cast<int>(Material::Si)),
      1.0f, 1e-6f);
}

void TestSurfaceResponseUnmaskedPreservesWeight() {
  viennaps::ray::SurfaceResponse<float> response;
  VC_TEST_ASSERT_ISCLOSE(response.applyMaterialMask(0.75f, 0), 0.75f, 1e-6f);
}

void TestSurfaceResponseCoverageAccess() {
  viennaps::ray::SurfaceResponse<float> response;

  // Missing model returns nullopt.
  VC_TEST_ASSERT(!response.getCoverage(nullptr, "coverageA", 0));

  // Model without coverages returns nullopt.
  auto emptyModel = SmartPointer<TestSurfaceModel>::New(0);
  VC_TEST_ASSERT(!response.getCoverage(emptyModel, "coverageA", 0));

  // Present coverage at valid index.
  auto model = SmartPointer<TestSurfaceModel>::New(4);
  model->setCoverageValue(2, 0.75f);
  auto value = response.getCoverage(model, "coverageA", 2);
  VC_TEST_ASSERT(value.has_value());
  VC_TEST_ASSERT_ISCLOSE(*value, 0.75f, 1e-6f);

  // Missing label returns nullopt.
  VC_TEST_ASSERT(!response.getCoverage(model, "coverageB", 2));

  // Out-of-range index returns nullopt.
  VC_TEST_ASSERT(!response.getCoverage(model, "coverageA", 10));
}

} // namespace

int main() {
  std::cerr << "TestDiffuseNormalOrthogonality\n";
  TestDiffuseNormalOrthogonality();
  std::cerr << "TestSpecularMirrorLaw\n";
  TestSpecularMirrorLaw();
  std::cerr << "TestConedCosineDegenerates\n";
  TestConedCosineDegenerates();
  std::cerr << "TestOrthonormalBasisProperties\n";
  TestOrthonormalBasisProperties();
  std::cerr << "TestRouletteSurvivalRateMatchesSticking\n";
  TestRouletteSurvivalRateMatchesSticking();
  std::cerr << "TestRouletteHighWeightAlwaysContinues\n";
  TestRouletteHighWeightAlwaysContinues();
  std::cerr << "TestRouletteZeroInitialWeightKills\n";
  TestRouletteZeroInitialWeightKills();
  std::cerr << "TestEventQueueDeterministicOrdering\n";
  TestEventQueueDeterministicOrdering();
  std::cerr << "TestEventQueueTieBreakingByInsertionOrder\n";
  TestEventQueueTieBreakingByInsertionOrder();
  std::cerr << "TestSurfaceResponseMaskMaterialZerosWeight\n";
  TestSurfaceResponseMaskMaterialZerosWeight();
  std::cerr << "TestSurfaceResponseMultipleMaskMaterials\n";
  TestSurfaceResponseMultipleMaskMaterials();
  std::cerr << "TestSurfaceResponseUnmaskedPreservesWeight\n";
  TestSurfaceResponseUnmaskedPreservesWeight();
  std::cerr << "TestSurfaceResponseCoverageAccess\n";
  TestSurfaceResponseCoverageAccess();

  return 0;
}
