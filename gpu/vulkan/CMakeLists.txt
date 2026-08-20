if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
  project(viennaps_vulkan_probe LANGUAGES CXX)
  include(CTest)
endif()

# The parent project adds this directory before its general test block.  Enable
# the local CTest registry early so Vulkan smoke tests are still registered in
# an integrated build.
if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  enable_testing()
endif()

set(VULKAN_PROBE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/VulkanProbe.cpp")

if(NOT DEFINED VIENNAPS_ENABLE_VULKAN)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_ENABLE_VULKAN OFF CACHE BOOL "Enable Vulkan support probes when available.")
  else()
    set(VIENNAPS_ENABLE_VULKAN OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PROBE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_PROBE OFF CACHE BOOL "Build the Vulkan device probe utility.")
  else()
    set(VIENNAPS_BUILD_VULKAN_PROBE OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_SMOKE OFF CACHE BOOL "Build the Vulkan compute smoke utility.")
  else()
    set(VIENNAPS_BUILD_VULKAN_SMOKE OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OFF CACHE BOOL
        "Build the Vulkan primitive differential smoke utility.")
  else()
    set(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OFF CACHE BOOL
        "Build the Vulkan Level Set update differential smoke utility.")
  else()
    set(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_RAY_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_RAY_SMOKE OFF CACHE BOOL
        "Build the Vulkan deterministic ray reduction smoke test.")
  else()
    set(VIENNAPS_BUILD_VULKAN_RAY_SMOKE OFF)
  endif()
endif()

if(NOT DEFINED VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE OFF CACHE BOOL
        "Build the Vulkan surface-model differential smoke utility.")
  else()
    set(VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE OFF)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ViennaPSVulkan.cmake")
if(NOT DEFINED VIENNAPS_VULKAN_SDK_AVAILABLE)
  viennaps_configure_vulkan_probe()
endif()

