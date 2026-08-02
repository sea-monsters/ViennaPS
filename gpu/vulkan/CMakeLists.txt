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
    VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OR VIENNAPS_BUILD_VULKAN_PROBE))
  add_subdirectory(runtime)
endif()

if(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OR VIENNAPS_BUILD_VULKAN_PROBE)
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

if(VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  if(VIENNAPS_VULKAN_SDK_AVAILABLE)
    add_subdirectory(surface)
  else()
    message(STATUS
            "[ViennaPS Vulkan Surface] Vulkan SDK unavailable; skipping smoke.")
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
      if(_VIENNAPS_PROBE_UPDATE_SPV AND _VIENNAPS_PROBE_CLASSIFICATION_SPV AND
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
