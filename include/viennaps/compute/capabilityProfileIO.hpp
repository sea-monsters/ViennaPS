#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "backendPolicy.hpp"

namespace viennaps::compute {

constexpr std::uint32_t kCapabilityProfileSchemaVersion = 3U;

struct HardwareFingerprint {
  std::string deviceUuid;
  std::string driverUuid;
  std::uint64_t vendorId = 0;
  std::uint64_t deviceId = 0;
  std::string deviceName;
  std::string driverVersion;
  std::string driverDate;
};

struct CapabilityProfileRecord {
  std::uint32_t schemaVersion = kCapabilityProfileSchemaVersion;
  std::string recordedAt;
  HardwareFingerprint hardware;
  CapabilityProfile capabilityProfile{};
};

enum class CapabilityProfileIOError {
  NONE = 0,
  IO_ERROR,
  JSON_SYNTAX_ERROR,
  SCHEMA_MISMATCH,
  MISSING_FIELD,
  TYPE_MISMATCH,
};

struct CapabilityProfileIOResult {
  bool ok = false;
  CapabilityProfileRecord record{};
  CapabilityProfileIOError error = CapabilityProfileIOError::NONE;
  std::string message;
};

[[nodiscard]] inline bool isCapabilityProfileHardwareFingerprintStale(
    const CapabilityProfileRecord &record,
    const HardwareFingerprint &currentHardware) {
  return record.hardware.deviceUuid != currentHardware.deviceUuid ||
         record.hardware.driverUuid != currentHardware.driverUuid ||
         record.hardware.vendorId != currentHardware.vendorId ||
         record.hardware.deviceId != currentHardware.deviceId ||
         record.hardware.deviceName != currentHardware.deviceName ||
         record.hardware.driverVersion != currentHardware.driverVersion ||
         record.hardware.driverDate != currentHardware.driverDate;
}

namespace detail {

enum class JsonValueType { NULL_VALUE, BOOL, NUMBER, STRING, OBJECT };

struct JsonValue {
  JsonValueType type = JsonValueType::NULL_VALUE;
  bool boolValue = false;
  std::uint64_t numberValue = 0;
  std::string stringValue;
  std::vector<std::pair<std::string, JsonValue>> objectMembers;
};

struct JsonParseError {
  bool ok = true;
  std::string message;
  std::size_t position = 0;
};

class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  JsonParseError parse(JsonValue &root) {
    JsonParseError status;
    if (!consumeUtf8Bom()) {
      status.ok = false;
      status.message = "Input contains incomplete UTF-8 BOM.";
      return status;
    }

    skipWhitespace();
    if (!parseValue(root)) {
      return {false, lastError_, pos_};
    }

    skipWhitespace();
    if (pos_ != text_.size()) {
      return {false, "Expected end of JSON document.", pos_};
    }
    return {true};
  }

private:
  bool consumeUtf8Bom() {
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
      pos_ += 3;
    }
    return true;
  }

  void skipWhitespace() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  bool setError(std::string_view message) {
    lastError_ = std::string(message);
    return false;
  }

