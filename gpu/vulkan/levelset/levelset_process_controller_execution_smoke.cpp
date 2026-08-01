// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_process_controller.hpp"

#include <compute/capabilityProfileIO.hpp>
#include <process/psProcess.hpp>

#include <lsMakeGeometry.hpp>
#include <lsToSurfaceMesh.hpp>
#include <lsVelocityField.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Controller = viennaps::vulkan::levelset::LevelSetProcessController<2>;
using ComputeBackend = viennaps::compute::ComputeBackend;
using HardwareFingerprint = viennaps::compute::HardwareFingerprint;
using ManualSelectionConfig = viennaps::compute::ManualSelectionConfig;
using Precision = viennaps::compute::Precision;
using Stage = viennaps::compute::Stage;
using StageWorkload = viennaps::compute::StageWorkload;
using ComputeSession = viennaps::vulkan::runtime::ComputeSession;

class ConstantVelocityField final : public viennals::VelocityField<float> {
public:
  explicit ConstantVelocityField(const float speed) : speed_(speed) {}

  float getScalarVelocity(const std::array<float, 3> &, int,
                          const std::array<float, 3> &,
                          unsigned long) override {
    return speed_;
  }

private:
  float speed_ = 0.0F;
};

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < VK_UUID_SIZE; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

[[nodiscard]] bool collectHardware(HardwareFingerprint &fingerprint,
                                   std::string &error) {
  ComputeSession probeSession;
  if (!probeSession.initialize(error)) {
    return false;
  }

  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &idProperties;
  vkGetPhysicalDeviceProperties2(probeSession.selection().handle, &properties);

  fingerprint.deviceUuid = uuidToHex(idProperties.deviceUUID);
  fingerprint.driverUuid = uuidToHex(idProperties.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  fingerprint.driverDate = "unknown";
  return true;
}

[[nodiscard]] bool writeProfile(const std::filesystem::path &directory,
                                const HardwareFingerprint &hardware,
                                std::string &error) {
  std::error_code filesystemError;
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    error = filesystemError.message();
    return false;
  }

  viennaps::compute::CapabilityProfileRecord record;
  record.recordedAt = "2026-08-02T00:00:00Z";
  record.hardware = hardware;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      128ULL * 1024ULL * 1024ULL;
  const auto profilePath = directory / (hardware.deviceUuid + ".json");
  return viennaps::compute::writeCapabilityProfileRecordToFile(
      profilePath.string(), record, &error);
}

struct TempDirectoryGuard {
  std::filesystem::path path;
  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

[[nodiscard]] std::filesystem::path profileDirectory() {
  static std::uint64_t sequence = 0U;
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("viennaps-levelset-controller-execution-smoke-" +
          std::to_string(tick) + "-" + std::to_string(sequence++));
}

[[nodiscard]] viennaps::SmartPointer<viennaps::Domain<float, 2>>
makeProcessDomain() {
  constexpr int dimension = 2;
  constexpr viennahrle::CoordType extent = 4;
  constexpr viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * dimension] = {-extent, extent, -extent,
                                                 extent};
  viennals::BoundaryConditionEnum boundary[dimension] = {
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY,
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY};
  auto levelSet =
      viennals::SmartPointer<viennals::Domain<float, dimension>>::New(
          bounds, boundary, gridDelta);
  float origin[dimension] = {0.0F, 0.0F};
  viennals::MakeGeometry<float, dimension>(
      levelSet, viennals::SmartPointer<viennals::Sphere<float, dimension>>::New(
                    origin, 1.5F))
      .apply();

  auto domain = viennaps::SmartPointer<viennaps::Domain<float, 2>>::New();
  domain->insertNextLevelSetAsMaterial(levelSet, viennaps::Material::Undefined,
                                       false);
  return domain;
}

