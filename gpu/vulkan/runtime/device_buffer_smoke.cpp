// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "compute_session.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using viennaps::vulkan::runtime::ComputeSession;
using viennaps::vulkan::runtime::DeviceBuffer;

bool expectFailure(const bool result, const std::string &error,
                  const char *label) {
  if (result || error.empty()) {
    std::cerr << label << " unexpectedly succeeded." << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  ComputeSession session{};
  std::string error;
  if (!session.initialize(error)) {
    std::cerr << "session initialization failed: " << error << '\n';
    return 1;
  }

  static_assert(!std::is_copy_constructible_v<DeviceBuffer>);
  static_assert(!std::is_copy_assignable_v<DeviceBuffer>);

  DeviceBuffer source{};
  DeviceBuffer destination{};
  if (!source.create(session.device(), 4u * sizeof(std::uint32_t), error) ||
      !destination.create(session.device(), 4u * sizeof(std::uint32_t),
                          error)) {
    std::cerr << "device buffer creation failed: " << error << '\n';
    return 1;
  }

  const std::array<std::uint32_t, 4u> expected{11u, 23u, 47u, 89u};
  if (!source.upload(session, expected.data(), sizeof(expected), 0u, error)) {
    std::cerr << "device buffer upload failed: " << error << '\n';
    return 1;
  }
  if (!source.copyTo(session, destination, sizeof(expected), 0u, 0u, error)) {
    std::cerr << "device buffer copy failed: " << error << '\n';
    return 1;
  }

  DeviceBuffer moved{std::move(source)};
  if (source.isValid() || !moved.isValid() ||
      moved.size() != sizeof(expected)) {
    std::cerr << "device buffer move state is invalid." << '\n';
    return 1;
  }
  source = std::move(moved);

  std::array<std::uint32_t, 4u> actual{};
  if (!destination.download(session, actual.data(), sizeof(actual), 0u,
                            error) || actual != expected) {
    std::cerr << "device buffer download mismatch: " << error << '\n';
    return 1;
  }

  DeviceBuffer empty{};
  error.clear();
  if (!expectFailure(empty.create(session.device(), 0u, error), error,
                     "zero-size create")) {
    return 1;
  }
  error.clear();
  if (!expectFailure(source.create(session.device(), sizeof(expected) + 4u,
                                   error),
                     error, "different-size create")) {
    return 1;
  }
  error.clear();
  if (!expectFailure(source.upload(session, expected.data(), 0u, 0u, error),
                     error, "zero-size upload")) {
    return 1;
  }
  error.clear();
  if (!expectFailure(source.upload(session, nullptr, sizeof(expected), 0u,
                                   error),
                     error, "null upload")) {
    return 1;
  }
  error.clear();
  if (!expectFailure(source.download(session, actual.data(), sizeof(actual),
                                     sizeof(std::uint32_t), error),
                     error, "out-of-range download")) {
    return 1;
  }
  error.clear();
  if (!expectFailure(source.copyTo(session, destination, sizeof(expected), 0u,
                                   sizeof(std::uint32_t), error),
                     error, "out-of-range copy")) {
    return 1;
  }

  DeviceBuffer uninitialized{};
  error.clear();
  if (!expectFailure(uninitialized.download(session, actual.data(),
                                            sizeof(actual), 0u, error),
                     error, "uninitialized download")) {
    return 1;
  }

  ComputeSession foreignSession{};
  error.clear();
  if (!foreignSession.initialize(error)) {
    std::cerr << "foreign session initialization failed: " << error << '\n';
    return 1;
  }
  error.clear();
  if (!expectFailure(source.upload(foreignSession, expected.data(),
                                   sizeof(expected), 0u, error),
                     error, "session mismatch upload")) {
    return 1;
  }

  std::cout << "DeviceBuffer smoke passed." << '\n';
  return 0;
}
