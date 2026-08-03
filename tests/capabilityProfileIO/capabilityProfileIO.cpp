#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <compute/backendPolicy.hpp>
#include <compute/capabilityProfileIO.hpp>
#include <compute/deploymentProfile.hpp>
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
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.status ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.status);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.contractId ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.contractId);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.caseCount ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.caseCount);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs);
  VC_TEST_ASSERT(left.capabilityProfile.vulkanFp32NumericalSmoke.elapsedMs ==
                 right.capabilityProfile.vulkanFp32NumericalSmoke.elapsedMs);
  VC_TEST_ASSERT(left.capabilityProfile.safeVulkanWorkingSetBytes ==
                 right.capabilityProfile.safeVulkanWorkingSetBytes);
}

void writeRecordToTempDir(const HardwareFingerprint &hardware,
                          const CapabilityProfileRecord &record,
                          const std::string &dirName,
                          const std::string &suffix = "") {
  const auto dir =
      std::filesystem::path(std::filesystem::temp_directory_path()) / dirName;
  std::filesystem::create_directories(dir);
  const auto filePath = dir / (hardware.deviceUuid + suffix + ".json");
  auto persisted = record;
  if (persisted.capabilityProfile.vulkanPrimitiveSuitePass) {
    persisted.capabilityProfile.vulkanFp32NumericalSmoke.status =
        VulkanNumericalSmokeStatus::PASS;
    persisted.capabilityProfile.vulkanFp32NumericalSmoke.contractId =
        std::string(kVulkanFp32NumericalSmokeContract);
    persisted.capabilityProfile.vulkanFp32NumericalSmoke.caseCount = 1U;
    persisted.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp = 0U;
    persisted.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs =
        kVulkanFp32NumericalSmokeWatchdogMs;
  }
  VC_TEST_ASSERT(writeCapabilityProfileRecordToFile(filePath.string(),
                                                    persisted, nullptr));
}

void cleanTempProfileIfExists(const HardwareFingerprint &hardware,
                              const std::string &dirName,
                              const std::string &suffix = "") {
  const auto dir =
      std::filesystem::path(std::filesystem::temp_directory_path()) / dirName;
  const auto filePath = dir / (hardware.deviceUuid + suffix + ".json");
  std::error_code removeError;
  std::filesystem::remove(filePath, removeError);
}

void assertManualBlockReasonContainsVulkan(const StageSelection &selection) {
  VC_TEST_ASSERT(!selection.ok);
  VC_TEST_ASSERT(!selection.selected);
  VC_TEST_ASSERT(!selection.rejectionReasons.empty());
  VC_TEST_ASSERT(selection.rejectionReasons[0].find(
                     "Manual backend blocked: ") != std::string::npos);
}

#ifdef _WIN32
void setEnvVar(const std::string &name, const std::string &value) {
  _putenv_s(name.c_str(), value.c_str());
}
#else
void setEnvVar(const std::string &name, const std::string &value) {
  if (value.empty()) {
    unsetenv(name.c_str());
    return;
  }
  setenv(name.c_str(), value.c_str(), 1);
}
#endif

