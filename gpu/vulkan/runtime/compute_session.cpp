// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Reusable compute-session wrapper: one instance + one logical device, with
// optional manual device selection and one shared command pool.

#include "compute_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint32_t kUnsetManualDeviceIndex =
    std::numeric_limits<std::uint32_t>::max();

std::mutex &generationMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_set<std::uint64_t> &liveGenerations() {
  static std::unordered_set<std::uint64_t> generations;
  return generations;
}

bool registerGeneration(const std::uint64_t generation) {
  try {
    std::lock_guard lock(generationMutex());
    return liveGenerations().insert(generation).second;
  } catch (...) {
    return false;
  }
}

void unregisterGeneration(const std::uint64_t generation) {
  if (generation == 0u) {
    return;
  }
  std::lock_guard lock(generationMutex());
  liveGenerations().erase(generation);
}

std::uint64_t allocateGeneration() {
  static std::atomic<std::uint64_t> next{1u};
  std::uint64_t generation = next.fetch_add(1u, std::memory_order_relaxed);
  while (generation == 0u) {
    generation = next.fetch_add(1u, std::memory_order_relaxed);
  }
  return generation;
}

[[nodiscard]] bool isHexDigit(const char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] std::uint8_t hexToNibble(const char ch) {
  if (ch >= '0' && ch <= '9') {
    return static_cast<std::uint8_t>(ch - '0');
  }
  if (ch >= 'a' && ch <= 'f') {
    return static_cast<std::uint8_t>(ch - 'a' + 10);
  }
  return static_cast<std::uint8_t>(ch - 'A' + 10);
}

[[nodiscard]] bool parseUuid(std::string_view uuid,
                             std::array<std::uint8_t, VK_UUID_SIZE> &bytes) {
  bytes.fill(0);
  std::array<std::uint8_t, VK_UUID_SIZE> parsed{};
  std::size_t byteCount = 0;
  bool half = false;
  char first = '\0';
  for (const char ch : uuid) {
    if (ch == '-' || ch == '{' || ch == '}') {
      continue;
    }
    if (!isHexDigit(ch)) {
      return false;
    }
    if (byteCount >= VK_UUID_SIZE) {
      return false;
    }
    if (!half) {
      first = ch;
      half = true;
      continue;
    }
    const std::size_t offset = byteCount++;
    parsed[offset] =
        static_cast<std::uint8_t>((hexToNibble(first) << 4) | hexToNibble(ch));
    half = false;
  }
  if (half || byteCount != VK_UUID_SIZE) {
    return false;
  }
  bytes = parsed;
  return true;
}

[[nodiscard]] bool hasNameMatch(const char *lhs, const std::string_view rhs) {
  if (lhs == nullptr || rhs.empty()) {
    return false;
  }
  const std::string_view lhsView(lhs);
  return lhsView == rhs;
}

[[nodiscard]] bool selectPhysicalDevice(
    VkPhysicalDevice device,
    viennaps::vulkan::runtime::ComputeDeviceSelection &selection,
    std::string &error) {
  if (device == VK_NULL_HANDLE) {
    error = "Invalid Vulkan physical device.";
    return false;
  }

  viennaps::vulkan::runtime::ComputeQueueSpec queue{};
  if (!viennaps::vulkan::runtime::selectComputeQueueFamily(device, queue,
                                                           error)) {
    return false;
  }
  selection = {};
  selection.handle = device;
  selection.queue = queue;
  vkGetPhysicalDeviceProperties(device, &selection.properties);
  vkGetPhysicalDeviceMemoryProperties(device, &selection.memoryProperties);
  return true;
}

[[nodiscard]] bool
enumeratePhysicalDevices(VkInstance instance,
                         std::vector<VkPhysicalDevice> &devices,
                         std::string &error) {
  std::uint32_t deviceCount = 0;
  const VkResult countResult =
      vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
  if (countResult != VK_SUCCESS || deviceCount == 0) {
    error = "No Vulkan physical device found.";
    return false;
  }
  devices.resize(deviceCount);
  const VkResult enumerateResult =
      vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
  if (enumerateResult != VK_SUCCESS) {
    devices.clear();
    error = "Failed to enumerate Vulkan physical devices.";
    return false;
  }
  devices.resize(deviceCount);
  return true;
}

