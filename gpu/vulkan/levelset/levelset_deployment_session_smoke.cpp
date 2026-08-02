// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_deployment_session.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using Session = viennaps::vulkan::levelset::LevelSetDeploymentSession<2>;
using Request = Session::Request;
using ComputeBackend = viennaps::compute::ComputeBackend;
using SelectionMode = viennaps::compute::SelectionMode;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "[levelset-deployment] " << message << '\n';
  return condition;
}

[[nodiscard]] Request cpuRequest() {
  Request request;
  request.selection.selectionMode = SelectionMode::MANUAL;
  request.selection.globalBackend = ComputeBackend::CPU;
  request.workload.stage = viennaps::compute::Stage::LEVEL_SET;
  request.workload.precision = viennaps::compute::Precision::FP32;
  request.workload.estimatedBytes = 1U;
  return request;
}

[[nodiscard]] bool setProbeEnvironment(const std::string &value) {
#ifdef _WIN32
  return _putenv_s("VIENNAPS_DEVICE_PROBE_PATH", value.c_str()) == 0;
#else
  return setenv("VIENNAPS_DEVICE_PROBE_PATH", value.c_str(), 1) == 0;
#endif
}

void clearProbeEnvironment() {
#ifdef _WIN32
  _putenv_s("VIENNAPS_DEVICE_PROBE_PATH", "");
#else
  unsetenv("VIENNAPS_DEVICE_PROBE_PATH");
#endif
}

struct ScopedProbeEnvironment {
  bool hadPreviousValue = false;
  std::string previousValue;

  [[nodiscard]] bool set(const std::string &value) {
#ifdef _WIN32
    char *current = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&current, &length, "VIENNAPS_DEVICE_PROBE_PATH") == 0 &&
        current != nullptr) {
      hadPreviousValue = true;
      previousValue = current;
      std::free(current);
    }
#else
    if (const char *current = std::getenv("VIENNAPS_DEVICE_PROBE_PATH")) {
      hadPreviousValue = true;
      previousValue = current;
    }
#endif
    return setProbeEnvironment(value);
  }

  ~ScopedProbeEnvironment() {
    if (hadPreviousValue) {
      (void)setProbeEnvironment(previousValue);
    } else {
      clearProbeEnvironment();
    }
  }
};

struct TemporaryDirectory {
  std::filesystem::path path;

  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

} // namespace

int main() {
  static_assert(std::is_same_v<
                decltype(std::declval<Session &>().configure(
                    std::declval<Session::ProcessType &>())),
                Session::Controller::Result>);

  std::string error;
  Session session;
  auto request = cpuRequest();
  const auto first = session.provision(request, {}, {}, error);
  if (!check(first.cached && first.manualCpuBypass && first.ok,
             "manual CPU provision did not bypass deployment"))
    return 1;
  if (!check(first.collectorInvoked == false && first.probeInvoked == false,
             "manual CPU provision invoked a deployment callback"))
    return 1;

  const auto repeatedCpu = session.provision(request, {}, {}, error);
  if (!check(repeatedCpu.cached && repeatedCpu.reused &&
                 repeatedCpu.manualCpuBypass && repeatedCpu.ok,
             "manual CPU provision did not reuse the deployment cache"))
    return 1;

  session.resetDeployment();
  if (!check(!session.isProvisioned(), "resetDeployment retained the cache"))
    return 1;

  const auto uniqueId =
      std::chrono::steady_clock::now().time_since_epoch().count();
  TemporaryDirectory temporary{
      std::filesystem::temp_directory_path() /
      ("viennaps-levelset-deployment-session-smoke-" +
       std::to_string(uniqueId))};
  std::error_code ec;
  std::filesystem::remove_all(temporary.path, ec);
  std::filesystem::create_directories(temporary.path, ec);
  if (ec)
    return 1;
  const auto profilePath = temporary.path / "profile.json";
  const auto explicitProbe = temporary.path / "explicit-probe.exe";
  const auto envProbe = temporary.path / "env-probe.exe";
  std::ofstream(explicitProbe).put('\n');
  std::ofstream(envProbe).put('\n');

  auto autoRequest = cpuRequest();
  autoRequest.selection.selectionMode = SelectionMode::AUTO;
  autoRequest.configuredProfilePath = profilePath.string();
  viennaps::compute::VulkanDeploymentBootstrapOptions options;
  options.profilePath = profilePath.string();
  options.probeExecutable = explicitProbe.string();
  options.transientOutputDirectory = temporary.path;
  std::size_t collectorCalls = 0U;
  options.collector = [&collectorCalls](
                          viennaps::compute::HardwareFingerprint &fingerprint,
                          std::uint32_t &, std::string &) {
    ++collectorCalls;
    fingerprint.deviceUuid = "device";
    fingerprint.driverUuid = "driver";
    fingerprint.vendorId = 1U;
    fingerprint.deviceId = 2U;
    fingerprint.deviceName = "fake";
    fingerprint.driverVersion = "1";
    fingerprint.driverDate = "unknown";
    return true;
  };
  std::string observedProbe;
  std::size_t launcherCalls = 0U;
  options.launcher = [&observedProbe, &launcherCalls](
                       const std::vector<std::string> &argv,
                       std::chrono::milliseconds,
                       std::string &launcherError) {
    ++launcherCalls;
    if (argv.empty() || argv.front().empty()) {
      launcherError = "missing probe executable";
      return false;
    }
    observedProbe = argv.front();
    return false;
  };

  session.resetDeployment();
  const auto envValue = envProbe.string();
  ScopedProbeEnvironment probeEnvironment;
  if (!probeEnvironment.set(envValue))
    return 1;
  const auto explicitResult = session.provision(autoRequest, options, {}, error);
  if (!check(explicitResult.cached && explicitResult.collectorInvoked &&
                 explicitResult.probeInvoked && !explicitResult.ok,
             "explicit probe-path provision did not reach the fake launcher"))
    return 1;
  if (!check(observedProbe == explicitProbe.string(),
             "configured probe path did not override the environment"))
    return 1;

  const auto collectorCallsAfterFirst = collectorCalls;
  const auto launcherCallsAfterFirst = launcherCalls;
  const auto cachedResult = session.provision(autoRequest, options, {}, error);
  if (!check(cachedResult.cached && cachedResult.reused && !cachedResult.ok,
             "repeated provision did not return the cached failure decision"))
    return 1;
  if (!check(collectorCalls == collectorCallsAfterFirst &&
                 launcherCalls == launcherCallsAfterFirst,
             "repeated provision reinvoked deployment callbacks"))
    return 1;

  options.probeExecutable.clear();
  session.resetDeployment();
  observedProbe.clear();
  const auto envResult = session.provision(autoRequest, options, {}, error);
  if (!check(envResult.cached && envResult.probeInvoked && !envResult.ok,
             "environment probe path was not resolved"))
    return 1;
  if (!check(session.probeExecutable() == envValue,
             "environment probe path did not win when option was empty"))
    return 1;
  if (!check(observedProbe == envValue,
             "environment probe path was not passed to the launcher"))
    return 1;

  return 0;
}
