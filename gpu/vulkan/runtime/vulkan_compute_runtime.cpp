// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "vulkan_compute_runtime.hpp"

#include "compute_session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#include <cstring>
#include <vector>

namespace {

[[nodiscard]] std::uint32_t
chooseBestMemoryType(const VkPhysicalDeviceMemoryProperties &memoryProperties,
                     const VkMemoryRequirements &requirements,
                     const VkMemoryPropertyFlags requiredFlags) {
  std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) == 0u) {
      continue;
    }
    const auto candidateFlags = memoryProperties.memoryTypes[i].propertyFlags;
    if ((candidateFlags & requiredFlags) != requiredFlags) {
      continue;
    }
    best = i;
    if ((candidateFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
      return i;
    }
  }
  return best;
}

void setError(std::string &error, std::string_view message, VkResult result) {
  if (!error.empty()) {
    return;
  }
  if (message.empty()) {
    error = "operation failed";
    return;
  }
  std::ostringstream out;
  out << message << ": " << viennaps::vulkan::runtime::vkResultToString(result);
  error = out.str();
}

} // namespace

namespace viennaps::vulkan::runtime {

const char *vkResultToString(const VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED:
    return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT:
    return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_TOO_MANY_OBJECTS:
    return "VK_ERROR_TOO_MANY_OBJECTS";
  case VK_ERROR_FORMAT_NOT_SUPPORTED:
    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  case VK_ERROR_SURFACE_LOST_KHR:
    return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_OUT_OF_POOL_MEMORY:
    return "VK_ERROR_OUT_OF_POOL_MEMORY";
  default:
    return "VK_UNKNOWN_ERROR";
  }
}

std::string_view vkDeviceTypeName(const VkPhysicalDeviceType type) {
  switch (type) {
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return "integrated-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return "discrete-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return "virtual-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return "cpu";
  case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    return "other";
  default:
    return "unknown";
  }
}

bool readSpirv(const std::string_view path, SpirvProgram &program,
               std::string &error) {
  std::ifstream input(std::string(path), std::ios::binary | std::ios::ate);
  if (!input) {
    error = std::string("Failed to open SPIR-V file: ") + std::string(path);
    return false;
  }
  const auto size = static_cast<std::size_t>(input.tellg());
  if (size == 0 || size % sizeof(std::uint32_t) != 0u) {
    error = "SPIR-V size is invalid (empty or not a multiple of 4 bytes).";
    return false;
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(size);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(size));
  if (!input) {
    error = "Failed to fully read SPIR-V input file.";
    return false;
  }
  const std::size_t wordCount = size / sizeof(std::uint32_t);
  const auto *raw = reinterpret_cast<const std::uint32_t *>(bytes.data());
  program.words.assign(raw, raw + wordCount);
  return true;
}

bool selectComputeQueueFamily(VkPhysicalDevice device, ComputeQueueSpec &spec,
                              std::string &error) {
  std::uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
  if (queueFamilyCount == 0) {
    error = "Physical device reports zero queue families.";
    return false;
  }
  std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           families.data());

  spec = {};
  bool foundAnyCompute = false;
  bool foundDedicated = false;
  for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
    const bool hasCompute =
        (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u;
    if (!hasCompute) {
      continue;
    }
    if (!foundAnyCompute) {
      spec.familyIndex = i;
      foundAnyCompute = true;
    }
    const bool hasGraphics =
        (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u;
    if (!foundDedicated && !hasGraphics) {
      spec.familyIndex = i;
      spec.dedicatedQueue = true;
      foundDedicated = true;
    }
  }

  if (!foundAnyCompute) {
    error = "No compute-capable queue family found.";
    spec = {};
    return false;
  }
  spec.dedicatedQueue = foundDedicated;
  return true;
}

bool pickFirstComputeDevice(VkInstance instance,
                            ComputeDeviceSelection &selection,
                            std::string &error) {
  std::uint32_t deviceCount = 0;
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) !=
          VK_SUCCESS ||
      deviceCount == 0) {
    error = "No Vulkan physical device found.";
    return false;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
  for (const auto device : devices) {
    ComputeQueueSpec queue{};
    if (!selectComputeQueueFamily(device, queue, error)) {
      error.clear();
      continue;
    }
    selection = {};
    selection.handle = device;
    selection.queue = queue;
    vkGetPhysicalDeviceProperties(device, &selection.properties);
    vkGetPhysicalDeviceMemoryProperties(device, &selection.memoryProperties);
    return true;
  }
  error = "No compute-capable Vulkan device found.";
  return false;
}

VulkanInstance::~VulkanInstance() { reset(); }

VulkanInstance::VulkanInstance(VulkanInstance &&other) noexcept
    : instance_(other.instance_) {
  other.instance_ = VK_NULL_HANDLE;
}

