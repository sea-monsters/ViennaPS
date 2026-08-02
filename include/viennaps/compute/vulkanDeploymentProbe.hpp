#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "backendPolicy.hpp"
#include "capabilityProfileIO.hpp"
#include "deploymentProfile.hpp"

namespace viennaps::compute {

// The launcher receives a fully tokenized argv vector. Implementations must
// not route it through a shell. It is called synchronously on the deployment
// thread and should enforce the supplied parent-side watchdog.
using VulkanProbeProcessLauncher =
    std::function<bool(const std::vector<std::string> &,
                       std::chrono::milliseconds, std::string &)>;

struct VulkanDeploymentProbeOptions {
  std::string executable;
  // Absolute path selected by the deployment host for this one-shot output.
  std::filesystem::path outputPath;
  // When deployment detection selected a non-default physical device, forward
  // its Vulkan enumeration index to the strict child probe.
  std::optional<std::uint32_t> strictFp32DeviceIndex;
  std::chrono::milliseconds watchdog = std::chrono::milliseconds(65'000);
  VulkanProbeProcessLauncher launcher;
};

namespace detail {

inline bool launchVulkanProbeProcess(const std::vector<std::string> &argv,
                                     const std::chrono::milliseconds watchdog,
                                     std::string &error) {
  if (argv.empty() || argv.front().empty()) {
    error = "Vulkan probe executable path is empty.";
    return false;
  }
#if defined(_WIN32)
  std::wstring command;
  for (const auto &arg : argv) {
    if (!command.empty())
      command.push_back(L' ');
    const auto wide = std::filesystem::path(arg).wstring();
    command.push_back(L'\"');
    std::size_t backslashes = 0U;
    for (const auto c : wide) {
      if (c == L'\\') {
        ++backslashes;
      } else if (c == L'\"') {
        command.append(backslashes * 2U + 1U, L'\\');
        command.push_back(L'\"');
        backslashes = 0U;
      } else {
        command.append(backslashes, L'\\');
        command.push_back(c);
        backslashes = 0U;
      }
    }
    command.append(backslashes * 2U, L'\\');
    command.push_back(L'\"');
  }
  std::vector<wchar_t> commandLine(command.begin(), command.end());
  commandLine.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    error = "Vulkan probe process launch failed.";
    return false;
  }
  const auto waitResult = WaitForSingleObject(
      process.hProcess, static_cast<DWORD>(watchdog.count()));
  if (waitResult == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 124U);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    error = "Vulkan probe parent watchdog timeout.";
    return false;
  }
  if (waitResult != WAIT_OBJECT_0) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    error = "Vulkan probe process wait failed.";
    return false;
  }
  DWORD exitCode = 1U;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != 0U) {
    error = "Vulkan probe process exited with code " +
            std::to_string(static_cast<unsigned long>(exitCode)) + ".";
    return false;
  }
  return true;
#else
  const pid_t child = fork();
  if (child < 0) {
    error = "Vulkan probe process fork failed.";
    return false;
  }
  if (child == 0) {
    std::vector<char *> childArgv;
    childArgv.reserve(argv.size() + 1U);
    for (const auto &arg : argv)
      childArgv.push_back(const_cast<char *>(arg.c_str()));
    childArgv.push_back(nullptr);
    execvp(childArgv.front(), childArgv.data());
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + watchdog;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    if (waited < 0) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      error = "Vulkan probe process wait failed.";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (waitpid(child, &status, WNOHANG) == 0) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    error = "Vulkan probe parent watchdog timeout.";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = "Vulkan probe process exited nonzero.";
    return false;
  }
  return true;
#endif
}

inline bool
validateStrictDeploymentProfile(const HardwareFingerprint &expected,
                                const CapabilityProfileRecord &record,
                                std::string &error) {
  if (record.schemaVersion != kCapabilityProfileSchemaVersion) {
    error = "Vulkan probe profile schema version is unsupported.";
    return false;
  }
  if (!hasCompleteHardwareFingerprint(record.hardware)) {
    error = "Vulkan probe profile hardware fingerprint is incomplete.";
    return false;
  }
  if (!hasCompleteHardwareFingerprint(expected) ||
      !sameHardwareFingerprint(expected, record.hardware)) {
    error = "Vulkan probe profile hardware fingerprint does not match the "
            "deployment identity.";
    return false;
  }
  const auto &smoke = record.capabilityProfile.vulkanFp32NumericalSmoke;
  if (smoke.status != VulkanNumericalSmokeStatus::PASS ||
      smoke.contractId != kVulkanFp32NumericalSmokeContract ||
      smoke.caseCount == 0U || smoke.mismatchCount != 0U ||
      smoke.maxUlp != 0U ||
      smoke.watchdogMs != kVulkanFp32NumericalSmokeWatchdogMs ||
      smoke.elapsedMs > smoke.watchdogMs || !smoke.failureDiagnostic.empty()) {
    error = "Vulkan probe profile strict FP32 evidence is invalid.";
    return false;
  }
  return true;
}

} // namespace detail