[[nodiscard]] bool selectManualDeviceByIndex(
    VkInstance instance, const std::uint32_t manualDeviceIndex,
    viennaps::vulkan::runtime::ComputeDeviceSelection &selection,
    std::string &error) {
  std::vector<VkPhysicalDevice> devices;
  if (!enumeratePhysicalDevices(instance, devices, error)) {
    return false;
  }
  if (manualDeviceIndex >= devices.size()) {
    error = "Manual device index is out of range.";
    return false;
  }
  if (!selectPhysicalDevice(devices[manualDeviceIndex], selection, error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool selectManualDeviceByName(
    VkInstance instance, const std::string_view manualDeviceName,
    viennaps::vulkan::runtime::ComputeDeviceSelection &selection,
    std::string &error) {
  std::vector<VkPhysicalDevice> devices;
  if (!enumeratePhysicalDevices(instance, devices, error)) {
    return false;
  }
  for (const auto device : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    if (!hasNameMatch(props.deviceName, manualDeviceName)) {
      continue;
    }
    if (selectPhysicalDevice(device, selection, error)) {
      return true;
    }
    return false;
  }
  error = "Manual device name did not match any compute device.";
  return false;
}

[[nodiscard]] bool selectManualDeviceByUuid(
    VkInstance instance, const std::string_view manualDeviceUuid,
    viennaps::vulkan::runtime::ComputeDeviceSelection &selection,
    std::string &error) {
  std::vector<VkPhysicalDevice> devices;
  if (!enumeratePhysicalDevices(instance, devices, error)) {
    return false;
  }
  std::array<std::uint8_t, VK_UUID_SIZE> requestedUuid{};
  if (!parseUuid(manualDeviceUuid, requestedUuid)) {
    error = "Manual device UUID format is invalid.";
    return false;
  }
  for (const auto device : devices) {
    VkPhysicalDeviceProperties2 props2{};
    VkPhysicalDeviceIDProperties idProperties{};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(device, &props2);
    if (!std::equal(std::begin(idProperties.deviceUUID),
                    std::end(idProperties.deviceUUID), requestedUuid.begin())) {
      continue;
    }
    if (selectPhysicalDevice(device, selection, error)) {
      return true;
    }
  }
  error = "Manual device UUID did not match any compute device.";
  return false;
}

} // namespace

namespace viennaps::vulkan::runtime {

ComputeSession::~ComputeSession() { reset(); }

ComputeSession::ComputeSession(ComputeSession &&other) noexcept
    : instance_(std::move(other.instance_)), device_(std::move(other.device_)),
      commandContext_(std::move(other.commandContext_)),
      selection_(other.selection_),
      configuredDeviceIndex_(other.configuredDeviceIndex_),
      configuredDeviceName_(std::move(other.configuredDeviceName_)),
      configuredDeviceUuid_(std::move(other.configuredDeviceUuid_)),
      initialized_(other.initialized_), generation_(other.generation_) {
  other.selection_ = {};
  other.configuredDeviceIndex_ = kUnsetManualDeviceIndex;
  other.configuredDeviceName_.clear();
  other.configuredDeviceUuid_.clear();
  other.initialized_ = false;
  other.generation_ = 0;
}

ComputeSession &ComputeSession::operator=(ComputeSession &&other) noexcept {
  if (this != &other) {
    reset();
    instance_ = std::move(other.instance_);
    device_ = std::move(other.device_);
    commandContext_ = std::move(other.commandContext_);
    selection_ = other.selection_;
    configuredDeviceIndex_ = other.configuredDeviceIndex_;
    configuredDeviceName_ = std::move(other.configuredDeviceName_);
    configuredDeviceUuid_ = std::move(other.configuredDeviceUuid_);
    initialized_ = other.initialized_;
    generation_ = other.generation_;
    other.selection_ = {};
    other.configuredDeviceIndex_ = kUnsetManualDeviceIndex;
    other.configuredDeviceName_.clear();
    other.configuredDeviceUuid_.clear();
    other.initialized_ = false;
    other.generation_ = 0;
  }
  return *this;
}

bool ComputeSession::initialize(std::string &error,
                                const ComputeSessionOptions &options) {
  error.clear();
  const unsigned int manualSelectorCount =
      static_cast<unsigned int>(options.manualDeviceIndex !=
                                kUnsetManualDeviceIndex) +
      static_cast<unsigned int>(!options.manualDeviceName.empty()) +
      static_cast<unsigned int>(!options.manualDeviceUuid.empty());
  if (manualSelectorCount > 1U) {
    error = "Specify at most one manual Vulkan device selector.";
    return false;
  }
  if (isValid()) {
    if (optionsMatch(options)) {
      return true;
    }
    error = "Vulkan compute session is already initialized with different "
            "device options; call reset() before reconfiguration.";
    return false;
  }

  reset();

  if (!instance_.create(error)) {
    reset();
    return false;
  }

  ComputeDeviceSelection selection{};
  if (!selectComputeDevice(error, options, selection) ||
      !device_.create(selection, error) ||
      !commandContext_.create(device_, selection.queue.familyIndex, error)) {
    reset();
    if (error.empty()) {
      error = "Failed to initialize Vulkan compute session.";
    }
    return false;
  }

  selection_ = selection;
  configuredDeviceIndex_ = options.manualDeviceIndex;
  configuredDeviceName_ = options.manualDeviceName;
  configuredDeviceUuid_ = options.manualDeviceUuid;
  generation_ = allocateGeneration();
  if (!registerGeneration(generation_)) {
    generation_ = 0;
    reset();
    error = "Failed to register Vulkan compute session generation.";
    return false;
  }
  initialized_ = true;
  return true;
}

void ComputeSession::reset() {
  if (!initialized_ && generation_ == 0u && !commandContext_.pool() &&
      !device_.isValid() && !instance_.isValid()) {
    return;
  }
  unregisterGeneration(generation_);
  commandContext_.reset();
  device_.reset();
  instance_.reset();
  selection_ = {};
  configuredDeviceIndex_ = kUnsetManualDeviceIndex;
  configuredDeviceName_.clear();
  configuredDeviceUuid_.clear();
  initialized_ = false;
  generation_ = 0;
}

bool ComputeSession::isValid() const {
  return initialized_ && instance_.isValid() && device_.isValid() &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

VulkanInstance &ComputeSession::instance() { return instance_; }
const VulkanInstance &ComputeSession::instance() const { return instance_; }
VulkanDevice &ComputeSession::device() { return device_; }
const VulkanDevice &ComputeSession::device() const { return device_; }
CommandContext &ComputeSession::commandContext() { return commandContext_; }
const CommandContext &ComputeSession::commandContext() const {
  return commandContext_;
}
const ComputeDeviceSelection &ComputeSession::selection() const {
  return selection_;
}
VkInstance ComputeSession::instanceHandle() const { return instance_.get(); }
VkDevice ComputeSession::deviceHandle() const { return device_.get(); }
std::uint64_t ComputeSession::generation() const { return generation_; }

bool isLiveComputeSessionGeneration(const std::uint64_t generation) {
  if (generation == 0u) {
    return false;
  }
  std::lock_guard lock(generationMutex());
  return liveGenerations().contains(generation);
}

bool ComputeSession::optionsMatch(const ComputeSessionOptions &options) const {
  return configuredDeviceIndex_ == options.manualDeviceIndex &&
         configuredDeviceName_ == options.manualDeviceName &&
         configuredDeviceUuid_ == options.manualDeviceUuid;
}

bool ComputeSession::selectComputeDevice(
    std::string &error, const ComputeSessionOptions &options,
    ComputeDeviceSelection &selection) const {
  if (options.manualDeviceIndex != kUnsetManualDeviceIndex) {
    return selectManualDeviceByIndex(instance_.get(), options.manualDeviceIndex,
                                     selection, error);
  }
  if (!options.manualDeviceUuid.empty()) {
    return selectManualDeviceByUuid(instance_.get(), options.manualDeviceUuid,
                                    selection, error);
  }
  if (!options.manualDeviceName.empty()) {
    return selectManualDeviceByName(instance_.get(), options.manualDeviceName,
                                    selection, error);
  }
  return pickFirstComputeDevice(instance_.get(), selection, error);
}

} // namespace viennaps::vulkan::runtime