VulkanInstance &VulkanInstance::operator=(VulkanInstance &&other) noexcept {
  if (this != &other) {
    reset();
    instance_ = other.instance_;
    other.instance_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool VulkanInstance::create(std::string &error) {
  if (isValid()) {
    return true;
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "ViennaPS Vulkan Runtime Smoke";
  appInfo.pEngineName = "ViennaPS";
  appInfo.applicationVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.engineVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo instanceInfo{};
  instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceInfo.pApplicationInfo = &appInfo;

  const VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateInstance failed", result);
    return false;
  }
  return true;
}

void VulkanInstance::reset() {
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

bool VulkanInstance::isValid() const { return instance_ != VK_NULL_HANDLE; }
VkInstance VulkanInstance::get() const { return instance_; }

VulkanDevice::~VulkanDevice() { reset(); }

VulkanDevice::VulkanDevice(VulkanDevice &&other) noexcept
    : physical_(other.physical_), device_(other.device_),
      computeQueue_(other.computeQueue_), queueFamily_(other.queueFamily_),
      selection_(other.selection_) {
  other.physical_ = VK_NULL_HANDLE;
  other.device_ = VK_NULL_HANDLE;
  other.computeQueue_ = VK_NULL_HANDLE;
  other.queueFamily_ = 0;
}

VulkanDevice &VulkanDevice::operator=(VulkanDevice &&other) noexcept {
  if (this != &other) {
    reset();
    physical_ = other.physical_;
    device_ = other.device_;
    computeQueue_ = other.computeQueue_;
    queueFamily_ = other.queueFamily_;
    selection_ = other.selection_;
    other.physical_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
    other.computeQueue_ = VK_NULL_HANDLE;
    other.queueFamily_ = 0;
  }
  return *this;
}

bool VulkanDevice::create(const ComputeDeviceSelection &selection,
                          std::string &error) {
  if (isValid()) {
    return true;
  }
  float queuePriority = 1.0F;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = selection.queue.familyIndex;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &queuePriority;

  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;

  const VkResult result =
      vkCreateDevice(selection.handle, &deviceInfo, nullptr, &device_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateDevice failed", result);
    return false;
  }

  vkGetDeviceQueue(device_, selection.queue.familyIndex, 0, &computeQueue_);
  if (computeQueue_ == VK_NULL_HANDLE) {
    setError(error, "vkGetDeviceQueue returned null queue", VK_ERROR_UNKNOWN);
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    return false;
  }

  physical_ = selection.handle;
  queueFamily_ = selection.queue.familyIndex;
  selection_ = selection;
  return true;
}

void VulkanDevice::reset() {
  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
  }
  device_ = VK_NULL_HANDLE;
  computeQueue_ = VK_NULL_HANDLE;
  physical_ = VK_NULL_HANDLE;
  queueFamily_ = 0;
}

bool VulkanDevice::isValid() const { return device_ != VK_NULL_HANDLE; }
VkDevice VulkanDevice::get() const { return device_; }
VkQueue VulkanDevice::computeQueue() const { return computeQueue_; }
std::uint32_t VulkanDevice::computeQueueFamily() const { return queueFamily_; }
VkPhysicalDevice VulkanDevice::physical() const { return physical_; }
const ComputeDeviceSelection &VulkanDevice::selection() const {
  return selection_;
}

HostVisibleBuffer::~HostVisibleBuffer() { reset(); }

HostVisibleBuffer::HostVisibleBuffer(HostVisibleBuffer &&other) noexcept
    : device_(other.device_), deviceForDestroy_(other.deviceForDestroy_),
      buffer_(other.buffer_), memory_(other.memory_), bytes_(other.bytes_),
      mapped_(other.mapped_), hostCoherent_(other.hostCoherent_) {
  other.device_ = VK_NULL_HANDLE;
  other.deviceForDestroy_ = VK_NULL_HANDLE;
  other.buffer_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.bytes_ = 0;
  other.mapped_ = nullptr;
  other.hostCoherent_ = false;
}

HostVisibleBuffer &
HostVisibleBuffer::operator=(HostVisibleBuffer &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    deviceForDestroy_ = other.deviceForDestroy_;
    buffer_ = other.buffer_;
    memory_ = other.memory_;
    bytes_ = other.bytes_;
    mapped_ = other.mapped_;
    hostCoherent_ = other.hostCoherent_;
    other.device_ = VK_NULL_HANDLE;
    other.deviceForDestroy_ = VK_NULL_HANDLE;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.bytes_ = 0;
    other.mapped_ = nullptr;
    other.hostCoherent_ = false;
  }
  return *this;
}

bool HostVisibleBuffer::create(VulkanDevice &device, VkDeviceSize bytes,
                               VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags required,
                               std::string &error) {
  if (bytes == 0) {
    error = "Requested buffer size is zero.";
    return false;
  }
  if (!device.isValid()) {
    error = "Invalid device passed to HostVisibleBuffer::create.";
    return false;
  }
  if (isValid()) {
    return true;
  }

  device_ = device.get();
  deviceForDestroy_ = device.get();
  bytes_ = bytes;
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = bytes_;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult result = vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateBuffer failed", result);
    reset();
    return false;
  }

  VkMemoryRequirements memReq{};
  vkGetBufferMemoryRequirements(device_, buffer_, &memReq);
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(device.physical(), &memProps);
  std::uint32_t memoryType = chooseBestMemoryType(memProps, memReq, required);
  if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
    error = "No matching host-visible memory type found.";
    reset();
    return false;
  }

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = memoryType;
  result = vkAllocateMemory(device_, &allocInfo, nullptr, &memory_);
  if (result != VK_SUCCESS) {
    setError(error, "vkAllocateMemory failed", result);
    reset();
    return false;
  }

  result = vkBindBufferMemory(device_, buffer_, memory_, 0);
  if (result != VK_SUCCESS) {
    setError(error, "vkBindBufferMemory failed", result);
    reset();
    return false;
  }
  hostCoherent_ = (memProps.memoryTypes[memoryType].propertyFlags &
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
  return true;
}

