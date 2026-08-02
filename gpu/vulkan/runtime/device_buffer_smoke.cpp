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
  const auto initialGeneration = session.generation();
  if (initialGeneration == 0u) {
    std::cerr << "session generation is zero.\n";
    return 1;
  }
  error.clear();
  if (!session.initialize(error) || session.generation() != initialGeneration) {
    std::cerr << "idempotent session initialization changed generation.\n";
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

  DeviceBuffer generationBuffer{};
  if (!generationBuffer.create(session, sizeof(std::uint32_t), error) ||
      generationBuffer.ownerSessionGeneration() != initialGeneration) {
    std::cerr << "generation-aware buffer creation failed: " << error << '\n';
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

  error.clear();
  if (!expectFailure(generationBuffer.upload(foreignSession, expected.data(),
                                             sizeof(std::uint32_t), 0u, error),
                     error, "generation mismatch upload")) {
    return 1;
  }

  ComputeSession movedSession{std::move(session)};
  if (session.generation() != 0u ||
      movedSession.generation() != initialGeneration ||
      !generationBuffer.upload(movedSession, expected.data(),
                                sizeof(std::uint32_t), 0u, error)) {
    std::cerr << "session move did not preserve generation-aware buffer.\n";
    return 1;
  }

  ComputeSession assignedSession{};
  assignedSession = std::move(movedSession);
  if (movedSession.generation() != 0u ||
      assignedSession.generation() != initialGeneration ||
      !generationBuffer.upload(assignedSession, expected.data(),
                               sizeof(std::uint32_t), 0u, error)) {
    std::cerr << "session move assignment lost generation-aware buffer.\n";
    return 1;
  }

  ComputeSession staleSession{};
  error.clear();
  if (!staleSession.initialize(error)) {
    std::cerr << "stale session initialization failed: " << error << '\n';
    return 1;
  }
  DeviceBuffer staleBuffer{};
  const auto staleGeneration = staleSession.generation();
  if (!staleBuffer.create(staleSession, sizeof(std::uint32_t), error) ||
      staleGeneration == 0u) {
    std::cerr << "stale buffer creation failed: " << error << '\n';
    return 1;
  }
  staleSession.reset();
  error.clear();
  if (!expectFailure(staleBuffer.upload(staleSession, expected.data(),
                                        sizeof(std::uint32_t), 0u, error),
                     error, "reset session upload")) {
    return 1;
  }
  if (!staleSession.initialize(error) ||
      staleSession.generation() == staleGeneration) {
    std::cerr << "session reinitialize did not get a new generation.\n";
    return 1;
  }
  error.clear();
  if (!expectFailure(staleBuffer.upload(staleSession, expected.data(),
                                        sizeof(std::uint32_t), 0u, error),
                     error, "reinitialized session upload")) {
    return 1;
  }
  staleBuffer.reset();
  generationBuffer.reset();
  source.reset();
  destination.reset();
  assignedSession.reset();
  staleSession.reset();

  std::cout << "DeviceBuffer smoke passed." << '\n';
  return 0;
}