[[nodiscard]] std::vector<std::array<float, 3>> collectSurfaceNodes(
    const viennals::SmartPointer<viennals::Domain<float, 2>> &domain) {
  auto mesh = viennals::SmartPointer<viennals::Mesh<float>>::New();
  viennals::ToSurfaceMesh<float, 2>(domain, mesh).apply();
  auto nodes = mesh->nodes;
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

void assertNear(const std::vector<std::array<float, 3>> &actual,
                const std::vector<std::array<float, 3>> &expected) {
  VC_TEST_ASSERT(actual.size() == expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const float diff = std::fabs(actual[i][axis] - expected[i][axis]);
      const float scale =
          std::max(std::fabs(actual[i][axis]), std::fabs(expected[i][axis]));
      VC_TEST_ASSERT(diff <= 1.0e-6F ||
                     diff <= 1.0e-6F * std::max(1.0F, scale));
    }
  }
}

struct StepResult {
  double advectedTime = 0.0;
  std::vector<std::array<float, 3>> nodes;
};

[[nodiscard]] StepResult runOneStep(
    const viennals::SmartPointer<viennals::Domain<float, 2>> &domain,
    const viennaps::Process<float, 2>::LevelSetUpdateExecutor &executor = {}) {
  viennals::Advect<float, 2> advect;
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(
      viennals::SmartPointer<ConstantVelocityField>::New(-0.1F));
  advect.setSpatialScheme(
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(viennals::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(0.05);
  advect.setTimeStepRatio(0.4999);
  advect.setDissipationAlpha(0.0);
  advect.setCheckDissipation(false);
  advect.setSingleStep(true);
  advect.setLevelSetUpdateExecutor(executor);
  advect.apply();
  return {advect.getAdvectedTime(), collectSurfaceNodes(domain)};
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);

  HardwareFingerprint hardware;
  std::string error;
  if (!collectHardware(hardware, error)) {
    std::cout << "[LevelSetControllerExecution] SKIP: " << error << '\n';
    return EXIT_SUCCESS;
  }

  const TempDirectoryGuard profile{profileDirectory()};
  VC_TEST_ASSERT(writeProfile(profile.path, hardware, error));

  const StageWorkload workload{Stage::LEVEL_SET,
                               Precision::FP32,
                               4096U,
                               false,
                               viennaps::compute::RayMode::NONE,
                               true};

  auto cpuDomain = makeProcessDomain();
  const auto cpu = runOneStep(cpuDomain->getSurface());
  VC_TEST_ASSERT(!cpu.nodes.empty());

  viennaps::Process<float, 2> process;

  ManualSelectionConfig selection;
  const auto configured = [&] {
    Controller controller;
    return controller.configure(process, selection, hardware, workload, {},
                                VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
                                profile.path.string());
  }();
  VC_TEST_ASSERT(configured.ok);
  VC_TEST_ASSERT(configured.prepared);
  VC_TEST_ASSERT(configured.usingVulkan);
  VC_TEST_ASSERT(configured.selectedBackend == ComputeBackend::VULKAN);

  // The controller is gone here. The callback copied from Process must retain
  // its deployment state and execute one ViennaLS step successfully.
  const auto executor = process.getLevelSetUpdateExecutor();
  VC_TEST_ASSERT(static_cast<bool>(executor));
  using Advect = viennals::Advect<float, 2>;
  unsigned executorCalls = 0U;
  auto executorStatus = Advect::LevelSetUpdateStatus::ERROR;
  std::string executorError;
  const auto observedExecutor =
      [executor, &executorCalls, &executorStatus, &executorError](
          const Advect::LevelSetUpdateContext &context,
          Advect::LevelSetUpdateOutput &output, std::string &callbackError) {
        ++executorCalls;
        executorStatus = executor(context, output, callbackError);
        executorError = callbackError;
        return executorStatus;
      };
  auto vulkanDomain = makeProcessDomain();
  const auto vulkan = runOneStep(vulkanDomain->getSurface(), observedExecutor);
  VC_TEST_ASSERT(executorCalls == 1U);
  VC_TEST_ASSERT(executorStatus == Advect::LevelSetUpdateStatus::HANDLED);
  VC_TEST_ASSERT(executorError.empty());
  VC_TEST_ASSERT(vulkan.advectedTime > 0.0);
  VC_TEST_ASSERT(vulkan.advectedTime == cpu.advectedTime);
  assertNear(vulkan.nodes, cpu.nodes);

  std::cout
      << "[LevelSetControllerExecution] callback lifetime CPU/Vulkan PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