void HostVisibleBuffer::reset() {
  if (mapped_ != nullptr && device_ != VK_NULL_HANDLE) {
    vkUnmapMemory(device_, memory_);
    mapped_ = nullptr;
  }
  if (buffer_ != VK_NULL_HANDLE && deviceForDestroy_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(deviceForDestroy_, buffer_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE && deviceForDestroy_ != VK_NULL_HANDLE) {
    vkFreeMemory(deviceForDestroy_, memory_, nullptr);
  }
  device_ = VK_NULL_HANDLE;
  deviceForDestroy_ = VK_NULL_HANDLE;
  buffer_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  bytes_ = 0;
  mapped_ = nullptr;
  hostCoherent_ = false;
}

bool HostVisibleBuffer::beginMap(std::string &error) {
  if (mapped_ != nullptr) {
    return true;
  }
  if (device_ == VK_NULL_HANDLE || memory_ == VK_NULL_HANDLE) {
    error = "Buffer has not been created.";
    return false;
  }
  const VkResult result =
      vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_);
  if (result != VK_SUCCESS) {
    setError(error, "vkMapMemory failed", result);
    return false;
  }
  return true;
}

bool HostVisibleBuffer::map(std::string &error) { return beginMap(error); }

void HostVisibleBuffer::unmap() {
  if (mapped_ == nullptr || device_ == VK_NULL_HANDLE ||
      memory_ == VK_NULL_HANDLE) {
    return;
  }
  vkUnmapMemory(device_, memory_);
  mapped_ = nullptr;
}

bool HostVisibleBuffer::write(const void *data, VkDeviceSize bytes,
                              VkDeviceSize offset, std::string &error) {
  if (mapped_ == nullptr && !beginMap(error)) {
    return false;
  }
  if (offset + bytes > bytes_) {
    error = "Buffer write exceeds mapped region.";
    return false;
  }
  std::memcpy(static_cast<std::uint8_t *>(mapped_) + offset, data,
              static_cast<std::size_t>(bytes));
  if (!hostCoherent_) {
    return flush(error);
  }
  return true;
}

bool HostVisibleBuffer::read(void *data, VkDeviceSize bytes,
                             VkDeviceSize offset, std::string &error) {
  if (mapped_ == nullptr && !beginMap(error)) {
    return false;
  }
  if (!hostCoherent_) {
    if (!invalidate(error)) {
      return false;
    }
  }
  if (offset + bytes > bytes_) {
    error = "Buffer read exceeds mapped region.";
    return false;
  }
  std::memcpy(data, static_cast<std::uint8_t *>(mapped_) + offset,
              static_cast<std::size_t>(bytes));
  return true;
}

bool HostVisibleBuffer::flush(std::string &error) {
  if (hostCoherent_ || memory_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    return true;
  }
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = memory_;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  const VkResult result = vkFlushMappedMemoryRanges(device_, 1, &range);
  if (result != VK_SUCCESS) {
    setError(error, "vkFlushMappedMemoryRanges failed", result);
    return false;
  }
  return true;
}

bool HostVisibleBuffer::invalidate(std::string &error) {
  if (hostCoherent_ || memory_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    return true;
  }
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = memory_;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  const VkResult result = vkInvalidateMappedMemoryRanges(device_, 1, &range);
  if (result != VK_SUCCESS) {
    setError(error, "vkInvalidateMappedMemoryRanges failed", result);
    return false;
  }
  return true;
}

bool HostVisibleBuffer::isValid() const { return buffer_ != VK_NULL_HANDLE; }
VkBuffer HostVisibleBuffer::handle() const { return buffer_; }
VkDeviceMemory HostVisibleBuffer::memory() const { return memory_; }
VkDeviceSize HostVisibleBuffer::size() const { return bytes_; }
void *HostVisibleBuffer::mappedPtr() const { return mapped_; }
bool HostVisibleBuffer::hostCoherent() const { return hostCoherent_; }
VkDevice HostVisibleBuffer::ownerDevice() const { return device_; }

DeviceBuffer::~DeviceBuffer() { reset(); }

DeviceBuffer::DeviceBuffer(DeviceBuffer &&other) noexcept
    : device_(other.device_), deviceForDestroy_(other.deviceForDestroy_),
      buffer_(other.buffer_), memory_(other.memory_), bytes_(other.bytes_),
      ownerSessionGeneration_(other.ownerSessionGeneration_) {
  other.device_ = VK_NULL_HANDLE;
  other.deviceForDestroy_ = VK_NULL_HANDLE;
  other.buffer_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.bytes_ = 0;
  other.ownerSessionGeneration_ = 0;
}

DeviceBuffer &DeviceBuffer::operator=(DeviceBuffer &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    deviceForDestroy_ = other.deviceForDestroy_;
    buffer_ = other.buffer_;
    memory_ = other.memory_;
    bytes_ = other.bytes_;
    ownerSessionGeneration_ = other.ownerSessionGeneration_;
    other.device_ = VK_NULL_HANDLE;
    other.deviceForDestroy_ = VK_NULL_HANDLE;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.bytes_ = 0;
    other.ownerSessionGeneration_ = 0;
  }
  return *this;
}

