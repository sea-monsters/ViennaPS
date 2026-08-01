if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(viennaps_vulkan_runtime LANGUAGES CXX)
endif()

if(NOT DEFINED VIENNAPS_ENABLE_VULKAN)
  set(VIENNAPS_ENABLE_VULKAN ON)
endif()
if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PROBE)
  set(VIENNAPS_BUILD_VULKAN_PROBE OFF)
endif()
if(NOT DEFINED VIENNAPS_BUILD_VULKAN_SMOKE)
  set(VIENNAPS_BUILD_VULKAN_SMOKE OFF)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ViennaPSVulkan.cmake)
if(NOT DEFINED VIENNAPS_VULKAN_SDK_AVAILABLE)
  viennaps_configure_vulkan_probe()
endif()

if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${_VIENNAPS_VULKAN_SDK_PATH}" _VIENNAPS_VULKAN_SDK_PATH)
  set(_VIENNAPS_VULKAN_LIBRARY "")
  if(EXISTS "${_VIENNAPS_VULKAN_SDK_PATH}/Include/vulkan/vulkan.h")
    if(EXISTS "${_VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
      set(_VIENNAPS_VULKAN_LIBRARY
          "${_VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
    elseif(EXISTS "${_VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan.lib")
      set(_VIENNAPS_VULKAN_LIBRARY
          "${_VIENNAPS_VULKAN_SDK_PATH}/Lib/vulkan.lib")
    elseif(EXISTS "${_VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan-1.lib")
      set(_VIENNAPS_VULKAN_LIBRARY
          "${_VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan-1.lib")
    elseif(EXISTS "${_VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan.lib")
      set(_VIENNAPS_VULKAN_LIBRARY "${_VIENNAPS_VULKAN_SDK_PATH}/lib/vulkan.lib")
    endif()
  endif()

  if(_VIENNAPS_VULKAN_LIBRARY)
    set(VIENNAPS_VULKAN_SDK_AVAILABLE ON)
    if(NOT TARGET Vulkan::Vulkan)
      add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    endif()
    set_target_properties(Vulkan::Vulkan PROPERTIES
      IMPORTED_LOCATION "${_VIENNAPS_VULKAN_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${_VIENNAPS_VULKAN_SDK_PATH}/Include")
  else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      set(VIENNAPS_VULKAN_SDK_AVAILABLE ON)
    endif()
  endif()
endif()

if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  message(STATUS
          "[ViennaPS Vulkan Runtime] Vulkan SDK unavailable; runtime smoke is not built.")
  message(STATUS
          "[ViennaPS Vulkan Runtime] Set VULKAN_SDK and reconfigure to build runtime smoke.")
  return()
endif()

add_library(viennaps_vulkan_compute_runtime
  vulkan_compute_runtime.cpp
)
target_include_directories(viennaps_vulkan_compute_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(viennaps_vulkan_compute_runtime PRIVATE Vulkan::Vulkan)
target_compile_features(viennaps_vulkan_compute_runtime PRIVATE cxx_std_20)

if(NOT VIENNAPS_BUILD_VULKAN_SMOKE)
  message(STATUS
          "[ViennaPS Vulkan Runtime] Runtime smoke disabled; set "
          "VIENNAPS_BUILD_VULKAN_SMOKE=ON to enable.")
  return()
endif()

set(VULKAN_RUNTIME_SMOKE_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_smoke.comp")
set(VULKAN_RUNTIME_SMOKE_SPV "${CMAKE_CURRENT_BINARY_DIR}/compute_smoke.comp.spv")

find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for Vulkan runtime smoke compile")
if(NOT VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${_VIENNAPS_VULKAN_SDK_PATH}" _VIENNAPS_VULKAN_SDK_PATH)
  if(WIN32)
    set(_VIENNAPS_VULKAN_BIN_DIR "${_VIENNAPS_VULKAN_SDK_PATH}/Bin")
    if(EXISTS "${_VIENNAPS_VULKAN_BIN_DIR}")
      find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
                   NAMES glslc glslangValidator
                   PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
    endif()
  else()
    set(_VIENNAPS_VULKAN_BIN_DIR "${_VIENNAPS_VULKAN_SDK_PATH}/bin")
    if(EXISTS "${_VIENNAPS_VULKAN_BIN_DIR}")
      find_program(VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER
                   NAMES glslc glslangValidator
                   PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
    endif()
  endif()
endif()

if(NOT VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER)
  message(STATUS
          "[ViennaPS Vulkan Runtime] Vulkan SDK detected but no glslc/glslangValidator "
          "was found; skip runtime smoke compile.")
  return()
endif()

if("${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VULKAN_RUNTIME_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER} -V
            ${VULKAN_RUNTIME_SMOKE_SHADER}
            -o ${VULKAN_RUNTIME_SMOKE_SPV}
    DEPENDS ${VULKAN_RUNTIME_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating runtime SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VULKAN_RUNTIME_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_SMOKE_SHADER_COMPILER}
            -o ${VULKAN_RUNTIME_SMOKE_SPV}
            ${VULKAN_RUNTIME_SMOKE_SHADER}
    DEPENDS ${VULKAN_RUNTIME_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating runtime SPIR-V shader.")
endif()

add_custom_target(viennaps-runtime-compute-smoke-spv
  DEPENDS ${VULKAN_RUNTIME_SMOKE_SPV})

string(REPLACE "\\" "/" _VIENNAPS_RUNTIME_SMOKE_SPV_PATH
               "${VULKAN_RUNTIME_SMOKE_SPV}")
add_executable(viennaps-runtime-compute-smoke
  runtime_smoke.cpp
)
add_dependencies(viennaps-runtime-compute-smoke
  viennaps-runtime-compute-smoke-spv)
target_compile_definitions(viennaps-runtime-compute-smoke PRIVATE
  "VIENNAPS_RUNTIME_SMOKE_SPV_PATH=\"${_VIENNAPS_RUNTIME_SMOKE_SPV_PATH}\"")
target_link_libraries(viennaps-runtime-compute-smoke
  PRIVATE
    viennaps_vulkan_compute_runtime
    Vulkan::Vulkan)
target_compile_features(viennaps-runtime-compute-smoke PRIVATE cxx_std_20)
