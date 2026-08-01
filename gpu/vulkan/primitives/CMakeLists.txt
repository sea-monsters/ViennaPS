if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
  project(viennaps_vulkan_primitives LANGUAGES CXX)
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
if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE)
  set(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OFF)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ViennaPSVulkan.cmake")
if(NOT DEFINED VIENNAPS_VULKAN_SDK_AVAILABLE)
  viennaps_configure_vulkan_probe()
endif()

if(NOT VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS
            "[ViennaPS Vulkan Primitives] Build target is off. "
            "Enable with -DVIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE=ON.")
  else()
    message(STATUS
            "[ViennaPS Vulkan Primitives] Build target is off by default in parent builds.")
  endif()
  return()
endif()

if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  message(STATUS
          "[ViennaPS Vulkan Primitives] Vulkan SDK not available; skipping primitives smoke.")
  return()
endif()

set(VULKAN_PRIMITIVES_SMOKE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/PrimitivesSmoke.cpp")
set(VULKAN_PRIMITIVES_SMOKE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/primitives.comp")
set(VULKAN_PRIMITIVES_SMOKE_SPV "${CMAKE_CURRENT_BINARY_DIR}/primitives.comp.spv")
set(VULKAN_PRIMITIVES_PROD_SMOKE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/production_primitives_smoke.cpp")
set(VULKAN_PRIMITIVES_PROD_SMOKE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/elementwise.comp")
set(VULKAN_PRIMITIVES_PROD_SMOKE_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/elementwise.comp.spv")

find_program(VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for Vulkan primitives smoke compile")
if(NOT VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${VIENNAPS_VULKAN_SDK_PATH}" VIENNAPS_VULKAN_SDK_PATH)
  if(WIN32)
    set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/Bin")
  else()
    set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/bin")
  endif()
  if(EXISTS "${_VIENNAPS_VULKAN_BIN_DIR}")
    find_program(VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER
                 NAMES glslc glslangValidator
                 PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
  endif()
endif()

if(NOT VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER)
  message(STATUS
          "[ViennaPS Vulkan Primitives] Vulkan SDK detected but no glslc or "
          "glslangValidator was found; skipping primitives smoke.")
  return()
endif()

if("${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER} -V
            ${VULKAN_PRIMITIVES_SMOKE_SHADER}
            -o ${VULKAN_PRIMITIVES_SMOKE_SPV}
    DEPENDS ${VULKAN_PRIMITIVES_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan primitives SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}
            -o ${VULKAN_PRIMITIVES_SMOKE_SPV}
            ${VULKAN_PRIMITIVES_SMOKE_SHADER}
    DEPENDS ${VULKAN_PRIMITIVES_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan primitives SPIR-V shader.")
endif()

add_custom_target(viennaps-vulkan-primitives-smoke-spv
  DEPENDS ${VULKAN_PRIMITIVES_SMOKE_SPV})
add_library(viennaps_vulkan_primitives
  vulkan_primitives.cpp
)
target_link_libraries(viennaps_vulkan_primitives
  PUBLIC
    viennaps_vulkan_compute_runtime
    Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_primitives
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_primitives PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_primitives PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_primitives
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH
               "${VULKAN_PRIMITIVES_SMOKE_SPV}")

add_executable(viennaps-vulkan-primitives-smoke
  ${VULKAN_PRIMITIVES_SMOKE_SOURCE})
add_dependencies(viennaps-vulkan-primitives-smoke
  viennaps-vulkan-primitives-smoke-spv)
target_compile_features(viennaps-vulkan-primitives-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-primitives-smoke PRIVATE /W4)
else()
  target_compile_options(viennaps-vulkan-primitives-smoke
                         PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_definitions(
  viennaps-vulkan-primitives-smoke
  PRIVATE
    "VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH=\"${VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-primitives-smoke PRIVATE Vulkan::Vulkan)

if("${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_PROD_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER} -V
            ${VULKAN_PRIMITIVES_PROD_SMOKE_SHADER}
            -o ${VULKAN_PRIMITIVES_PROD_SMOKE_SPV}
    DEPENDS ${VULKAN_PRIMITIVES_PROD_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan elementwise SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_PROD_SMOKE_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}
            -o ${VULKAN_PRIMITIVES_PROD_SMOKE_SPV}
            ${VULKAN_PRIMITIVES_PROD_SMOKE_SHADER}
    DEPENDS ${VULKAN_PRIMITIVES_PROD_SMOKE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan elementwise SPIR-V shader.")
endif()
add_custom_target(viennaps-vulkan-elementwise-smoke-spv
  DEPENDS ${VULKAN_PRIMITIVES_PROD_SMOKE_SPV})

string(REPLACE "\\" "/"
               VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH
               "${VULKAN_PRIMITIVES_PROD_SMOKE_SPV}")

add_executable(viennaps-vulkan-primitives-production-smoke
  ${VULKAN_PRIMITIVES_PROD_SMOKE_SOURCE})
add_dependencies(viennaps-vulkan-primitives-production-smoke
  viennaps-vulkan-elementwise-smoke-spv)
target_compile_features(viennaps-vulkan-primitives-production-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-primitives-production-smoke
                         PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps-vulkan-primitives-production-smoke
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_compile_definitions(
  viennaps-vulkan-primitives-production-smoke
  PRIVATE
    "VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH=\"${VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-primitives-production-smoke
  PRIVATE
    viennaps_vulkan_primitives
    Vulkan::Vulkan)

set(VULKAN_PRIMITIVES_RNG_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rng.comp")
set(VULKAN_PRIMITIVES_RNG_SPV "${CMAKE_CURRENT_BINARY_DIR}/rng.comp.spv")
if("${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}" MATCHES
   ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_RNG_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER} -V
            ${VULKAN_PRIMITIVES_RNG_SHADER}
            -o ${VULKAN_PRIMITIVES_RNG_SPV}
    DEPENDS ${VULKAN_PRIMITIVES_RNG_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan RNG SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VULKAN_PRIMITIVES_RNG_SPV}
    COMMAND ${VIENNAPS_VULKAN_PRIMITIVES_SMOKE_SHADER_COMPILER}
            -o ${VULKAN_PRIMITIVES_RNG_SPV}
            ${VULKAN_PRIMITIVES_RNG_SHADER}
    DEPENDS ${VULKAN_PRIMITIVES_RNG_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan RNG SPIR-V shader.")
endif()
add_custom_target(viennaps-vulkan-rng-smoke-spv
  DEPENDS ${VULKAN_PRIMITIVES_RNG_SPV})

add_library(viennaps_vulkan_rng_primitives rng_primitives.cpp)
target_link_libraries(viennaps_vulkan_rng_primitives
  PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_rng_primitives
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_rng_primitives PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_rng_primitives PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_rng_primitives
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH
               "${VULKAN_PRIMITIVES_RNG_SPV}")
add_executable(viennaps-vulkan-rng-production-smoke
  rng_production_smoke.cpp)
add_dependencies(viennaps-vulkan-rng-production-smoke
  viennaps-vulkan-rng-smoke-spv)
target_compile_features(viennaps-vulkan-rng-production-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-rng-production-smoke PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps-vulkan-rng-production-smoke
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_compile_definitions(viennaps-vulkan-rng-production-smoke
  PRIVATE
    "VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH=\"${VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-rng-production-smoke
  PRIVATE viennaps_vulkan_rng_primitives Vulkan::Vulkan)

if(BUILD_TESTING)
  add_test(NAME viennaps-vulkan-primitives-smoke
           COMMAND viennaps-vulkan-primitives-smoke)
  add_test(NAME viennaps-vulkan-primitives-production-smoke
           COMMAND viennaps-vulkan-primitives-production-smoke)
  add_test(NAME viennaps-vulkan-rng-production-smoke
           COMMAND viennaps-vulkan-rng-production-smoke)
endif()