bool DeviceBuffer::create(VulkanDevice &device, const VkDeviceSize bytes,
                          std::string &error) {
  return createImpl(device, bytes, 0u, error);
}

bool DeviceBuffer::create(ComputeSession &session, const VkDeviceSize bytes,
                          std::string &error) {
  error.clear();
  if (!session.isValid() || session.generation() == 0u) {
    error = "Compute session is not initialized.";
    return false;
  }
  return createImpl(session.device(), bytes, session.generation(), error);
}

bool DeviceBuffer::createImpl(VulkanDevice &device, const VkDeviceSize bytes,
                              const std::uint64_t sessionGeneration,
                              std::string &error) {
  error.clear();
  if (bytes == 0) {
    error = "Requested device buffer size is zero.";
    return false;
  }
  if (!device.isValid()) {
    error = "Invalid device passed to DeviceBuffer::create.";
    return false;
  }
  if (isValid()) {
    if (device_ == device.get() && bytes_ == bytes &&
        ownerSessionGeneration_ == sessionGeneration) {
      return true;
    }
    error = "DeviceBuffer is already initialized with different properties.";
    return false;
  }

  const VkDevice targetDevice = device.get();
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = bytes;
  bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(targetDevice, &bufferInfo, nullptr, &buffer);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateBuffer failed", result);
    return false;
  }

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(targetDevice, buffer, &requirements);
  const std::uint32_t memoryType = chooseBestMemoryType(
      device.selection().memoryProperties, requirements,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
    error = "No matching device-local memory type found.";
    vkDestroyBuffer(targetDevice, buffer, nullptr);
    return false;
  }

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = requirements.size;
  allocInfo.memoryTypeIndex = memoryType;
  result = vkAllocateMemory(targetDevice, &allocInfo, nullptr, &memory);
  if (result != VK_SUCCESS) {
    setError(error, "vkAllocateMemory failed", result);
    vkDestroyBuffer(targetDevice, buffer, nullptr);
    return false;
  }
  result = vkBindBufferMemory(targetDevice, buffer, memory, 0);
  if (result != VK_SUCCESS) {
    setError(error, "vkBindBufferMemory failed", result);
    vkFreeMemory(targetDevice, memory, nullptr);
    vkDestroyBuffer(targetDevice, buffer, nullptr);
    return false;
  }

  device_ = targetDevice;
  deviceForDestroy_ = targetDevice;
  buffer_ = buffer;
  memory_ = memory;
  bytes_ = bytes;
  ownerSessionGeneration_ = sessionGeneration;
  return true;
}

void DeviceBuffer::reset() {
  const bool canDestroy =
      ownerSessionGeneration_ == 0u ||
      isLiveComputeSessionGeneration(ownerSessionGeneration_);
  if (canDestroy && buffer_ != VK_NULL_HANDLE &&
      deviceForDestroy_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(deviceForDestroy_, buffer_, nullptr);
  }
  if (canDestroy && memory_ != VK_NULL_HANDLE &&
      deviceForDestroy_ != VK_NULL_HANDLE) {
    vkFreeMemory(deviceForDestroy_, memory_, nullptr);
  }
  device_ = VK_NULL_HANDLE;
  deviceForDestroy_ = VK_NULL_HANDLE;
  buffer_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  bytes_ = 0;
  ownerSessionGeneration_ = 0;
}

namespace {

[[nodiscard]] bool validateDeviceTransfer(
    const DeviceBuffer &buffer, ComputeSession &session, const void *data,
    const VkDeviceSize bytes, const VkDeviceSize offset, std::string &error) {
  if (!session.isValid()) {
    error = "Compute session is not initialized.";
    return false;
  }
  if (!buffer.isValid()) {
    error = "DeviceBuffer has not been created.";
    return false;
  }
  if (buffer.ownerSessionGeneration() != 0u &&
      buffer.ownerSessionGeneration() != session.generation()) {
    error = "DeviceBuffer belongs to a stale or different session generation.";
    return false;
  }
  if (buffer.ownerDevice() != session.deviceHandle()) {
    error = "DeviceBuffer and ComputeSession use different Vulkan devices.";
    return false;
  }
  if (bytes == 0) {
    error = "Device buffer transfer size is zero.";
    return false;
  }
  if (data == nullptr) {
    error = "Device buffer transfer data pointer is null.";
    return false;
  }
  if (offset > buffer.size() || bytes > buffer.size() - offset) {
    error = "Device buffer transfer exceeds the buffer range.";
    return false;
  }
  return true;
}

[[nodiscard]] bool validateDeviceRange(const DeviceBuffer &buffer,
                                       const VkDeviceSize bytes,
                                       const VkDeviceSize offset,
                                       std::string &error) {
  if (!buffer.isValid()) {
    error = "DeviceBuffer has not been created.";
    return false;
  }
  if (bytes == 0) {
    error = "Device buffer transfer size is zero.";
    return false;
  }
  if (offset > buffer.size() || bytes > buffer.size() - offset) {
    error = "Device buffer transfer exceeds the buffer range.";
    return false;
  }
  return true;
}

[[nodiscard]] bool submitTransfer(ComputeSession &session,
                                   VkCommandBuffer commandBuffer,
                                   Fence &fence, std::string &error) {
  if (!fence.create(session.device(), error)) {
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer;
  const VkResult result = vkQueueSubmit(session.device().computeQueue(), 1,
                                        &submit, fence.get());
  if (result != VK_SUCCESS) {
    setError(error, "vkQueueSubmit failed", result);
    return false;
  }
  return fence.wait(std::numeric_limits<std::uint64_t>::max(), error);
}

void addTransferToComputeBarrier(VkCommandBuffer commandBuffer,
                                 VkBuffer buffer, VkDeviceSize offset,
                                 VkDeviceSize bytes) {
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = bytes;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                       &barrier, 0, nullptr);
}

void addDeviceToTransferWriteBarrier(VkCommandBuffer commandBuffer,
                                     VkBuffer buffer, VkDeviceSize offset,
                                     VkDeviceSize bytes) {
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = bytes;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                       &barrier, 0, nullptr);
}

void addDeviceToTransferBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                VkDeviceSize offset, VkDeviceSize bytes) {
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = bytes;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                       &barrier, 0, nullptr);
}

} // namespace

bool DeviceBuffer::upload(ComputeSession &session, const void *data,
                          const VkDeviceSize bytes, const VkDeviceSize offset,
                          std::string &error) {
  error.clear();
  if (!validateDeviceTransfer(*this, session, data, bytes, offset, error)) {
    return false;
  }
  HostVisibleBuffer staging{};
  if (!staging.create(session.device(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, error) ||
      !staging.write(data, bytes, 0, error)) {
    return false;
  }
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (!session.commandContext().allocatePrimary(commandBuffer, error)) {
    return false;
  }
  Fence fence{};
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
  if (result == VK_SUCCESS) {
    addDeviceToTransferWriteBarrier(commandBuffer, buffer_, offset, bytes);
    VkBufferCopy region{0, offset, bytes};
    vkCmdCopyBuffer(commandBuffer, staging.handle(), buffer_, 1, &region);
    addTransferToComputeBarrier(commandBuffer, buffer_, offset, bytes);
    result = vkEndCommandBuffer(commandBuffer);
  }
  if (result != VK_SUCCESS) {
    setError(error, "DeviceBuffer upload command recording failed", result);
  } else if (!submitTransfer(session, commandBuffer, fence, error)) {
    result = VK_ERROR_UNKNOWN;
  }
  vkFreeCommandBuffers(session.deviceHandle(), session.commandContext().pool(),
                       1, &commandBuffer);
  return result == VK_SUCCESS && error.empty();
}

bool DeviceBuffer::download(ComputeSession &session, void *data,
                            const VkDeviceSize bytes, const VkDeviceSize offset,
                            std::string &error) const {
  error.clear();
  if (!validateDeviceTransfer(*this, session, data, bytes, offset, error)) {
    return false;
  }
  HostVisibleBuffer staging{};
  if (!staging.create(session.device(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, error)) {
    return false;
  }
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (!session.commandContext().allocatePrimary(commandBuffer, error)) {
    return false;
  }
  Fence fence{};
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
  if (result == VK_SUCCESS) {
    addDeviceToTransferBarrier(commandBuffer, buffer_, offset, bytes);
    VkBufferCopy region{offset, 0, bytes};
    vkCmdCopyBuffer(commandBuffer, buffer_, staging.handle(), 1, &region);
    result = vkEndCommandBuffer(commandBuffer);
  }
  if (result != VK_SUCCESS) {
    setError(error, "DeviceBuffer download command recording failed", result);
  } else if (!submitTransfer(session, commandBuffer, fence, error)) {
    result = VK_ERROR_UNKNOWN;
  }
  vkFreeCommandBuffers(session.deviceHandle(), session.commandContext().pool(),
                       1, &commandBuffer);
  if (result != VK_SUCCESS || !error.empty()) {
    return false;
  }
  return staging.read(data, bytes, 0, error);
}

bool DeviceBuffer::copyTo(ComputeSession &session, DeviceBuffer &destination,
                          const VkDeviceSize bytes,
                          const VkDeviceSize sourceOffset,
                          const VkDeviceSize destinationOffset,
                          std::string &error) const {
  error.clear();
  if (!session.isValid()) {
    error = "Compute session is not initialized.";
    return false;
  }
  if (!validateDeviceRange(*this, bytes, sourceOffset, error) ||
      !validateDeviceRange(destination, bytes, destinationOffset, error)) {
    return false;
  }
  if ((ownerSessionGeneration() != 0u &&
       ownerSessionGeneration() != session.generation()) ||
      (destination.ownerSessionGeneration() != 0u &&
       destination.ownerSessionGeneration() != session.generation())) {
    error = "DeviceBuffer belongs to a stale or different session generation.";
    return false;
  }
  if (ownerDevice() != session.deviceHandle() ||
      destination.ownerDevice() != session.deviceHandle()) {
    error = "DeviceBuffer and ComputeSession use different Vulkan devices.";
    return false;
  }
  if (buffer_ == destination.buffer_) {
    error = "DeviceBuffer copy requires distinct source and destination "
            "buffers.";
    return false;
  }
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (!session.commandContext().allocatePrimary(commandBuffer, error)) {
    return false;
  }
  Fence fence{};
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
  if (result == VK_SUCCESS) {
    VkBufferMemoryBarrier barriers[2]{};
    for (auto &barrier : barriers) {
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    barriers[0].buffer = buffer_;
    barriers[0].offset = sourceOffset;
    barriers[0].size = bytes;
    barriers[1].buffer = destination.buffer_;
    barriers[1].offset = destinationOffset;
    barriers[1].size = bytes;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 2,
                         barriers, 0, nullptr);
    VkBufferCopy region{sourceOffset, destinationOffset, bytes};
    vkCmdCopyBuffer(commandBuffer, buffer_, destination.buffer_, 1, &region);
    addTransferToComputeBarrier(commandBuffer, destination.buffer_,
                                destinationOffset, bytes);
    result = vkEndCommandBuffer(commandBuffer);
  }
  if (result != VK_SUCCESS) {
    setError(error, "DeviceBuffer copy command recording failed", result);
  } else if (!submitTransfer(session, commandBuffer, fence, error)) {
    result = VK_ERROR_UNKNOWN;
  }
  vkFreeCommandBuffers(session.deviceHandle(), session.commandContext().pool(),
                       1, &commandBuffer);
  return result == VK_SUCCESS && error.empty();
}

bool DeviceBuffer::isValid() const { return buffer_ != VK_NULL_HANDLE; }
VkBuffer DeviceBuffer::handle() const { return buffer_; }
VkDeviceMemory DeviceBuffer::memory() const { return memory_; }
VkDeviceSize DeviceBuffer::size() const { return bytes_; }
VkDevice DeviceBuffer::ownerDevice() const { return device_; }
std::uint64_t DeviceBuffer::ownerSessionGeneration() const {
  return ownerSessionGeneration_;
}

ShaderModule::~ShaderModule() { reset(); }

ShaderModule::ShaderModule(ShaderModule &&other) noexcept
    : device_(other.device_), module_(other.module_) {
  other.device_ = VK_NULL_HANDLE;
  other.module_ = VK_NULL_HANDLE;
}

ShaderModule &ShaderModule::operator=(ShaderModule &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    module_ = other.module_;
    other.device_ = VK_NULL_HANDLE;
    other.module_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool ShaderModule::create(VulkanDevice &device, const SpirvProgram &program,
                          std::string &error) {
  if (program.words.empty()) {
    error = "Invalid SPIR-V payload.";
    return false;
  }
  if (!device.isValid()) {
    error = "Invalid device passed to ShaderModule::create.";
    return false;
  }
  if (module_ != VK_NULL_HANDLE) {
    return true;
  }
  device_ = device.get();
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = program.words.size() * sizeof(std::uint32_t);
  info.pCode = program.words.data();
  const VkResult result =
      vkCreateShaderModule(device_, &info, nullptr, &module_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateShaderModule failed", result);
    module_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void ShaderModule::reset() {
  if (module_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device_, module_, nullptr);
  }
  module_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void ShaderModule::abandon() {
  module_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkShaderModule ShaderModule::get() const { return module_; }

DescriptorSetLayout::~DescriptorSetLayout() { reset(); }

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout &&other) noexcept
    : device_(other.device_), layout_(other.layout_) {
  other.device_ = VK_NULL_HANDLE;
  other.layout_ = VK_NULL_HANDLE;
}

DescriptorSetLayout &
DescriptorSetLayout::operator=(DescriptorSetLayout &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    layout_ = other.layout_;
    other.device_ = VK_NULL_HANDLE;
    other.layout_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool DescriptorSetLayout::create(
    VulkanDevice &device,
    std::span<const VkDescriptorSetLayoutBinding> bindings,
    std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to DescriptorSetLayout::create.";
    return false;
  }
  if (layout_ != VK_NULL_HANDLE) {
    return true;
  }
  device_ = device.get();
  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  info.pBindings = bindings.data();
  const VkResult result =
      vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateDescriptorSetLayout failed", result);
    reset();
    return false;
  }
  return true;
}

void DescriptorSetLayout::reset() {
  if (layout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
  }
  layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void DescriptorSetLayout::abandon() {
  layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkDescriptorSetLayout DescriptorSetLayout::get() const { return layout_; }

PipelineLayout::~PipelineLayout() { reset(); }

PipelineLayout::PipelineLayout(PipelineLayout &&other) noexcept
    : device_(other.device_), layout_(other.layout_) {
  other.device_ = VK_NULL_HANDLE;
  other.layout_ = VK_NULL_HANDLE;
}

PipelineLayout &PipelineLayout::operator=(PipelineLayout &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    layout_ = other.layout_;
    other.device_ = VK_NULL_HANDLE;
    other.layout_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool PipelineLayout::create(VulkanDevice &device,
                            VkDescriptorSetLayout descriptorSetLayout,
                            std::string &error) {
  return create(device, descriptorSetLayout, {}, error);
}

bool PipelineLayout::create(
    VulkanDevice &device, VkDescriptorSetLayout descriptorSetLayout,
    std::span<const VkPushConstantRange> pushConstantRanges,
    std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to PipelineLayout::create.";
    return false;
  }
  if (layout_ != VK_NULL_HANDLE) {
    return true;
  }
  const auto maxPushConstantBytes =
      device.selection().properties.limits.maxPushConstantsSize;
  for (const auto &range : pushConstantRanges) {
    if (range.stageFlags == 0 || range.size == 0 || (range.offset % 4u) != 0u ||
        (range.size % 4u) != 0u || range.offset > maxPushConstantBytes ||
        range.size > maxPushConstantBytes - range.offset) {
      error = "Invalid or unsupported push-constant range.";
      return false;
    }
  }
  device_ = device.get();
  VkPipelineLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = 1;
  info.pSetLayouts = &descriptorSetLayout;
  info.pushConstantRangeCount =
      static_cast<std::uint32_t>(pushConstantRanges.size());
  info.pPushConstantRanges = pushConstantRanges.data();
  const VkResult result =
      vkCreatePipelineLayout(device_, &info, nullptr, &layout_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreatePipelineLayout failed", result);
    reset();
    return false;
  }
  return true;
}

void PipelineLayout::reset() {
  if (layout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, layout_, nullptr);
  }
  layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void PipelineLayout::abandon() {
  layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkPipelineLayout PipelineLayout::get() const { return layout_; }

ComputePipeline::~ComputePipeline() { reset(); }

ComputePipeline::ComputePipeline(ComputePipeline &&other) noexcept
    : device_(other.device_), pipeline_(other.pipeline_) {
  other.device_ = VK_NULL_HANDLE;
  other.pipeline_ = VK_NULL_HANDLE;
}

ComputePipeline &ComputePipeline::operator=(ComputePipeline &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    pipeline_ = other.pipeline_;
    other.device_ = VK_NULL_HANDLE;
    other.pipeline_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool ComputePipeline::create(VulkanDevice &device, const ShaderModule &shader,
                             const PipelineLayout &layout, std::string &error) {
  return create(device, shader, layout, ComputePipelineOptions{}, error);
}

bool ComputePipeline::create(VulkanDevice &device, const ShaderModule &shader,
                             const PipelineLayout &layout,
                             const ComputePipelineOptions &options,
                             std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to ComputePipeline::create.";
    return false;
  }
  if (pipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  if (shader.get() == VK_NULL_HANDLE || layout.get() == VK_NULL_HANDLE) {
    error = "Pipeline requires valid shader module and pipeline layout.";
    return false;
  }
  if (options.entryPoint.empty()) {
    error = "Compute pipeline entry point must not be empty.";
    return false;
  }
  if (!options.specializationEntries.empty() &&
      (options.specializationData == nullptr ||
       options.specializationDataSize == 0)) {
    error = "Specialization entries require non-empty data.";
    return false;
  }
  for (const auto &entry : options.specializationEntries) {
    if (entry.offset > options.specializationDataSize ||
        entry.size > options.specializationDataSize - entry.offset) {
      error = "Specialization entry exceeds the supplied data.";
      return false;
    }
  }
  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  VkPipelineShaderStageCreateInfo stageInfo{};
  stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stageInfo.module = shader.get();
  const std::string entryPoint(options.entryPoint);
  stageInfo.pName = entryPoint.c_str();
  VkSpecializationInfo specializationInfo{};
  if (!options.specializationEntries.empty()) {
    specializationInfo.mapEntryCount =
        static_cast<std::uint32_t>(options.specializationEntries.size());
    specializationInfo.pMapEntries = options.specializationEntries.data();
    specializationInfo.dataSize = options.specializationDataSize;
    specializationInfo.pData = options.specializationData;
    stageInfo.pSpecializationInfo = &specializationInfo;
  }
  pipelineInfo.stage = stageInfo;
  pipelineInfo.layout = layout.get();

  device_ = device.get();
  const VkResult result = vkCreateComputePipelines(
      device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateComputePipelines failed", result);
    reset();
    return false;
  }
  return true;
}

void ComputePipeline::reset() {
  if (pipeline_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, pipeline_, nullptr);
  }
  pipeline_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void ComputePipeline::abandon() {
  pipeline_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkPipeline ComputePipeline::get() const { return pipeline_; }

DescriptorPool::~DescriptorPool() { reset(); }

DescriptorPool::DescriptorPool(DescriptorPool &&other) noexcept
    : device_(other.device_), pool_(other.pool_) {
  other.device_ = VK_NULL_HANDLE;
  other.pool_ = VK_NULL_HANDLE;
}

DescriptorPool &DescriptorPool::operator=(DescriptorPool &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    pool_ = other.pool_;
    other.device_ = VK_NULL_HANDLE;
    other.pool_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool DescriptorPool::create(VulkanDevice &device,
                            std::uint32_t descriptorSetCount,
                            VkDescriptorType descriptorType,
                            std::string &error) {
  return create(device, descriptorSetCount, 2u, descriptorType, error);
}

bool DescriptorPool::create(VulkanDevice &device,
                            const std::uint32_t descriptorSetCount,
                            const std::uint32_t descriptorsPerSet,
                            const VkDescriptorType descriptorType,
                            std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to DescriptorPool::create.";
    return false;
  }
  if (pool_ != VK_NULL_HANDLE) {
    return true;
  }
  if (descriptorSetCount == 0u || descriptorsPerSet == 0u ||
      descriptorSetCount >
          std::numeric_limits<std::uint32_t>::max() / descriptorsPerSet) {
    error = "Descriptor pool counts must be non-zero and must not overflow.";
    return false;
  }
  device_ = device.get();
  VkDescriptorPoolSize poolSize{};
  poolSize.type = descriptorType;
  poolSize.descriptorCount = descriptorSetCount * descriptorsPerSet;
  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.poolSizeCount = 1;
  info.pPoolSizes = &poolSize;
  info.maxSets = descriptorSetCount;
  const VkResult result =
      vkCreateDescriptorPool(device_, &info, nullptr, &pool_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateDescriptorPool failed", result);
    reset();
    return false;
  }
  return true;
}

bool DescriptorPool::allocate(VkDescriptorSetLayout layout,
                              VkDescriptorSet &set, std::string &error) {
  if (pool_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    error = "DescriptorPool must be initialized before allocation.";
    return false;
  }
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = pool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &layout;
  const VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &set);
  if (result != VK_SUCCESS) {
    setError(error, "vkAllocateDescriptorSets failed", result);
    return false;
  }
  return true;
}

void DescriptorPool::reset() {
  if (pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, pool_, nullptr);
  }
  pool_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void DescriptorPool::abandon() {
  pool_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkDescriptorPool DescriptorPool::get() const { return pool_; }

CommandContext::~CommandContext() { reset(); }

CommandContext::CommandContext(CommandContext &&other) noexcept
    : device_(other.device_), pool_(other.pool_) {
  other.device_ = VK_NULL_HANDLE;
  other.pool_ = VK_NULL_HANDLE;
}

CommandContext &CommandContext::operator=(CommandContext &&other) noexcept {
  if (this != &other) {
    reset();
    device_ = other.device_;
    pool_ = other.pool_;
    other.device_ = VK_NULL_HANDLE;
    other.pool_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool CommandContext::create(VulkanDevice &device,
                            std::uint32_t queueFamilyIndex,
                            std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to CommandContext::create.";
    return false;
  }
  if (pool_ != VK_NULL_HANDLE) {
    return true;
  }
  device_ = device.get();
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = queueFamilyIndex;
  const VkResult result =
      vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateCommandPool failed", result);
    device_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void CommandContext::reset() {
  if (pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device_, pool_, nullptr);
  }
  device_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
}

bool CommandContext::allocatePrimary(VkCommandBuffer &commandBuffer,
                                     std::string &error) {
  if (pool_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    error = "CommandContext must be created before allocatePrimary.";
    return false;
  }
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  const VkResult result =
      vkAllocateCommandBuffers(device_, &alloc, &commandBuffer);
  if (result != VK_SUCCESS) {
    setError(error, "vkAllocateCommandBuffers failed", result);
    return false;
  }
  return true;
}

VkCommandPool CommandContext::pool() const { return pool_; }

Fence::~Fence() { destroy(); }

Fence::Fence(Fence &&other) noexcept
    : device_(other.device_), fence_(other.fence_) {
  other.device_ = VK_NULL_HANDLE;
  other.fence_ = VK_NULL_HANDLE;
}

Fence &Fence::operator=(Fence &&other) noexcept {
  if (this != &other) {
    destroy();
    device_ = other.device_;
    fence_ = other.fence_;
    other.device_ = VK_NULL_HANDLE;
    other.fence_ = VK_NULL_HANDLE;
  }
  return *this;
}

bool Fence::create(VulkanDevice &device, std::string &error) {
  if (!device.isValid()) {
    error = "Invalid device passed to Fence::create.";
    return false;
  }
  if (fence_ != VK_NULL_HANDLE) {
    return true;
  }
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  const VkResult result =
      vkCreateFence(device.get(), &fenceInfo, nullptr, &fence_);
  if (result != VK_SUCCESS) {
    setError(error, "vkCreateFence failed", result);
    return false;
  }
  device_ = device.get();
  return true;
}

bool Fence::wait(std::uint64_t timeoutNs, std::string &error) const {
  if (fence_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    error = "Fence was not created.";
    return false;
  }
  const VkResult result =
      vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeoutNs);
  if (result != VK_SUCCESS) {
    setError(error, "vkWaitForFences failed", result);
    return false;
  }
  return true;
}

void Fence::reset() const {
  if (fence_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkResetFences(device_, 1, &fence_);
  }
}

void Fence::reset(VulkanDevice &device) {
  if (fence_ != VK_NULL_HANDLE && device.get() != VK_NULL_HANDLE) {
    vkResetFences(device.get(), 1, &fence_);
  }
}

void Fence::destroy() {
  if (fence_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
    vkDestroyFence(device_, fence_, nullptr);
  }
  fence_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void Fence::abandon() {
  fence_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

VkFence Fence::get() const { return fence_; }

std::uint32_t orderedFloatBits(const float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
}

bool exactlyEqualFloat(const float lhs, const float rhs) {
  if (lhs == rhs) {
    return true;
  }
  const std::uint32_t lhsBits = orderedFloatBits(lhs);
  const std::uint32_t rhsBits = orderedFloatBits(rhs);
  const std::uint32_t diff =
      lhsBits > rhsBits ? (lhsBits - rhsBits) : (rhsBits - lhsBits);
  return diff == 0u;
}

} // namespace viennaps::vulkan::runtime
