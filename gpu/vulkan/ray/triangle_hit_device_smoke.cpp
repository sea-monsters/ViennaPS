// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "triangle_hit_device.hpp"

#include <bit>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH                           \
  "triangle_hit_device.comp.spv"
#endif
using namespace viennaps::vulkan::ray;
int main() {
  std::string error;
  viennaps::vulkan::runtime::ComputeSession session;
  if (!session.initialize(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  DeviceTriangleHitPrimitive p;
  if (!p.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                    error)) {
    std::cerr << error << '\n';
    return 1;
  }
  const Triangle first{{-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
  const Triangle second{{-1, -1, 4}, {1, -1, 4}, {0, 1, 4}};
  const std::vector<Triangle> triangles{first, second, first};
  std::vector<Ray> rays(5);
  rays[0].origin = {0, 0, 0};
  rays[0].direction = {0, 0, 1};
  rays[0].tMax = 10;
  rays[1].origin = {2, 0, 0};
  rays[1].direction = {0, 0, 1};
  rays[1].tMax = 10;
  rays[2] = rays[0];
  rays[2].tMin = 3;
  rays[3] = rays[0];
  rays[3].tMin = 2;
  rays[3].tMax = 2;
  rays[4] = rays[0];
  rays[4].origin[0] = 0.5F;
  viennaps::vulkan::runtime::DeviceBuffer o, d, t, h;
  if (!p.createRayBuffers(rays.size(), o, d, error) ||
      !p.createTriangleBuffer(triangles.size(), t, error) ||
      !p.createHitBuffer(rays.size(), h, error) ||
      !p.uploadRays(rays, o, d, error) ||
      !p.uploadTriangles(triangles, t, error) ||
      !p.dispatch(o, d, t, rays.size(), triangles.size(), h, rays.size(),
                  error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<TriangleHit> gpu(rays.size()), cpu(rays.size());
  if (!p.downloadHits(gpu.size(), h, gpu, error) ||
      !intersectCpu(rays, triangles, cpu, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < gpu.size(); ++i)
    if (std::bit_cast<std::uint32_t>(gpu[i].t) !=
            std::bit_cast<std::uint32_t>(cpu[i].t) ||
        gpu[i].triangleIndex != cpu[i].triangleIndex ||
        std::bit_cast<std::uint32_t>(gpu[i].u) !=
            std::bit_cast<std::uint32_t>(cpu[i].u) ||
        std::bit_cast<std::uint32_t>(gpu[i].v) !=
            std::bit_cast<std::uint32_t>(cpu[i].v)) {
      std::cerr << "GPU/CPU mismatch\n";
      return 1;
    }
  std::vector<TriangleHit> sentinel(rays.size());
  for (std::size_t i = 0; i < sentinel.size(); ++i)
    sentinel[i] = {std::bit_cast<float>(0x80000000U + static_cast<unsigned>(i)),
                   0x12340000U + static_cast<unsigned>(i),
                   std::bit_cast<float>(0x80000000U), 0.25F};
  if (!h.upload(session, sentinel.data(), sentinel.size() * sizeof(TriangleHit),
                0, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  const auto unchanged = [&]() {
    std::vector<TriangleHit> now(sentinel.size());
    return p.downloadHits(now.size(), h, now, error) &&
           std::memcmp(now.data(), sentinel.data(),
                       now.size() * sizeof(TriangleHit)) == 0;
  };
  std::string bad;
  if (p.dispatch(o, d, t, rays.size(), triangles.size(), o, rays.size(), bad) ||
      bad.empty() || !unchanged()) {
    std::cerr << "alias guard failed\n";
    return 1;
  }
  viennaps::vulkan::runtime::ComputeSession foreignSession;
  viennaps::vulkan::runtime::DeviceBuffer foreignOrigins;
  if (!foreignSession.initialize(error) ||
      !foreignOrigins.create(foreignSession, o.size(), error) ||
      p.dispatch(foreignOrigins, d, t, rays.size(), triangles.size(), h,
                 rays.size(), bad) ||
      bad.empty() || !unchanged()) {
    std::cerr << "foreign-session guard failed\n";
    return 1;
  }
  if (!p.dispatch(o, d, t, 0U, triangles.size(), h, rays.size(), bad) ||
      !unchanged()) {
    std::cerr << "N=0 transaction failed\n";
    return 1;
  }
  if (p.dispatch(o, d, t, rays.size(), triangles.size(), h, 0U, bad) ||
      bad.empty() || !unchanged()) {
    std::cerr << "capacity guard failed\n";
    return 1;
  }
  std::cout << "triangle hit device Vulkan dispatch PASS\n";
  return 0;
}