if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${VIENNAPS_VULKAN_SDK_PATH}" VIENNAPS_VULKAN_SDK_PATH)
  message(STATUS "[ViennaPS Vulkan Probe] VULKAN_SDK path candidate: "
                 "${VIENNAPS_VULKAN_SDK_PATH}")
  set(_VIENNAPS_VULKAN_LIBRARY "")
  if(EXISTS "${VIENNAPS_VULKAN_SDK_PATH}/Include/vulkan/vulkan.h")
    if(EXISTS "${VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
      set(_VIENNAPS_VULKAN_LIBRARY "${VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
    elseif(EXISTS "${VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan.lib")
      set(_VIENNAPS_VULKAN_LIBRARY "${VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan.lib")
    elseif(EXISTS "${VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan-1.lib")
      set(_VIENNAPS_VULKAN_LIBRARY "${VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan-1.lib")
    elseif(EXISTS "${VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan.lib")
      set(_VIENNAPS_VULKAN_LIBRARY "${VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan.lib")
    endif()
  endif()

  if(_VIENNAPS_VULKAN_LIBRARY)
    set(VIENNAPS_VULKAN_SDK_AVAILABLE ON)
    if(NOT TARGET Vulkan::Vulkan)
      add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    endif()
    set_target_properties(Vulkan::Vulkan PROPERTIES
      IMPORTED_LOCATION "${_VIENNAPS_VULKAN_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${VIENNAPS_VULKAN_SDK_PATH}/Include")
    message(STATUS "[ViennaPS Vulkan Probe] Vulkan SDK detected by Vulkan headers/libraries "
                   "under ${VIENNAPS_VULKAN_SDK_PATH}.")
  else()
    set(_VIENNAPS_VULKAN_SDK_CMAKE_DIR_HINTS "")
    list(APPEND _VIENNAPS_VULKAN_SDK_CMAKE_DIR_HINTS
         "${VIENNAPS_VULKAN_SDK_PATH}/Lib/cmake/vulkan"
         "${VIENNAPS_VULKAN_SDK_PATH}/lib/cmake/vulkan"
         "${VIENNAPS_VULKAN_SDK_PATH}/share/vulkan"
         "${VIENNAPS_VULKAN_SDK_PATH}/share/vulkan/cmake"
         "${VIENNAPS_VULKAN_SDK_PATH}/share/cmake/vulkan")
    set(_VIENNAPS_VULKAN_SDK_CMAKE_DIR "")
    foreach(_DIR IN LISTS _VIENNAPS_VULKAN_SDK_CMAKE_DIR_HINTS)
      if(EXISTS "${_DIR}")
        set(_VIENNAPS_VULKAN_SDK_CMAKE_DIR "${_DIR}")
        break()
      endif()
    endforeach()
    find_package(Vulkan QUIET HINTS ${_VIENNAPS_VULKAN_SDK_CMAKE_DIR_HINTS})
    if(Vulkan_FOUND)
      set(VIENNAPS_VULKAN_SDK_AVAILABLE ON)
      message(STATUS "[ViennaPS Vulkan Probe] Vulkan SDK detected via VULKAN_SDK path: "
                     "${VIENNAPS_VULKAN_SDK_PATH}.")
    else()
      if(_VIENNAPS_VULKAN_SDK_CMAKE_DIR)
        message(STATUS "[ViennaPS Vulkan Probe] VULKAN_SDK points to "
                       "${VIENNAPS_VULKAN_SDK_PATH}, but Vulkan package lookup still failed.")
      else()
        message(STATUS "[ViennaPS Vulkan Probe] VULKAN_SDK was set, but known Vulkan CMake "
                       "package folders were not found under ${VIENNAPS_VULKAN_SDK_PATH}.")
      endif()
    endif()
  endif()
endif()

set(VULKAN_SMOKE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ComputeSmoke.cpp")
set(VULKAN_SMOKE_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_smoke.comp")
set(VULKAN_SMOKE_SPV "${CMAKE_CURRENT_BINARY_DIR}/compute_smoke.comp.spv")

if(VIENNAPS_VULKAN_SDK_AVAILABLE AND
   (VIENNAPS_ENABLE_VULKAN OR VIENNAPS_BUILD_VULKAN_SMOKE OR
    VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE OR
    VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OR VIENNAPS_BUILD_VULKAN_RAY_SMOKE OR
    VIENNAPS_BUILD_VULKAN_PROBE))
  add_subdirectory(runtime)
endif()

if(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OR
   VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OR VIENNAPS_BUILD_VULKAN_PROBE OR
   VIENNAPS_BUILD_VULKAN_RAY_SMOKE)
  add_subdirectory(primitives)
endif()

if(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OR VIENNAPS_BUILD_VULKAN_PROBE)
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    add_subdirectory(levelset)
  else()
    message(STATUS
            "[ViennaPS Vulkan Level Set] Vulkan SDK unavailable; skipping smoke.")
  endif()
endif()

if(VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE OR
   VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    add_subdirectory(surface)
  else()
    message(STATUS
            "[ViennaPS Vulkan Surface] Vulkan SDK unavailable; skipping smoke.")
  endif()
endif()

if(VIENNAPS_BUILD_VULKAN_RAY_SMOKE OR VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    add_subdirectory(ray)
  else()
    message(STATUS
            "[ViennaPS Vulkan Ray] Vulkan SDK unavailable; skipping ray smoke.")
  endif()
endif()

if(VIENNAPS_BUILD_VULKAN_PROBE)
  add_executable(viennaps-device-probe ${VULKAN_PROBE_SOURCE})
  target_compile_features(viennaps-device-probe PRIVATE cxx_std_20)
  set_target_properties(viennaps-device-probe PROPERTIES OUTPUT_NAME "viennaps-device-probe")
  target_include_directories(viennaps-device-probe
                             PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include/viennaps")
  if(MSVC)
    target_compile_options(viennaps-device-probe PRIVATE /W4)
  else()
    target_compile_options(viennaps-device-probe
                           PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    target_compile_definitions(viennaps-device-probe PRIVATE VIENNAPS_VULKAN_ENABLED)
    target_link_libraries(viennaps-device-probe PRIVATE Vulkan::Vulkan)
    if(TARGET viennaps_vulkan_hrle_compaction AND
       TARGET viennaps_vulkan_levelset_update AND
       TARGET viennaps_vulkan_reduction_scan_primitives)
      # The device probe is also a standalone deployment utility.  Its strict
      # FP32 path must remain buildable when the optional ViennaLS headers are
      # not installed, even though the shader-only level-set targets exist.
      include(CheckIncludeFileCXX)
      set(_VIENNAPS_PROBE_SAVED_REQUIRED_INCLUDES
          "${CMAKE_REQUIRED_INCLUDES}")
      set(_VIENNAPS_PROBE_VIENNAPS_INCLUDES)
      if(TARGET ViennaPS)
        get_target_property(_VIENNAPS_PROBE_VIENNAPS_INCLUDES ViennaPS
                            INTERFACE_INCLUDE_DIRECTORIES)
      endif()
      set(CMAKE_REQUIRED_INCLUDES
          "${_VIENNAPS_PROBE_VIENNAPS_INCLUDES};${CMAKE_CURRENT_LIST_DIR}/../../include/viennaps")
      check_include_file_cxx("hrleDomain.hpp"
                             _VIENNAPS_PROBE_HAS_HRLE_DOMAIN)
      check_include_file_cxx("hrleSparseStarIterator.hpp"
                             _VIENNAPS_PROBE_HAS_HRLE_ITERATOR)
      check_include_file_cxx("lsDomain.hpp"
                             _VIENNAPS_PROBE_HAS_LS_DOMAIN)
      set(CMAKE_REQUIRED_INCLUDES
          "${_VIENNAPS_PROBE_SAVED_REQUIRED_INCLUDES}")
      get_property(_VIENNAPS_PROBE_UPDATE_SPV
                   TARGET viennaps_vulkan_levelset_update
                   PROPERTY VIENNAPS_LEVELSET_UPDATE_SPV)
      get_property(_VIENNAPS_PROBE_CLASSIFICATION_SPV
                   TARGET viennaps_vulkan_levelset_update
                   PROPERTY VIENNAPS_HRLE_CLASSIFICATION_SPV)
      get_property(_VIENNAPS_PROBE_REDUCTION_SPV
                   TARGET viennaps_vulkan_reduction_scan_primitives
                   PROPERTY VIENNAPS_REDUCTION_SCAN_SPV)
      get_property(_VIENNAPS_PROBE_ACTION_FLAGS_SPV
                   TARGET viennaps_vulkan_hrle_compaction
                   PROPERTY VIENNAPS_HRLE_ACTION_FLAGS_SPV)
      get_property(_VIENNAPS_PROBE_COMPACT_SPV
                   TARGET viennaps_vulkan_hrle_compaction
                   PROPERTY VIENNAPS_HRLE_COMPACT_SPV)
      if(_VIENNAPS_PROBE_HAS_HRLE_DOMAIN AND
         _VIENNAPS_PROBE_HAS_HRLE_ITERATOR AND
         _VIENNAPS_PROBE_HAS_LS_DOMAIN AND
         _VIENNAPS_PROBE_UPDATE_SPV AND _VIENNAPS_PROBE_CLASSIFICATION_SPV AND
         _VIENNAPS_PROBE_REDUCTION_SPV AND _VIENNAPS_PROBE_ACTION_FLAGS_SPV AND
         _VIENNAPS_PROBE_COMPACT_SPV)
        foreach(_VIENNAPS_PROBE_PATH IN ITEMS
                _VIENNAPS_PROBE_UPDATE_SPV
                _VIENNAPS_PROBE_CLASSIFICATION_SPV
                _VIENNAPS_PROBE_REDUCTION_SPV
                _VIENNAPS_PROBE_ACTION_FLAGS_SPV
                _VIENNAPS_PROBE_COMPACT_SPV)
          string(REPLACE "\\" "/" ${_VIENNAPS_PROBE_PATH}
                 "${${_VIENNAPS_PROBE_PATH}}")
        endforeach()
        target_compile_definitions(viennaps-device-probe PRIVATE
          VIENNAPS_VULKAN_LEVELSET_SUITE_AVAILABLE
          "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_PROBE_UPDATE_SPV}\""
          "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_PROBE_CLASSIFICATION_SPV}\""
          "VIENNAPS_REDUCTION_SCAN_SPV_PATH=\"${_VIENNAPS_PROBE_REDUCTION_SPV}\""
          "VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH=\"${_VIENNAPS_PROBE_ACTION_FLAGS_SPV}\""
          "VIENNAPS_HRLE_COMPACT_SPV_PATH=\"${_VIENNAPS_PROBE_COMPACT_SPV}\"")
        add_dependencies(viennaps-device-probe
          viennaps-vulkan-levelset-update-spv
          viennaps-vulkan-hrle-classification-spv
          viennaps-vulkan-reduction-scan-smoke-spv
          viennaps-vulkan-hrle-compaction-spv)
        target_link_libraries(viennaps-device-probe PRIVATE
          ViennaPS viennaps_vulkan_levelset_update
          viennaps_vulkan_hrle_compaction)
      else()
        message(STATUS
                "[ViennaPS Vulkan Probe] Optional ViennaLS level-set suite is unavailable; building strict probe only.")
      endif()
    endif()
    message(STATUS "[ViennaPS Vulkan Probe] Vulkan SDK detected; building full probe.")
  else()
    message(STATUS "[ViennaPS Vulkan Probe] Vulkan SDK not available; building diagnostic "
                   "probe stub.")
  endif()
else()
  message(STATUS "[ViennaPS Vulkan Probe] VIENNAPS_BUILD_VULKAN_PROBE is OFF. "
                 "Enable with -DVIENNAPS_BUILD_VULKAN_PROBE=ON.")
endif()

if(VIENNAPS_BUILD_VULKAN_SMOKE)
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
                 NAMES glslc glslangValidator
                 DOC "Shader compiler used for Vulkan smoke compile")
    if(NOT VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
       EXISTS "$ENV{VULKAN_SDK}")
      file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VIENNAPS_VULKAN_SDK_PATH)
      string(STRIP "${VIENNAPS_VULKAN_SDK_PATH}" VIENNAPS_VULKAN_SDK_PATH)
      set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/Bin")
      if(WIN32)
        if(EXISTS "${_VIENNAPS_VULKAN_BIN_DIR}")
          find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
                       NAMES glslc glslangValidator
                       PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
        endif()
      else()
        set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/bin")
        if(EXISTS "${_VIENNAPS_VULKAN_BIN_DIR}")
          find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
                       NAMES glslc glslangValidator
                       PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
        endif()
      endif()
    endif()
    if(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER)
      if("${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
        add_custom_command(
          OUTPUT ${VULKAN_SMOKE_SPV}
          COMMAND ${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER} -V
                  ${VULKAN_SMOKE_SHADER} -o ${VULKAN_SMOKE_SPV}
          DEPENDS ${VULKAN_SMOKE_SHADER}
          WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
          COMMENT "Generating Vulkan compute smoke SPIR-V.")
      else()
        add_custom_command(
          OUTPUT ${VULKAN_SMOKE_SPV}
          COMMAND ${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER}
                  -o ${VULKAN_SMOKE_SPV} ${VULKAN_SMOKE_SHADER}
          DEPENDS ${VULKAN_SMOKE_SHADER}
          WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
          COMMENT "Generating Vulkan compute smoke SPIR-V.")
      endif()
      add_custom_target(viennaps-compute-smoke-spv DEPENDS ${VULKAN_SMOKE_SPV})

      string(REPLACE "\\" "/" VIENNAPS_VULKAN_COMPUTE_SMOKE_SPV_PATH
                     "${VULKAN_SMOKE_SPV}")
      add_executable(viennaps-compute-smoke ${VULKAN_SMOKE_SOURCE})
      add_dependencies(viennaps-compute-smoke viennaps-compute-smoke-spv)
      target_compile_features(viennaps-compute-smoke PRIVATE cxx_std_20)
      target_compile_definitions(
        viennaps-compute-smoke
        PRIVATE
          "VIENNAPS_COMPUTE_SMOKE_SPV_PATH=\"${VIENNAPS_VULKAN_COMPUTE_SMOKE_SPV_PATH}\"")
      target_link_libraries(viennaps-compute-smoke PRIVATE Vulkan::Vulkan)
      message(STATUS "[ViennaPS Vulkan Smoke] Vulkan SDK and shader compiler detected; "
                     "building compute smoke executable.")
    else()
      message(STATUS "[ViennaPS Vulkan Smoke] Vulkan SDK detected but no glslc or "
                     "glslangValidator found; skipping compute smoke target.")
    endif()
  else()
    message(STATUS "[ViennaPS Vulkan Smoke] Vulkan SDK not available; skipping compute smoke "
                   "target.")
  endif()
else()
  message(STATUS "[ViennaPS Vulkan Smoke] VIENNAPS_BUILD_VULKAN_SMOKE is OFF. "
                 "Enable with -DVIENNAPS_BUILD_VULKAN_SMOKE=ON.")
endif()

# The strict-FP32 child uses a dedicated non-experimental FP32 numerical
# shader. This target is wired after the probe target is declared.
if(VIENNAPS_BUILD_VULKAN_PROBE AND TARGET viennaps-device-probe)
  find_program(_VIENNAPS_STRICT_FP32_COMPILER NAMES glslc glslangValidator)
  if(NOT _VIENNAPS_STRICT_FP32_COMPILER)
    if(DEFINED VIENNAPS_VULKAN_SDK_PATH)
      set(_VIENNAPS_STRICT_SDK "${VIENNAPS_VULKAN_SDK_PATH}")
    elseif(DEFINED ENV{VULKAN_SDK})
      file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _VIENNAPS_STRICT_SDK)
    endif()
    if(WIN32)
      if(EXISTS "${_VIENNAPS_STRICT_SDK}/Bin/glslangValidator.exe")
        set(_VIENNAPS_STRICT_FP32_COMPILER
            "${_VIENNAPS_STRICT_SDK}/Bin/glslangValidator.exe" CACHE FILEPATH
            "Strict FP32 shader compiler" FORCE)
      elseif(EXISTS "${_VIENNAPS_STRICT_SDK}/Bin/glslc.exe")
        set(_VIENNAPS_STRICT_FP32_COMPILER
            "${_VIENNAPS_STRICT_SDK}/Bin/glslc.exe" CACHE FILEPATH
            "Strict FP32 shader compiler" FORCE)
      endif()
      if(NOT _VIENNAPS_STRICT_FP32_COMPILER)
        find_program(_VIENNAPS_STRICT_FP32_COMPILER NAMES glslc glslangValidator
                     PATHS "${_VIENNAPS_STRICT_SDK}/Bin")
      endif()
    else()
      find_program(_VIENNAPS_STRICT_FP32_COMPILER NAMES glslc glslangValidator
                   PATHS "${_VIENNAPS_STRICT_SDK}/bin")
    endif()
  endif()
  if(_VIENNAPS_STRICT_FP32_COMPILER)
    set(_VIENNAPS_STRICT_FP32_SHADER
        "${CMAKE_CURRENT_SOURCE_DIR}/strict_fp32/strict_fp32.comp")
    set(_VIENNAPS_STRICT_FP32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/strict_fp32.comp.spv")
    if("${_VIENNAPS_STRICT_FP32_COMPILER}" MATCHES ".*glslangValidator.*")
      add_custom_command(
        OUTPUT ${_VIENNAPS_STRICT_FP32_SPV}
        COMMAND ${_VIENNAPS_STRICT_FP32_COMPILER} -V
                ${_VIENNAPS_STRICT_FP32_SHADER}
                -o ${_VIENNAPS_STRICT_FP32_SPV}
        DEPENDS ${_VIENNAPS_STRICT_FP32_SHADER})
    else()
      add_custom_command(
        OUTPUT ${_VIENNAPS_STRICT_FP32_SPV}
        COMMAND ${_VIENNAPS_STRICT_FP32_COMPILER}
                -o ${_VIENNAPS_STRICT_FP32_SPV}
                ${_VIENNAPS_STRICT_FP32_SHADER}
        DEPENDS ${_VIENNAPS_STRICT_FP32_SHADER})
    endif()
    add_custom_target(viennaps-strict-fp32-spv
                      DEPENDS ${_VIENNAPS_STRICT_FP32_SPV})
    add_dependencies(viennaps-device-probe viennaps-strict-fp32-spv)
    string(REPLACE "\\" "/" _VIENNAPS_PROBE_FP32_SPV_PATH
                   "${_VIENNAPS_STRICT_FP32_SPV}")
    target_compile_definitions(viennaps-device-probe PRIVATE
      "VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH=\"${_VIENNAPS_PROBE_FP32_SPV_PATH}\"")
  else()
    message(STATUS "[ViennaPS strict FP32] No glslc/glslangValidator; strict smoke disabled.")
  endif()
endif()

# The normal build remains source-tree oriented.  A release-facing Vulkan
# payload is opt-in so CPU/no-SDK consumers keep the existing install/export
# contract.  The payload target materializes every shader target available in
# the configured SDK, then installs both the original GLSL and generated SPIR-V
# alongside the reusable runtime library exported by runtime/CMakeLists.txt.
if(VIENNAPS_INSTALL_VULKAN_PAYLOAD)
  if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
    message(FATAL_ERROR
            "VIENNAPS_INSTALL_VULKAN_PAYLOAD requires a Vulkan SDK")
  endif()

  include(GNUInstallDirs)
  set(_VIENNAPS_VULKAN_PAYLOAD_SPV_TARGETS
      viennaps-compute-smoke-spv
      viennaps-strict-fp32-spv
      viennaps-runtime-compute-smoke-spv
      viennaps-vulkan-primitives-smoke-spv
      viennaps-vulkan-elementwise-smoke-spv
      viennaps-vulkan-rng-smoke-spv
      viennaps-vulkan-gather-histogram-spv
      viennaps-vulkan-reduction-scan-spv
      viennaps-vulkan-radix-sort-spv
      viennaps-vulkan-levelset-update-spv
      viennaps-vulkan-wetetch-velocity-spv
      viennaps-vulkan-selective-epitaxy-velocity-spv
      viennaps-vulkan-teos-velocity-spv
      viennaps-vulkan-hrle-classification-spv
      viennaps-vulkan-hrle-compaction-spv
      viennaps-vulkan-coverage-delta-metric-spv
      viennaps-vulkan-graph-diffusion-spv
      viennaps-vulkan-neutral-transport-surface-spv
      viennaps-vulkan-triangle-hit-spv
      viennaps-vulkan-ray-reducer-spv
      viennaps-vulkan-ray-hit-batch-spv
      viennaps-vulkan-ray-flux-fused-spv
      viennaps-vulkan-triangle-hit-device-spv
      viennaps-vulkan-ray-record-compaction-spv
      viennaps-vulkan-ray-record-sort-spv
      viennaps-vulkan-ray-record-radix-sort-spv
      viennaps-vulkan-ray-record-radix-sort-spv-validated
      viennaps-vulkan-ray-surface-reduction-spv
      viennaps-vulkan-triangle-bvh-hit-spv
      viennaps-vulkan-triangle-bvh-refit-spv
      viennaps-vulkan-multibounce-event-spv
      viennaps-vulkan-multibounce-frontier-queue-spv)

  set(_VIENNAPS_VULKAN_PAYLOAD_ACTIVE_TARGETS)
  foreach(_VIENNAPS_PAYLOAD_TARGET IN LISTS
          _VIENNAPS_VULKAN_PAYLOAD_SPV_TARGETS)
    if(TARGET ${_VIENNAPS_PAYLOAD_TARGET})
      list(APPEND _VIENNAPS_VULKAN_PAYLOAD_ACTIVE_TARGETS
           ${_VIENNAPS_PAYLOAD_TARGET})
    endif()
  endforeach()

  if(NOT _VIENNAPS_VULKAN_PAYLOAD_ACTIVE_TARGETS)
    message(FATAL_ERROR
            "Vulkan payload requested but no SPIR-V target was configured")
  endif()

  add_custom_target(viennaps-vulkan-package-payload)
  if(TARGET viennaps_vulkan_compute_runtime)
    add_dependencies(viennaps-vulkan-package-payload
                     viennaps_vulkan_compute_runtime)
  endif()
  add_dependencies(viennaps-vulkan-package-payload
                   ${_VIENNAPS_VULKAN_PAYLOAD_ACTIVE_TARGETS})

  set(_VIENNAPS_VULKAN_PAYLOAD_ROOT
      "${CMAKE_INSTALL_DATADIR}/ViennaPS-${PROJECT_VERSION}/vulkan")
  install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/spv"
    FILES_MATCHING
    PATTERN "*.spv"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/strict_fp32/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/strict_fp32"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/runtime/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/runtime"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/primitives/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/primitives"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/levelset/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/levelset"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/surface/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/surface"
    FILES_MATCHING
    PATTERN "*.comp"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ray/shaders/"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders/ray"
    FILES_MATCHING
    PATTERN "*.comp"
  )

  set(_VIENNAPS_VULKAN_PAYLOAD_MANIFEST
      "${CMAKE_CURRENT_BINARY_DIR}/vulkan-payload-manifest.json")
  file(GENERATE
       OUTPUT "${_VIENNAPS_VULKAN_PAYLOAD_MANIFEST}"
       CONTENT "{\n  \"schema\": 1,\n  \"version\": \"${PROJECT_VERSION}\",\n  \"shaderRoot\": \"${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/shaders\",\n  \"spirvRoot\": \"${_VIENNAPS_VULKAN_PAYLOAD_ROOT}/spv\",\n  \"runtimeTarget\": \"ViennaTools::viennaps_vulkan_compute_runtime\"\n}\n")
  install(
    FILES "${_VIENNAPS_VULKAN_PAYLOAD_MANIFEST}"
    DESTINATION "${_VIENNAPS_VULKAN_PAYLOAD_ROOT}"
  )
endif()