std::string getEnvVar(const std::string &name) {
#ifdef _WIN32
  char *value = nullptr;
  std::size_t valueLength = 0;
  if (_dupenv_s(&value, &valueLength, name.c_str()) != 0 || value == nullptr) {
    return {};
  }
  const std::string result(value);
  std::free(value);
  return result;
#else
  const char *value = std::getenv(name.c_str());
  return value == nullptr ? std::string() : std::string(value);
#endif
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

void TestNumericalSmokeEvidenceRoundTrips() {
  CapabilityProfileRecord record;
  record.recordedAt = "2026-01-01T00:00:00Z";
  record.hardware.deviceUuid = "DEV";
  record.hardware.driverUuid = "DRV";
  record.hardware.deviceName = "fixture";
  record.hardware.driverVersion = "1";
  record.hardware.driverDate = "2026-01-01";
  record.capabilityProfile.vulkanFp32NumericalSmoke.status =
      VulkanNumericalSmokeStatus::FAIL;
  record.capabilityProfile.vulkanFp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  record.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp = 3U;
  record.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;
  const auto path = (std::filesystem::temp_directory_path() /
                     "viennaps-profile-numerical-smoke.json")
                        .string();
  VC_TEST_ASSERT(writeCapabilityProfileRecordToFile(path, record, nullptr));
  const auto loaded = loadCapabilityProfileRecordFromFile(path);
  VC_TEST_ASSERT(loaded.ok);
  VC_TEST_ASSERT(
      loaded.record.capabilityProfile.vulkanFp32NumericalSmoke.status ==
      VulkanNumericalSmokeStatus::FAIL);
  VC_TEST_ASSERT(
      loaded.record.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp == 3U);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestUnknownFieldsIgnored() {
  const std::string payload = R"({
    "schemaVersion": 3,
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
      "vulkanFp32NumericalSmoke": {
        "status": "NOT_RUN",
        "contractId": "",
        "caseCount": 0,
        "mismatchCount": 0,
        "maxUlp": 0,
        "watchdogMs": 0,
        "elapsedMs": 0,
        "failureDiagnostic": ""
      },
      "safeVulkanWorkingSetBytes": 0,
      "extraProfileField": true
    }
  })";
  const auto loaded = parseCapabilityProfileRecord(payload);
  VC_TEST_ASSERT(!loaded.ok);
  VC_TEST_ASSERT(loaded.error == CapabilityProfileIOError::SCHEMA_MISMATCH);
}

void TestLegacySchemaLoadsWithDefaultNumericalEvidence() {
  CapabilityProfileRecord legacy;
  legacy.recordedAt = "2026-01-01T00:00:00Z";
  legacy.hardware.deviceUuid = "DEV-LEGACY";
  legacy.hardware.driverUuid = "DRV-LEGACY";
  legacy.hardware.deviceName = "legacy";
  legacy.hardware.driverVersion = "1";
  legacy.hardware.driverDate = "2026-01-01";
  legacy.capabilityProfile.vulkanAvailable = true;
  legacy.capabilityProfile.vulkanCompute = true;
  legacy.capabilityProfile.vulkanPrimitiveSuitePass = true;
  for (const auto schemaVersion : {1U, 2U}) {
    legacy.schemaVersion = schemaVersion;
    const auto loaded = parseCapabilityProfileRecord(toJson(legacy));
    VC_TEST_ASSERT(loaded.ok);
    VC_TEST_ASSERT(loaded.record.schemaVersion == schemaVersion);
    VC_TEST_ASSERT(
        loaded.record.capabilityProfile.vulkanFp32NumericalSmoke.status ==
        VulkanNumericalSmokeStatus::NOT_RUN);
    VC_TEST_ASSERT(
        !viennaps::compute::detail::profileHasStrictFp32NumericalSmoke(
            loaded.record.capabilityProfile));
  }
}

void TestDuplicateAndOverflowFieldsAreRejected() {
  const std::string duplicate = R"({
    "schemaVersion":3,"recordedAt":"x",
    "hardwareFingerprint":{"deviceUuid":"d","driverUuid":"r","vendorId":1,"deviceId":2,"deviceName":"n","driverVersion":"1","driverDate":"d"},
    "capabilityProfile":{"cpuAvailable":true,"cpuAvailable":true}
  })";
  const auto duplicateResult = parseCapabilityProfileRecord(duplicate);
  VC_TEST_ASSERT(!duplicateResult.ok);
  VC_TEST_ASSERT(duplicateResult.error ==
                 CapabilityProfileIOError::SCHEMA_MISMATCH);

  const std::string duplicateHardware = R"({
    "schemaVersion":3,"recordedAt":"x",
    "hardwareFingerprint":{"deviceUuid":"d","deviceUuid":"d"}
  })";
  const auto duplicateHardwareResult =
      parseCapabilityProfileRecord(duplicateHardware);
  VC_TEST_ASSERT(!duplicateHardwareResult.ok);
  VC_TEST_ASSERT(duplicateHardwareResult.error ==
                 CapabilityProfileIOError::SCHEMA_MISMATCH);

  const std::string overflow = R"({"schemaVersion":18446744073709551616})";
  const auto overflowResult = parseCapabilityProfileRecord(overflow);
  VC_TEST_ASSERT(!overflowResult.ok);
  VC_TEST_ASSERT(overflowResult.error ==
                 CapabilityProfileIOError::JSON_SYNTAX_ERROR);
}

