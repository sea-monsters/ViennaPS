#include <filesystem>
#include <string>
#include <vector>

#include <compute/backendPolicy.hpp>
#include <compute/capabilityProfileIO.hpp>
#include <vcTestAsserts.hpp>

namespace viennacore {

using namespace viennaps::compute;

namespace {

void assertProfileEqual(const CapabilityProfileRecord &left,
                        const CapabilityProfileRecord &right) {
  VC_TEST_ASSERT(left.schemaVersion == right.schemaVersion);
  VC_TEST_ASSERT(left.recordedAt == right.recordedAt);

  VC_TEST_ASSERT(left.hardware.vendorId == right.hardware.vendorId);
  VC_TEST_ASSERT(left.hardware.deviceId == right.hardware.deviceId);
  VC_TEST_ASSERT(left.hardware.deviceUuid == right.hardware.deviceUuid);
  VC_TEST_ASSERT(left.hardware.driverUuid == right.hardware.driverUuid);
  VC_TEST_ASSERT(left.hardware.deviceName == right.hardware.deviceName);
  VC_TEST_ASSERT(left.hardware.driverVersion == right.hardware.driverVersion);
  VC_TEST_ASSERT(left.hardware.driverDate == right.hardware.driverDate);

  VC_TEST_ASSERT(left.capabilityProfile.cpuAvailable ==
                 right.capabilityProfile.cpuAvailable);
  VC_TEST_ASSERT(left.capabilityProfile.cudaAvailable ==
                 right.capabilityProfile.cudaAvailable);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanAvailable ==
                 right.capabilityProfile.vulkanAvailable);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanPrimitiveSuitePass ==
                 right.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp64SuitePass ==
                 right.capabilityProfile.vulkanFp64SuitePass);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanCompute ==
                 right.capabilityProfile.vulkanCompute);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanRayQuery ==
                 right.capabilityProfile.vulkanRayQuery);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanRayTracingPipeline ==
                 right.capabilityProfile.vulkanRayTracingPipeline);
  VC_TEST_ASSERT(left.capabilityProfile.shaderFloat64 ==
                 right.capabilityProfile.shaderFloat64);
  VC_TEST_ASSERT(left.capabilityProfile.safeVulkanWorkingSetBytes ==
                 right.capabilityProfile.safeVulkanWorkingSetBytes);
}

void assertBackendSelectionCpuFallbackOnNoShader64() {
  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-08-01T12:00:00Z";
  record.hardware.deviceUuid = "DEV-CPU-001";
  record.hardware.driverUuid = "DRV-CPU-001";
  record.hardware.vendorId = 1111;
  record.hardware.deviceId = 2222;
  record.hardware.deviceName = "test-device";
  record.hardware.driverVersion = "0.0.0";
  record.hardware.driverDate = "2026-08-01";
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.cudaAvailable = false;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanFp64SuitePass = false;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.vulkanRayQuery = true;
  record.capabilityProfile.vulkanRayTracingPipeline = false;
  record.capabilityProfile.shaderFloat64 = false;
  record.capabilityProfile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL;

  const auto plan =
      buildSelectionPlan(record.capabilityProfile,
                         std::vector<StageWorkload>{
                             {Stage::OXIDATION_LINEAR_SOLVE, Precision::FP64,
                              4096, false, RayMode::NONE, true}});

  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages.size() == 1);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
}

void TestRoundTrip() {
  CapabilityProfileRecord original;
  original.schemaVersion = kCapabilityProfileSchemaVersion;
  original.recordedAt = "2026-08-01T00:00:00Z";
  original.hardware.deviceUuid = "DEV-ARC-001";
  original.hardware.driverUuid = "DRV-ARC-001";
  original.hardware.vendorId = 32902;
  original.hardware.deviceId = 12345;
  original.hardware.deviceName = "Intel Arc";
  original.hardware.driverVersion = "32.0.101.0";
  original.hardware.driverDate = "2026-08-01";
  original.capabilityProfile.cpuAvailable = true;
  original.capabilityProfile.cudaAvailable = false;
  original.capabilityProfile.vulkanAvailable = true;
  original.capabilityProfile.vulkanPrimitiveSuitePass = true;
  original.capabilityProfile.vulkanFp64SuitePass = false;
  original.capabilityProfile.vulkanCompute = true;
  original.capabilityProfile.vulkanRayQuery = true;
  original.capabilityProfile.vulkanRayTracingPipeline = false;
  original.capabilityProfile.shaderFloat64 = false;
  original.capabilityProfile.safeVulkanWorkingSetBytes =
      256ULL * 1024ULL * 1024ULL;

  const auto tempPath = (std::filesystem::temp_directory_path() /
                         "viennaps_capability_profile_io_roundtrip.json")
                            .string();
  VC_TEST_ASSERT(
      writeCapabilityProfileRecordToFile(tempPath, original, nullptr));

  const auto loaded = loadCapabilityProfileRecordFromFile(tempPath);
  VC_TEST_ASSERT(loaded.ok);
  assertProfileEqual(original, loaded.record);

  std::error_code removeError;
  std::filesystem::remove(tempPath, removeError);
}