[[nodiscard]] inline DeploymentProfileProbe
makeVulkanDeploymentProfileProbe(VulkanDeploymentProbeOptions options) {
  return [options = std::move(options)](const HardwareFingerprint &expected,
                                        CapabilityProfileRecord &out,
                                        std::string &error) mutable {
    error.clear();
    out = CapabilityProfileRecord{};
    if (options.executable.empty()) {
      error = "Vulkan probe executable path is empty.";
      return false;
    }
    if (!detail::hasCompleteHardwareFingerprint(expected)) {
      error = "Deployment identity hardware fingerprint is incomplete.";
      return false;
    }
    if (options.watchdog.count() <= 0 ||
#if defined(_WIN32)
        static_cast<unsigned long long>(options.watchdog.count()) >
            static_cast<unsigned long long>(std::numeric_limits<DWORD>::max())
#else
        false
#endif
    ) {
      error = "Vulkan probe watchdog must be positive and bounded.";
      return false;
    }
    std::error_code ec;
    if (options.outputPath.empty() || !options.outputPath.is_absolute()) {
      error = "Vulkan probe output path must be absolute.";
      return false;
    }
    const auto outputDirectory =
        std::filesystem::weakly_canonical(options.outputPath.parent_path(), ec);
    if (ec || outputDirectory.empty() ||
        !std::filesystem::is_directory(outputDirectory, ec) || ec) {
      error = "Vulkan probe output directory is unavailable.";
      return false;
    }
    const auto filename = options.outputPath.filename();
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.string().find_first_of("/\\") != std::string::npos) {
      error = "Vulkan probe output path contains an invalid filename.";
      return false;
    }
    for (const auto &component : options.outputPath.parent_path()) {
      if (component == "." || component == "..") {
        error = "Vulkan probe output path contains traversal components.";
        return false;
      }
    }
    const auto outputPath = outputDirectory / filename;
    if (options.outputPath.lexically_normal() !=
        outputPath.lexically_normal()) {
      error = "Vulkan probe output path escapes its selected directory.";
      return false;
    }
    const auto existingStatus = std::filesystem::symlink_status(outputPath, ec);
    const bool outputIsMissing =
        existingStatus.type() == std::filesystem::file_type::not_found ||
        ec == std::errc::no_such_file_or_directory;
    if (ec && !outputIsMissing) {
      error = "Vulkan probe output path cannot be inspected.";
      return false;
    }
    if (!outputIsMissing) {
      error = "Vulkan probe output path already exists.";
      return false;
    }
    ec.clear();
    std::vector<std::string> argv = {options.executable, "--strict-fp32-smoke",
                                     "--write-deployment-profile",
                                     outputPath.string(), "--validate-profile"};
    if (options.strictFp32DeviceIndex.has_value()) {
      argv.push_back("--strict-fp32-device-index");
      argv.push_back(std::to_string(*options.strictFp32DeviceIndex));
    }
    auto cleanup = [&]() {
      std::error_code cleanupError;
      std::filesystem::remove(outputPath, cleanupError);
    };
    bool launched = false;
    try {
      launched =
          options.launcher
              ? options.launcher(argv, options.watchdog, error)
              : detail::launchVulkanProbeProcess(argv, options.watchdog, error);
    } catch (const std::exception &exception) {
      error = std::string("Vulkan probe launcher threw: ") + exception.what();
      cleanup();
      return false;
    } catch (...) {
      error = "Vulkan probe launcher threw an unknown exception.";
      cleanup();
      return false;
    }
    if (!launched) {
      if (error.empty())
        error = "Vulkan probe process failed.";
      cleanup();
      return false;
    }
    const auto status = std::filesystem::symlink_status(outputPath, ec);
    if (ec || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
      error = "Vulkan probe output is not a regular file.";
      cleanup();
      return false;
    }
    const auto loaded =
        loadCapabilityProfileRecordFromFile(outputPath.string());
    cleanup();
    if (!loaded.ok) {
      error = "Vulkan probe profile could not be loaded: " + loaded.message;
      return false;
    }
    if (!detail::validateStrictDeploymentProfile(expected, loaded.record,
                                                 error))
      return false;
    out = loaded.record;
    return true;
  };
}

} // namespace viennaps::compute