void TestBadSchemaTypeIsRejected() {
  const std::string payload = R"({
    "schemaVersion": "3",
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
    "schemaVersion": 3,
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

void TestDeploymentProfileMissingRequiresProbeAndUsesCpuFallback() {
  const HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-001", "DRV-DEP-001", 111, 222, "device", "1.0.0", "2026-01-01"};
  const auto dirName = "viennaps-profile-missing";
  const auto profilePath = (std::filesystem::temp_directory_path() / dirName /
                            "missing-profile.json")
                               .string();

  std::error_code removeError;
  std::filesystem::remove(profilePath, removeError);

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 4096, false, RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{}, profilePath);
  VC_TEST_ASSERT(decision.requiresProbe);
  VC_TEST_ASSERT(decision.state == DeploymentProfileState::MISSING);
  VC_TEST_ASSERT(!decision.hasProfile);
  VC_TEST_ASSERT(decision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);
}

void TestDeploymentProfileFreshMatchAvoidsProbeAndCanSelectVulkan() {
  const HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-002", "DRV-DEP-002", 333,         444,
      "arc-device",  "1.2.3",       "2026-01-01"};
  const auto dirName = "viennaps-profile-fresh-match";

  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-08-01T00:00:00Z";
  record.hardware = runtimeFingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanFp64SuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.vulkanRayQuery = true;
  record.capabilityProfile.vulkanRayTracingPipeline = false;
  record.capabilityProfile.shaderFloat64 = false;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      1024ULL * 1024ULL * 64ULL;

  writeRecordToTempDir(runtimeFingerprint, record, dirName);

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024ULL * 1024ULL, false,
       RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{},
      (std::filesystem::temp_directory_path() / dirName).string());
  VC_TEST_ASSERT(!decision.requiresProbe);
  VC_TEST_ASSERT(decision.state == DeploymentProfileState::VALID);
  VC_TEST_ASSERT(decision.hasProfile);
  VC_TEST_ASSERT(decision.plan.stages[0].selectedBackend ==
                 ComputeBackend::VULKAN);

  cleanTempProfileIfExists(runtimeFingerprint, dirName);
}

void TestDeploymentProfileMismatchRequiresProbeAndCpuFallback() {
  HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-003", "DRV-DEP-A", 111,         222,
      "arc-device",  "1.2.3",     "2026-01-01"};
  HardwareFingerprint profileFingerprint = runtimeFingerprint;
  profileFingerprint.driverUuid = "DRV-DEP-OLD";
  const auto dirName = "viennaps-profile-mismatch";

  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-08-01T00:00:00Z";
  record.hardware = profileFingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      1024ULL * 1024ULL * 64ULL;

  writeRecordToTempDir(runtimeFingerprint, record, dirName);

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 2048ULL, false, RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{},
      (std::filesystem::temp_directory_path() / dirName).string());
  VC_TEST_ASSERT(decision.requiresProbe);
  VC_TEST_ASSERT(decision.state == DeploymentProfileState::STALE);
  VC_TEST_ASSERT(!decision.hasProfile);
  VC_TEST_ASSERT(decision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);

  cleanTempProfileIfExists(runtimeFingerprint, dirName);
}