void TestUnknownFieldsIgnored() {
  const std::string payload = R"({
    "schemaVersion": 2,
    "recordedAt": "2026-08-01T00:00:00Z",
    "hardwareFingerprint": {
      "deviceUuid": "DEV-DUMMY-001",
      "driverUuid": "DRV-DUMMY-001",
      "vendorId": 32902,
      "deviceId": 12345,
      "deviceName": "dummy-device",
      "driverVersion": "32.0.101.0",
      "driverDate": "2026-08-01",
      "extraFingerprintField": "ignore-me"
    },
    "unknownTopLevel": "ignore-me",
    "capabilityProfile": {
      "cpuAvailable": true,
      "cudaAvailable": false,
      "vulkanAvailable": false,
      "vulkanPrimitiveSuitePass": false,
      "vulkanFp64SuitePass": false,
      "vulkanCompute": false,
      "vulkanRayQuery": false,
      "vulkanRayTracingPipeline": false,
      "shaderFloat64": false,
      "safeVulkanWorkingSetBytes": 0,
      "extraProfileField": true
    }
  })";
  const auto loaded = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(loaded.ok);
  VC_TEST_ASSERT(loaded.record.capabilityProfile.cpuAvailable);
}

void TestBadSchemaTypeIsRejected() {
  const std::string payload = R"({
    "schemaVersion": "2",
    "capabilityProfile": {
      "cpuAvailable": true,
      "cudaAvailable": false,
      "vulkanAvailable": false,
      "vulkanPrimitiveSuitePass": false,
      "vulkanFp64SuitePass": false,
      "vulkanCompute": false,
      "vulkanRayQuery": false,
      "vulkanRayTracingPipeline": false,
      "shaderFloat64": false,
      "safeVulkanWorkingSetBytes": 0
    }
  })";
  const auto result = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(!result.ok);
  VC_TEST_ASSERT(result.error == CapabilityProfileIOError::TYPE_MISMATCH);
}

void TestMissingRequiredFieldIsRejected() {
  const std::string payload = R"({
    "schemaVersion": 2,
    "recordedAt": "2026-08-01T00:00:00Z",
    "hardwareFingerprint": {
      "deviceUuid": "DEV-MISSING-001",
      "driverUuid": "DRV-MISSING-001",
      "vendorId": 32902,
      "deviceId": 12345,
      "deviceName": "dummy-device",
      "driverVersion": "32.0.101.0",
      "driverDate": "2026-08-01"
    },
    "capabilityProfile": {
      "cpuAvailable": true,
      "cudaAvailable": false,
      "vulkanAvailable": false,
      "vulkanPrimitiveSuitePass": false,
      "vulkanFp64SuitePass": false,
      "vulkanCompute": false,
      "vulkanRayQuery": false,
      "vulkanRayTracingPipeline": false,
      "shaderFloat64": false
    }
  })";
  const auto result = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(!result.ok);
  VC_TEST_ASSERT(result.error == CapabilityProfileIOError::MISSING_FIELD);
  VC_TEST_ASSERT(result.record.capabilityProfile.cpuAvailable);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanAvailable);
}

void TestUnknownSchemaVersionIsRejected() {
  const std::string payload = R"({
    "schemaVersion": 99,
    "recordedAt": "2026-08-01T00:00:00Z",
    "hardwareFingerprint": {
      "deviceUuid": "DEV-UNK-001",
      "driverUuid": "DRV-UNK-001",
      "vendorId": 1,
      "deviceId": 2,
      "deviceName": "future-device",
      "driverVersion": "99.0",
      "driverDate": "2026-08-01"
    },
    "capabilityProfile": {
      "cpuAvailable": true,
      "cudaAvailable": false,
      "vulkanAvailable": false,
      "vulkanPrimitiveSuitePass": false,
      "vulkanFp64SuitePass": false,
      "vulkanCompute": false,
      "vulkanRayQuery": false,
      "vulkanRayTracingPipeline": false,
      "shaderFloat64": false,
      "safeVulkanWorkingSetBytes": 0
    }
  })";
  const auto result = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(!result.ok);
  VC_TEST_ASSERT(result.error == CapabilityProfileIOError::SCHEMA_MISMATCH);
}

void TestCorruptedInputIsRejected() {
  const std::string payload = R"({ "schemaVersion": 2, "recordedAt": "oops", )";
  const auto result = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(!result.ok);
  VC_TEST_ASSERT(result.error == CapabilityProfileIOError::JSON_SYNTAX_ERROR);
}

} // namespace

} // namespace viennacore

int main() {
  viennacore::TestRoundTrip();
  viennacore::TestUnknownFieldsIgnored();
  viennacore::TestBadSchemaTypeIsRejected();
  viennacore::TestMissingRequiredFieldIsRejected();
  viennacore::TestUnknownSchemaVersionIsRejected();
  viennacore::TestCorruptedInputIsRejected();
  viennacore::assertBackendSelectionCpuFallbackOnNoShader64();
  return 0;
}