  bool expect(char c) {
    skipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != c) {
      return setError("Unexpected character.");
    }
    ++pos_;
    return true;
  }

  bool parseValue(JsonValue &value) {
    skipWhitespace();
    if (pos_ >= text_.size()) {
      return setError("Unexpected end of input.");
    }

    const char c = text_[pos_];
    if (c == '{') {
      return parseObject(value);
    }
    if (c == '"') {
      value.type = JsonValueType::STRING;
      return parseString(value.stringValue);
    }
    if (c == 't' || c == 'f') {
      value.type = JsonValueType::BOOL;
      return parseBoolean(value.boolValue);
    }
    if (c == 'n') {
      ++pos_;
      if (pos_ + 2 < text_.size() && text_.substr(pos_, 3) == "ull") {
        pos_ += 3;
        value.type = JsonValueType::NULL_VALUE;
        return true;
      }
      return setError("Invalid null literal.");
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      value.type = JsonValueType::NUMBER;
      return parseNumber(value.numberValue);
    }

    return setError("Unsupported JSON token.");
  }

  bool parseObject(JsonValue &value) {
    if (!expect('{')) {
      return false;
    }
    value.type = JsonValueType::OBJECT;
    value.objectMembers.clear();

    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }

    while (true) {
      skipWhitespace();
      std::string key;
      if (!parseString(key)) {
        return false;
      }

      if (!expect(':')) {
        return false;
      }

      JsonValue memberValue;
      if (!parseValue(memberValue)) {
        return false;
      }
      value.objectMembers.emplace_back(std::move(key), std::move(memberValue));

      skipWhitespace();
      if (pos_ >= text_.size()) {
        return setError("Unexpected end of object.");
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return setError("Expected ',' or '}' in object.");
    }
  }

  bool parseString(std::string &value) {
    value.clear();
    if (!expect('"')) {
      return false;
    }
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c == '\\') {
        if (pos_ >= text_.size()) {
          return setError("Invalid string escape.");
        }
        const char escape = text_[pos_++];
        switch (escape) {
        case '"':
          value.push_back('"');
          break;
        case '\\':
          value.push_back('\\');
          break;
        case '/':
          value.push_back('/');
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) {
            return setError("Invalid unicode escape.");
          }
          pos_ += 4;
          value.push_back('?');
          break;
        }
        default:
          return setError("Unsupported string escape.");
        }
      } else {
        value.push_back(c);
      }
    }
    return setError("Unterminated string.");
  }

  bool parseBoolean(bool &value) {
    if (text_.substr(pos_, 4) == "true") {
      value = true;
      pos_ += 4;
      return true;
    }
    if (text_.substr(pos_, 5) == "false") {
      value = false;
      pos_ += 5;
      return true;
    }
    return setError("Invalid boolean token.");
  }

  bool parseNumber(std::uint64_t &value) {
    value = 0;
    if (pos_ >= text_.size() ||
        !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      return setError("Expected number.");
    }
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        break;
      }
      const auto digit = static_cast<std::uint64_t>(c - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL)
        return setError("Number exceeds uint64 range.");
      value = value * 10ULL + digit;
      ++pos_;
    }
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string lastError_;
};