void TestDeploymentCorruptedOrUnknownProfileFallsClosedToCpu() {
  const HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-004", "DRV-DEP-004", 1, 1, "device", "1.0.0", "2026-01-01"};
  const auto dir =
      std::filesystem::temp_directory_path() / "viennaps-profile-bad";
  std::filesystem::create_directories(dir);
  const auto corruptedPath = (dir / "corrupt.json").string();
  const auto schemaPath = (dir / "schema.json").string();
  const auto evidencePath = (dir / "evidence.json").string();

  {
    std::ofstream badFile(corruptedPath, std::ios::binary);
    badFile << "{ \"schemaVersion\": 2,";
  }
  {
    std::ofstream schemaFile(schemaPath, std::ios::binary);
    schemaFile
        << R"({"schemaVersion":99,"recordedAt":"2026-08-01T00:00:00Z","hardwareFingerprint":{"deviceUuid":"X","driverUuid":"Y","vendorId":1,"deviceId":2,"deviceName":"x","driverVersion":"1","driverDate":"2026-01-01"},"capabilityProfile":{"cpuAvailable":true,"cudaAvailable":false,"vulkanAvailable":true,"vulkanPrimitiveSuitePass":true,"vulkanFp64SuitePass":true,"vulkanCompute":true,"vulkanRayQuery":false,"vulkanRayTracingPipeline":false,"shaderFloat64":true,"safeVulkanWorkingSetBytes":1024}})";
  }
  {
    std::ofstream evidenceFile(evidencePath, std::ios::binary);
    evidenceFile
        << R"({"schemaVersion":3,"recordedAt":"2026-08-01T00:00:00Z","hardwareFingerprint":{"deviceUuid":"DEV-DEP-004","driverUuid":"DRV-DEP-004","vendorId":1,"deviceId":1,"deviceName":"device","driverVersion":"1.0.0","driverDate":"2026-01-01"},"capabilityProfile":{"cpuAvailable":true,"cudaAvailable":false,"vulkanAvailable":true,"vulkanPrimitiveSuitePass":true,"vulkanFp64SuitePass":false,"vulkanCompute":true,"vulkanRayQuery":false,"vulkanRayTracingPipeline":false,"shaderFloat64":false,"vulkanFp32NumericalSmoke":{"status":"UNKNOWN","contractId":"fp32-bitwise-watchdog-v1","caseCount":1,"mismatchCount":0,"maxUlp":0,"watchdogMs":60000,"elapsedMs":1,"failureDiagnostic":""},"safeVulkanWorkingSetBytes":1024}})";
  }

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP64, 1024ULL, false, RayMode::NONE, true}};
  const auto corruptDecision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{}, corruptedPath);
  VC_TEST_ASSERT(corruptDecision.requiresProbe);
  VC_TEST_ASSERT(corruptDecision.state == DeploymentProfileState::INVALID);
  VC_TEST_ASSERT(!corruptDecision.hasProfile);
  VC_TEST_ASSERT(corruptDecision.profileReadResult.error ==
                 CapabilityProfileIOError::JSON_SYNTAX_ERROR);
  VC_TEST_ASSERT(corruptDecision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);

  const auto schemaDecision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{}, schemaPath);
  VC_TEST_ASSERT(schemaDecision.requiresProbe);
  VC_TEST_ASSERT(schemaDecision.state == DeploymentProfileState::INVALID);
  VC_TEST_ASSERT(!schemaDecision.hasProfile);
  VC_TEST_ASSERT(schemaDecision.profileReadResult.error ==
                 CapabilityProfileIOError::SCHEMA_MISMATCH);
  VC_TEST_ASSERT(schemaDecision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);

  const auto evidenceDecision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{}, evidencePath);
  VC_TEST_ASSERT(evidenceDecision.requiresProbe);
  VC_TEST_ASSERT(evidenceDecision.state == DeploymentProfileState::INVALID);
  VC_TEST_ASSERT(!evidenceDecision.hasProfile);
  VC_TEST_ASSERT(evidenceDecision.profileReadResult.error ==
                 CapabilityProfileIOError::TYPE_MISMATCH);
  VC_TEST_ASSERT(evidenceDecision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);

  std::error_code removeError;
  std::filesystem::remove(corruptedPath, removeError);
  std::filesystem::remove(schemaPath, removeError);
  std::filesystem::remove(evidencePath, removeError);
}

