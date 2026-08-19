#include <rayTraceTriangle.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "p5_trace_observer.hpp"

namespace {
using T = float;
using Vec = viennacore::Vec3D<T>;

class Source final : public viennaray::Source<T> {
public:
  explicit Source(T weight) : weight_(weight) {}
  std::array<Vec, 2> getOriginAndDirection(std::size_t,
                                           viennaray::RNG &) const override {
    return {Vec{0.0F, 0.0F, 0.0F}, Vec{0.0F, 0.0F, 1.0F}};
  }
  std::size_t getNumPoints() const override { return 1U; }
  T getSourceArea() const override { return 1.0F; }
  T getInitialRayWeight(std::size_t) const override { return weight_; }

private:
  T weight_;
};

class Particle final : public viennaray::Particle<Particle, T> {
public:
  explicit Particle(T sticking) : sticking_(sticking) {}
  void surfaceCollision(T weight, const Vec &, const Vec &, unsigned int prim,
                        int, viennacore::PointData<T> &local,
                        const viennacore::PointData<T> *,
                        viennaray::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    (void)draw(rng);
    local.addToScalarData(0U, prim, weight);
  }
  std::pair<T, Vec> surfaceReflection(T, const Vec &, const Vec &, unsigned int,
                                      int, const viennacore::PointData<T> *,
                                      viennaray::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    (void)draw(rng);
    return {sticking_, Vec{0.0F, 0.0F, 1.0F}};
  }
  void initNew(viennaray::RNG &) final {}
  T getSourceDistributionPower() const final { return 1.0F; }
  std::vector<std::string> getLocalDataLabels() const final { return {"flux"}; }

private:
  T sticking_;
};

bool runCase(const char *path, T weight, T sticking) {
#ifdef _OPENMP
  omp_set_num_threads(1);
#endif
  viennaray::TraceTriangle<T, 3> tracer;
  viennaray::BoundaryCondition boundaries[3]{
      viennaray::BoundaryCondition::IGNORE_BOUNDARY,
      viennaray::BoundaryCondition::IGNORE_BOUNDARY,
      viennaray::BoundaryCondition::IGNORE_BOUNDARY};
  tracer.setBoundaryConditions(boundaries);
  tracer.setSourceDirection(viennaray::TraceDirection::POS_Z);
  tracer.setNumberOfRaysFixed(1U);
  // Three front-face triangles expose two continuations then the third-hit
  // reflection-limit termination. Callback draws stay in TraceKernel's RNG.
  tracer.setMaxReflections(2U);
  tracer.setMaxBoundaryHits(0U);
  tracer.setUseRandomSeeds(false);
  tracer.setRngSeed(1U);
  tracer.setSource(std::make_shared<Source>(weight));
  tracer.setParticleType(std::make_unique<Particle>(sticking));

  std::vector<Vec> points;
  std::vector<viennacore::Vec3D<unsigned>> triangles;
  for (unsigned z = 1U; z <= 3U; ++z) {
    const auto base = static_cast<unsigned>(points.size());
    points.push_back({-10.0F, -10.0F, static_cast<T>(z)});
    points.push_back({10.0F, -10.0F, static_cast<T>(z)});
    points.push_back({-10.0F, 10.0F, static_cast<T>(z)});
    triangles.push_back({base, base + 2U, base + 1U});
  }
  tracer.setGeometry(points, triangles, 1.0F);

  viennaps_p5_trace_observer::Sink sink(path);
  if (!sink.good()) return false;
  viennaps_p5_trace_observer::Scope scope(sink);
  tracer.apply();
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: trace_emitter output\n";
    return 2;
  }
  std::remove(argv[1]);
  if (!runCase(argv[1], 1.0F, 0.0F) ||
      !runCase(argv[1], 0.05F, 0.95F)) {
    std::cerr << "trace emitter FAIL\n";
    return 1;
  }
#ifdef VIENNAPS_P5_REFERENCE_EMITTER
  std::cout << "trace emitter PASS root=D:/Codex_lib/code_reference/ViennaPS\n";
#else
  std::cout << "trace emitter PASS root=D:/Codex_lib/ViennaPSMod\n";
#endif
  return 0;
}
