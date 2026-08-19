cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED VIENNAPS_VULKAN_INSTALL_PREFIX OR
   VIENNAPS_VULKAN_INSTALL_PREFIX STREQUAL "")
  message(FATAL_ERROR
          "VIENNAPS_VULKAN_INSTALL_PREFIX must name an installed prefix")
endif()

get_filename_component(_prefix
                       "${VIENNAPS_VULKAN_INSTALL_PREFIX}" ABSOLUTE)
set(_version "4.6.2")
set(_cmake_dir "${_prefix}/share/cmake/ViennaPS-${_version}")
set(_payload_root "${_prefix}/share/ViennaPS-${_version}/vulkan")
set(_config "${_cmake_dir}/ViennaPSConfig.cmake")
set(_targets "${_cmake_dir}/ViennaPSTargets.cmake")

foreach(_required IN ITEMS
        "${_config}"
        "${_targets}"
        "${_payload_root}/vulkan-payload-manifest.json"
        "${_payload_root}/shaders/compute_smoke.comp"
        "${_payload_root}/shaders/runtime/compute_smoke.comp"
        "${_payload_root}/shaders/ray/triangle_hit.comp"
        "${_payload_root}/shaders/levelset/teos_velocity.comp"
        "${_prefix}/include/viennaps-${_version}/vulkan/runtime/compute_session.hpp")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "Vulkan install payload is missing: ${_required}")
  endif()
endforeach()

file(READ "${_payload_root}/vulkan-payload-manifest.json" _manifest)
set(_json_error)
string(JSON _manifest_schema ERROR_VARIABLE _json_error GET "${_manifest}"
       schema)
if(_json_error OR NOT _manifest_schema MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
          "Vulkan install payload manifest has no numeric schema: ${_json_error}")
endif()
if(NOT _manifest_schema EQUAL 1)
  message(FATAL_ERROR
          "Unsupported Vulkan install payload manifest schema: ${_manifest_schema}")
endif()

string(JSON _manifest_version ERROR_VARIABLE _json_error GET "${_manifest}"
       version)
if(_json_error OR NOT _manifest_version STREQUAL "${_version}")
  message(FATAL_ERROR
          "Vulkan install payload manifest version does not match ${_version}")
endif()

foreach(_manifest_key IN ITEMS shaderRoot spirvRoot runtimeTarget)
  string(JSON _manifest_value ERROR_VARIABLE _json_error GET "${_manifest}"
         "${_manifest_key}")
  if(_json_error OR _manifest_value STREQUAL "")
    message(FATAL_ERROR
            "Vulkan install payload manifest is missing ${_manifest_key}")
  endif()
  if(_manifest_value MATCHES "(^|[\\\"[:space:]])[A-Za-z]:[/\\]" OR
     _manifest_value MATCHES "(^|[\\\"[:space:]])/")
    message(FATAL_ERROR
            "Vulkan install payload manifest contains an absolute path in ${_manifest_key}")
  endif()
  set("_manifest_${_manifest_key}" "${_manifest_value}")
endforeach()

if(NOT _manifest_runtimeTarget STREQUAL "ViennaTools::viennaps_vulkan_compute_runtime")
  message(FATAL_ERROR
          "Vulkan install payload manifest runtimeTarget is not the exported runtime")
endif()

string(JSON _manifest_shader_root GET "${_manifest}" shaderRoot)
string(JSON _manifest_spv_root GET "${_manifest}" spirvRoot)
if(NOT IS_DIRECTORY "${_prefix}/${_manifest_shader_root}" OR
   NOT IS_DIRECTORY "${_prefix}/${_manifest_spv_root}")
  message(FATAL_ERROR
          "Vulkan install payload manifest roots do not resolve below the install prefix")
endif()

file(GLOB_RECURSE _spv_files LIST_DIRECTORIES FALSE
     "${_payload_root}/spv/*.spv")
list(LENGTH _spv_files _spv_count)
if(_spv_count LESS 1)
  message(FATAL_ERROR "Vulkan install payload contains no SPIR-V files")
endif()

file(GLOB_RECURSE _shader_files LIST_DIRECTORIES FALSE
     "${_payload_root}/shaders/*.comp")
list(LENGTH _shader_files _shader_count)
if(_shader_count LESS 1)
  message(FATAL_ERROR "Vulkan install payload contains no GLSL shader sources")
endif()

file(GLOB _runtime_libraries
     "${_prefix}/lib/*viennaps_vulkan_compute_runtime*"
     "${_prefix}/bin/*viennaps_vulkan_compute_runtime*")
list(LENGTH _runtime_libraries _runtime_library_count)
if(_runtime_library_count LESS 1)
  message(FATAL_ERROR
          "Installed Vulkan runtime library was not found below ${_prefix}")
endif()

file(READ "${_config}" _config_text)
file(READ "${_targets}" _targets_text)
if(NOT _config_text MATCHES "ViennaCS;Vulkan")
  message(FATAL_ERROR "Installed package config does not require Vulkan")
endif()
if(NOT _targets_text MATCHES "viennaps_vulkan_compute_runtime")
  message(FATAL_ERROR
          "Installed package targets do not export the Vulkan runtime")
endif()

message(STATUS
        "PD5 Vulkan install payload PASS (spv=${_spv_count}, shaders=${_shader_count}, runtime=${_runtime_library_count})")