void TestDeploymentManualAlwaysAppliedOverAuto() {
  const HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-005", "DRV-DEP-005", 10, 20, "device", "1.0.0", "2026-01-01"};
  const auto dirName = "viennaps-profile-manual";

  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-08-01T00:00:00Z";
  record.hardware = runtimeFingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanFp64SuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL;

  writeRecordToTempDir(runtimeFingerprint, record, dirName);

  ManualSelectionConfig manualCpu;
  manualCpu.selectionMode = SelectionMode::MANUAL;
  manualCpu.globalBackend = ComputeBackend::CPU;
  const std::vector<StageWorkload> workloads = {{Stage::RAY_TRACING,
                                                 Precision::FP32, 2048ULL,
                                                 false, RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, manualCpu,
      (std::filesystem::temp_directory_path() / dirName).string());
  VC_TEST_ASSERT(decision.plan.ok);
  VC_TEST_ASSERT(decision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);
  VC_TEST_ASSERT(!decision.requiresProbe);

  cleanTempProfileIfExists(runtimeFingerprint, dirName);
}

void TestDeploymentManualVulkanUnsupportedMustFailExplicitly() {
  HardwareFingerprint runtimeFingerprint = {
      "DEV-DEP-006", "DRV-DEP-006", 1, 1, "device", "1.0.0", "2026-01-01"};
  const auto dirName = "viennaps-profile-manual-vulkan";

  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-08-01T00:00:00Z";
  record.hardware = runtimeFingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = false;
  record.capabilityProfile.vulkanPrimitiveSuitePass = false;
  record.capabilityProfile.vulkanCompute = false;
  record.capabilityProfile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL;

  writeRecordToTempDir(runtimeFingerprint, record, dirName);

  ManualSelectionConfig manualVulkan;
  manualVulkan.selectionMode = SelectionMode::MANUAL;
  manualVulkan.globalBackend = ComputeBackend::VULKAN;
  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 2048ULL, false, RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, manualVulkan,
      (std::filesystem::temp_directory_path() / dirName).string());
  assertManualBlockReasonContainsVulkan(decision.plan.stages[0]);

  cleanTempProfileIfExists(runtimeFingerprint, dirName);
}

void TestDeploymentProfileDirectoryFallsBackToDefaultWhenEnvEmpty() {
  const std::string oldEnv =
      getEnvVar(std::string(kDeploymentProfileDirEnvVar));
  setEnvVar(std::string(kDeploymentProfileDirEnvVar), "");

  const auto defaultDir = resolveDeploymentProfileDirectory("");
  VC_TEST_ASSERT(defaultDir == std::filesystem::path(
                                   std::string(kDeploymentProfileDefaultDir)));
  VC_TEST_ASSERT(!defaultDir.is_absolute());

  if (oldEnv.empty()) {
    setEnvVar(std::string(kDeploymentProfileDirEnvVar), "");
  } else {
    setEnvVar(std::string(kDeploymentProfileDirEnvVar), oldEnv);
  }
}

} // namespace

} // namespace viennacore

int main() {
  viennacore::TestRoundTrip();
  viennacore::TestNumericalSmokeEvidenceRoundTrips();
  viennacore::TestLegacySchemaLoadsWithDefaultNumericalEvidence();
  viennacore::TestDuplicateAndOverflowFieldsAreRejected();
  viennacore::TestUnknownFieldsIgnored();
  viennacore::TestBadSchemaTypeIsRejected();
  viennacore::TestMissingRequiredFieldIsRejected();
  viennacore::TestUnknownSchemaVersionIsRejected();
  viennacore::TestCorruptedInputIsRejected();

  viennacore::TestDeploymentProfileMissingRequiresProbeAndUsesCpuFallback();
  viennacore::TestDeploymentProfileFreshMatchAvoidsProbeAndCanSelectVulkan();
  viennacore::TestDeploymentProfileMismatchRequiresProbeAndCpuFallback();
  viennacore::TestDeploymentCorruptedOrUnknownProfileFallsClosedToCpu();
  viennacore::TestDeploymentManualAlwaysAppliedOverAuto();
  viennacore::TestDeploymentManualVulkanUnsupportedMustFailExplicitly();
  viennacore::TestDeploymentProfileDirectoryFallsBackToDefaultWhenEnvEmpty();

  return 0;
}