[[nodiscard]] inline bool jsonObjectGet(const JsonValue &object,
                                        std::string_view key,
                                        const JsonValue *&member) {
  if (object.type != JsonValueType::OBJECT) {
    return false;
  }
  for (const auto &entry : object.objectMembers) {
    if (entry.first == key) {
      member = &entry.second;
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline bool
validateObjectMembers(const JsonValue &object,
                      std::initializer_list<std::string_view> allowed,
                      std::string &error) {
  if (object.type != JsonValueType::OBJECT) {
    error = "Expected JSON object.";
    return false;
  }
  for (std::size_t i = 0U; i < object.objectMembers.size(); ++i) {
    const auto &name = object.objectMembers[i].first;
    bool known = false;
    for (const auto candidate : allowed) {
      if (name == candidate) {
        known = true;
        break;
      }
    }
    if (!known) {
      error = "Unknown JSON field: " + name;
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (object.objectMembers[j].first == name) {
        error = "Duplicate JSON field: " + name;
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] inline bool jsonToUint64(const JsonValue *member,
                                       std::uint64_t &value) {
  if (!member || member->type != JsonValueType::NUMBER) {
    return false;
  }
  value = member->numberValue;
  return true;
}

[[nodiscard]] inline bool jsonToBool(const JsonValue *member, bool &value) {
  if (!member || member->type != JsonValueType::BOOL) {
    return false;
  }
  value = member->boolValue;
  return true;
}

[[nodiscard]] inline bool jsonToString(const JsonValue *member,
                                       std::string &value) {
  if (!member || member->type != JsonValueType::STRING) {
    return false;
  }
  value = member->stringValue;
  return true;
}

[[nodiscard]] inline std::string jsonEscape(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (const char c : input) {
    switch (c) {
    case '\"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      output.push_back(c);
      break;
    }
  }
  return output;
}

} // namespace detail

[[nodiscard]] inline std::string toJson(const CapabilityProfileRecord &record) {
  const auto &profile = record.capabilityProfile;
  std::ostringstream out;
  out << "{\n";
  out << "  \"schemaVersion\": " << record.schemaVersion << ",\n";
  out << "  \"recordedAt\": \"" << detail::jsonEscape(record.recordedAt)
      << "\",\n";
  out << "  \"hardwareFingerprint\": {\n";
  out << "    \"deviceUuid\": \""
      << detail::jsonEscape(record.hardware.deviceUuid) << "\",\n";
  out << "    \"driverUuid\": \""
      << detail::jsonEscape(record.hardware.driverUuid) << "\",\n";
  out << "    \"vendorId\": " << record.hardware.vendorId << ",\n";
  out << "    \"deviceId\": " << record.hardware.deviceId << ",\n";
  out << "    \"deviceName\": \""
      << detail::jsonEscape(record.hardware.deviceName) << "\",\n";
  out << "    \"driverVersion\": \""
      << detail::jsonEscape(record.hardware.driverVersion) << "\",\n";
  out << "    \"driverDate\": \""
      << detail::jsonEscape(record.hardware.driverDate) << "\"\n";
  out << "  },\n";
  out << "  \"capabilityProfile\": {\n";
  out << "    \"cpuAvailable\": " << (profile.cpuAvailable ? "true" : "false")
      << ",\n";
  out << "    \"cudaAvailable\": " << (profile.cudaAvailable ? "true" : "false")
      << ",\n";
  out << "    \"vulkanAvailable\": "
      << (profile.vulkanAvailable ? "true" : "false") << ",\n";
  out << "    \"vulkanPrimitiveSuitePass\": "
      << (profile.vulkanPrimitiveSuitePass ? "true" : "false") << ",\n";
  out << "    \"vulkanFp64SuitePass\": "
      << (profile.vulkanFp64SuitePass ? "true" : "false") << ",\n";
  out << "    \"vulkanCompute\": " << (profile.vulkanCompute ? "true" : "false")
      << ",\n";
  out << "    \"vulkanRayQuery\": "
      << (profile.vulkanRayQuery ? "true" : "false") << ",\n";
  out << "    \"vulkanRayTracingPipeline\": "
      << (profile.vulkanRayTracingPipeline ? "true" : "false") << ",\n";
  out << "    \"shaderFloat64\": " << (profile.shaderFloat64 ? "true" : "false")
      << ",\n";
  const auto smokeStatus = [](const VulkanNumericalSmokeStatus status) {
    switch (status) {
    case VulkanNumericalSmokeStatus::PASS:
      return "PASS";
    case VulkanNumericalSmokeStatus::FAIL:
      return "FAIL";
    case VulkanNumericalSmokeStatus::NOT_RUN:
    default:
      return "NOT_RUN";
    }
  };
  if (record.schemaVersion >= 3U)
    out << "    \"vulkanFp32NumericalSmoke\": {\n";
  if (record.schemaVersion >= 3U) {
    out << "      \"status\": \""
        << smokeStatus(profile.vulkanFp32NumericalSmoke.status) << "\",\n";
    out << "      \"contractId\": \""
        << detail::jsonEscape(profile.vulkanFp32NumericalSmoke.contractId)
        << "\",\n";
    out << "      \"caseCount\": " << profile.vulkanFp32NumericalSmoke.caseCount
        << ",\n";
    out << "      \"mismatchCount\": "
        << profile.vulkanFp32NumericalSmoke.mismatchCount << ",\n";
    out << "      \"maxUlp\": " << profile.vulkanFp32NumericalSmoke.maxUlp
        << ",\n";
    out << "      \"watchdogMs\": "
        << profile.vulkanFp32NumericalSmoke.watchdogMs << ",\n";
    out << "      \"elapsedMs\": " << profile.vulkanFp32NumericalSmoke.elapsedMs
        << ",\n";
    out << "      \"failureDiagnostic\": \""
        << detail::jsonEscape(
               profile.vulkanFp32NumericalSmoke.failureDiagnostic)
        << "\"\n";
    out << "    },\n";
  }
  out << "    \"safeVulkanWorkingSetBytes\": "
      << profile.safeVulkanWorkingSetBytes << "\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

[[nodiscard]] inline bool
writeCapabilityProfileRecordToFile(std::string_view filePath,
                                   const CapabilityProfileRecord &record,
                                   std::string *error = nullptr) {
  const auto targetPath = std::filesystem::path(std::string(filePath));
  if (targetPath.empty()) {
    if (error) {
      *error = "Profile file path must be non-empty.";
    }
    return false;
  }

  const auto nowTicks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto threadSuffix =
      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const auto tempPath = targetPath.parent_path() /
                        (targetPath.filename().string() + ".tmp-" +
                         std::to_string(nowTicks) + "-" + threadSuffix);

  std::ofstream out(tempPath, std::ios::binary);
  if (!out) {
    if (error) {
      *error = "Failed to open temporary profile file for writing: " +
               tempPath.string();
    }
    return false;
  }
  out << toJson(record);
  if (!out.good()) {
    out.close();
    std::error_code cleanupEc;
    std::filesystem::remove(tempPath, cleanupEc);
    if (error) {
      *error =
          "Failed while writing temporary profile file: " + tempPath.string();
    }
    return false;
  }

  out.flush();
  if (!out.good()) {
    out.close();
    std::error_code cleanupEc;
    std::filesystem::remove(tempPath, cleanupEc);
    if (error) {
      *error =
          "Failed while flushing temporary profile file: " + tempPath.string();
    }
    return false;
  }
  out.close();

  std::error_code replaceEc;
  std::filesystem::rename(tempPath, targetPath, replaceEc);
  if (replaceEc) {
    if (std::filesystem::exists(targetPath)) {
      std::error_code removeEc;
      std::filesystem::remove(targetPath, removeEc);
      if (removeEc) {
        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        if (error) {
          *error =
              "Failed to replace existing profile file: " + removeEc.message();
        }
        return false;
      }
    }
    std::filesystem::rename(tempPath, targetPath, replaceEc);
  }
  if (replaceEc) {
    std::error_code cleanupEc;
    std::filesystem::remove(tempPath, cleanupEc);
    if (error) {
      *error = "Failed to replace profile file: " + replaceEc.message();
    }
    return false;
  }
  return true;
}

[[nodiscard]] inline std::string
loadTextFile(std::string_view filePath, CapabilityProfileIOResult &result) {
  std::ifstream file(std::string(filePath), std::ios::binary);
  if (!file) {
    result.ok = false;
    result.error = CapabilityProfileIOError::IO_ERROR;
    result.message =
        "Failed to open file for reading: " + std::string(filePath);
    return {};
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.good()) {
    result.ok = false;
    result.error = CapabilityProfileIOError::IO_ERROR;
    result.message = "Failed to read file: " + std::string(filePath);
    return {};
  }
  result.ok = true;
  return buffer.str();
}

[[nodiscard]] inline CapabilityProfileIOResult
parseCapabilityProfileRecord(std::string_view jsonText) {
  CapabilityProfileIOResult out;
  detail::JsonValue root;
  detail::JsonParser parser(jsonText);
  const auto parsed = parser.parse(root);
  if (!parsed.ok) {
    out.ok = false;
    out.error = CapabilityProfileIOError::JSON_SYNTAX_ERROR;
    out.message = "JSON syntax error at byte " +
                  std::to_string(parsed.position) + ": " + parsed.message;
    return out;
  }

  if (root.type != detail::JsonValueType::OBJECT) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = "Top-level JSON value must be an object.";
    return out;
  }
  std::string memberError;
  if (!detail::validateObjectMembers(root,
                                     {"schemaVersion", "recordedAt",
                                      "hardwareFingerprint",
                                      "capabilityProfile"},
                                     memberError)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = memberError;
    return out;
  }

  const detail::JsonValue *member = nullptr;
  std::uint64_t schemaVersion = 0;
  if (!detail::jsonObjectGet(root, "schemaVersion", member)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message = "Missing or invalid required field: schemaVersion.";
    return out;
  }
  if (!detail::jsonToUint64(member, schemaVersion)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::TYPE_MISMATCH;
    out.message = "schemaVersion must be a number.";
    return out;
  }
  if (schemaVersion == 0U || schemaVersion > kCapabilityProfileSchemaVersion) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = "Unsupported schemaVersion: " + std::to_string(schemaVersion);
    return out;
  }

  out.record.schemaVersion = static_cast<std::uint32_t>(schemaVersion);

  if (!detail::jsonObjectGet(root, "recordedAt", member)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message = "Missing required field: recordedAt.";
    return out;
  }
  if (!detail::jsonToString(member, out.record.recordedAt)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::TYPE_MISMATCH;
    out.message = "recordedAt must be a string.";
    return out;
  }

  if (!detail::jsonObjectGet(root, "hardwareFingerprint", member) ||
      member->type != detail::JsonValueType::OBJECT) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message = "Missing required object: hardwareFingerprint.";
    return out;
  }
  if (!detail::validateObjectMembers(*member,
                                     {"deviceUuid", "driverUuid", "vendorId",
                                      "deviceId", "deviceName", "driverVersion",
                                      "driverDate"},
                                     memberError)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = memberError;
    return out;
  }

  const detail::JsonValue *hwMember = nullptr;
  const auto requireUint = [&](const char *name,
                               std::uint64_t &target) -> bool {
    if (!detail::jsonObjectGet(*member, name, hwMember)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::MISSING_FIELD;
      out.message =
          std::string("Missing required field: hardwareFingerprint.") + name +
          ".";
      return false;
    }
    if (!detail::jsonToUint64(hwMember, target)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          std::string("hardwareFingerprint.") + name + " must be a number.";
      return false;
    }
    return true;
  };
  const auto requireString = [&](const char *name,
                                 std::string &target) -> bool {
    if (!detail::jsonObjectGet(*member, name, hwMember)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::MISSING_FIELD;
      out.message =
          std::string("Missing required field: hardwareFingerprint.") + name +
          ".";
      return false;
    }
    if (!detail::jsonToString(hwMember, target)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          std::string("hardwareFingerprint.") + name + " must be a string.";
      return false;
    }
    return true;
  };

  if (!requireString("deviceUuid", out.record.hardware.deviceUuid) ||
      !requireString("driverUuid", out.record.hardware.driverUuid) ||
      !requireUint("vendorId", out.record.hardware.vendorId) ||
      !requireUint("deviceId", out.record.hardware.deviceId) ||
      !requireString("deviceName", out.record.hardware.deviceName) ||
      !requireString("driverVersion", out.record.hardware.driverVersion) ||
      !requireString("driverDate", out.record.hardware.driverDate)) {
    return out;
  }

  const detail::JsonValue *profileMember = nullptr;
  if (!detail::jsonObjectGet(root, "capabilityProfile", profileMember) ||
      profileMember->type != detail::JsonValueType::OBJECT) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message = "Missing required object: capabilityProfile.";
    return out;
  }
  if (!detail::validateObjectMembers(
          *profileMember,
          schemaVersion >= 3U
              ? std::initializer_list<
                    std::string_view>{"cpuAvailable", "cudaAvailable",
                                      "vulkanAvailable",
                                      "vulkanPrimitiveSuitePass",
                                      "vulkanFp64SuitePass", "vulkanCompute",
                                      "vulkanRayQuery",
                                      "vulkanRayTracingPipeline",
                                      "shaderFloat64",
                                      "vulkanFp32NumericalSmoke",
                                      "safeVulkanWorkingSetBytes"}
              : std::initializer_list<
                    std::string_view>{"cpuAvailable", "cudaAvailable",
                                      "vulkanAvailable",
                                      "vulkanPrimitiveSuitePass",
                                      "vulkanFp64SuitePass", "vulkanCompute",
                                      "vulkanRayQuery",
                                      "vulkanRayTracingPipeline",
                                      "shaderFloat64",
                                      "safeVulkanWorkingSetBytes"},
          memberError)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = memberError;
    return out;
  }

  auto &profile = out.record.capabilityProfile;
  const detail::JsonValue *field = nullptr;

  const auto requireBool = [&](const char *name, bool &target) -> bool {
    if (!detail::jsonObjectGet(*profileMember, name, field)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::MISSING_FIELD;
      out.message = std::string("Missing required field: ") +
                    "capabilityProfile." + name + ".";
      return false;
    }
    if (!detail::jsonToBool(field, target)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          std::string("capabilityProfile.") + name + " must be a boolean.";
      return false;
    }
    return true;
  };

  if (!requireBool("cpuAvailable", profile.cpuAvailable) ||
      !requireBool("cudaAvailable", profile.cudaAvailable) ||
      !requireBool("vulkanAvailable", profile.vulkanAvailable) ||
      !requireBool("vulkanPrimitiveSuitePass",
                   profile.vulkanPrimitiveSuitePass) ||
      !requireBool("vulkanFp64SuitePass", profile.vulkanFp64SuitePass) ||
      !requireBool("vulkanCompute", profile.vulkanCompute) ||
      !requireBool("vulkanRayQuery", profile.vulkanRayQuery) ||
      !requireBool("vulkanRayTracingPipeline",
                   profile.vulkanRayTracingPipeline) ||
      !requireBool("shaderFloat64", profile.shaderFloat64)) {
    return out;
  }

  const detail::JsonValue *smokeMember = nullptr;
  if (schemaVersion >= 3U &&
      (!detail::jsonObjectGet(*profileMember, "vulkanFp32NumericalSmoke",
                              smokeMember) ||
       smokeMember->type != detail::JsonValueType::OBJECT)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message =
        "Missing required object: capabilityProfile.vulkanFp32NumericalSmoke.";
    return out;
  }
  if (schemaVersion >= 3U &&
      !detail::validateObjectMembers(*smokeMember,
                                     {"status", "contractId", "caseCount",
                                      "mismatchCount", "maxUlp", "watchdogMs",
                                      "elapsedMs", "failureDiagnostic"},
                                     memberError)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::SCHEMA_MISMATCH;
    out.message = memberError;
    return out;
  }
  if (schemaVersion >= 3U) {
    auto &smoke = profile.vulkanFp32NumericalSmoke;
    const auto requireSmokeUint = [&](const char *name,
                                      std::uint64_t &target) -> bool {
      if (!detail::jsonObjectGet(*smokeMember, name, field)) {
        out.ok = false;
        out.error = CapabilityProfileIOError::MISSING_FIELD;
        out.message = std::string("Missing required field: ") +
                      "capabilityProfile.vulkanFp32NumericalSmoke." + name +
                      ".";
        return false;
      }
      if (!detail::jsonToUint64(field, target)) {
        out.ok = false;
        out.error = CapabilityProfileIOError::TYPE_MISMATCH;
        out.message =
            std::string("capabilityProfile.vulkanFp32NumericalSmoke.") + name +
            " must be an unsigned number.";
        return false;
      }
      return true;
    };
    std::string smokeStatus;
    if (!detail::jsonObjectGet(*smokeMember, "status", field) ||
        !detail::jsonToString(field, smokeStatus)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          "capabilityProfile.vulkanFp32NumericalSmoke.status must be a string.";
      return out;
    }
    if (smokeStatus == "NOT_RUN")
      smoke.status = VulkanNumericalSmokeStatus::NOT_RUN;
    else if (smokeStatus == "PASS")
      smoke.status = VulkanNumericalSmokeStatus::PASS;
    else if (smokeStatus == "FAIL")
      smoke.status = VulkanNumericalSmokeStatus::FAIL;
    else {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          "capabilityProfile.vulkanFp32NumericalSmoke.status is invalid.";
      return out;
    }
    if (!detail::jsonObjectGet(*smokeMember, "contractId", field) ||
        !detail::jsonToString(field, smoke.contractId)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message = "capabilityProfile.vulkanFp32NumericalSmoke.contractId "
                    "must be a string.";
      return out;
    }
    std::uint64_t smokeValue = 0;
    if (!requireSmokeUint("caseCount", smokeValue))
      return out;
    if (smokeValue > std::numeric_limits<std::uint32_t>::max()) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          "capabilityProfile.vulkanFp32NumericalSmoke.caseCount is too large.";
      return out;
    }
    smoke.caseCount = static_cast<std::uint32_t>(smokeValue);
    if (!requireSmokeUint("mismatchCount", smokeValue))
      return out;
    if (smokeValue > std::numeric_limits<std::uint32_t>::max()) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message = "capabilityProfile.vulkanFp32NumericalSmoke.mismatchCount "
                    "is too large.";
      return out;
    }
    smoke.mismatchCount = static_cast<std::uint32_t>(smokeValue);
    if (!requireSmokeUint("maxUlp", smokeValue))
      return out;
    if (smokeValue > std::numeric_limits<std::uint32_t>::max()) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          "capabilityProfile.vulkanFp32NumericalSmoke.maxUlp is too large.";
      return out;
    }
    smoke.maxUlp = static_cast<std::uint32_t>(smokeValue);
    if (!requireSmokeUint("watchdogMs", smoke.watchdogMs) ||
        !requireSmokeUint("elapsedMs", smoke.elapsedMs))
      return out;
    if (!detail::jsonObjectGet(*smokeMember, "failureDiagnostic", field) ||
        !detail::jsonToString(field, smoke.failureDiagnostic)) {
      out.ok = false;
      out.error = CapabilityProfileIOError::TYPE_MISMATCH;
      out.message =
          "capabilityProfile.vulkanFp32NumericalSmoke.failureDiagnostic "
          "must be a string.";
      return out;
    }
  }

  if (!detail::jsonObjectGet(*profileMember, "safeVulkanWorkingSetBytes",
                             field)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::MISSING_FIELD;
    out.message =
        "Missing required field: capabilityProfile.safeVulkanWorkingSetBytes.";
    return out;
  }

  std::uint64_t workingSetBytes = 0;
  if (!detail::jsonToUint64(field, workingSetBytes)) {
    out.ok = false;
    out.error = CapabilityProfileIOError::TYPE_MISMATCH;
    out.message = "capabilityProfile.safeVulkanWorkingSetBytes must be an "
                  "unsigned number.";
    return out;
  }
  profile.safeVulkanWorkingSetBytes = workingSetBytes;

  out.ok = true;
  out.error = CapabilityProfileIOError::NONE;
  return out;
}

[[nodiscard]] inline CapabilityProfileIOResult
loadCapabilityProfileRecordFromFile(std::string_view filePath) {
  CapabilityProfileIOResult result;
  const auto jsonText = loadTextFile(filePath, result);
  if (!result.ok && result.error != CapabilityProfileIOError::NONE) {
    return result;
  }
  return parseCapabilityProfileRecord(jsonText);
}

} // namespace viennaps::compute
