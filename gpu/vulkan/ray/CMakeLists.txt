if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  return()
endif()

set(VIENNAPS_VULKAN_RAY_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ray_reducer.comp")
set(VIENNAPS_VULKAN_RAY_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/ray_reducer.comp.spv")

find_program(VIENNAPS_VULKAN_RAY_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for the Vulkan ray reducer")
if(NOT VIENNAPS_VULKAN_RAY_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${VIENNAPS_VULKAN_SDK_PATH}" VIENNAPS_VULKAN_SDK_PATH)
  if(WIN32)
    set(_VIENNAPS_VULKAN_RAY_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/Bin")
  else()
    set(_VIENNAPS_VULKAN_RAY_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/bin")
  endif()
  find_program(VIENNAPS_VULKAN_RAY_SHADER_COMPILER
               NAMES glslc glslangValidator PATHS "${_VIENNAPS_VULKAN_RAY_BIN_DIR}")
endif()

if(NOT VIENNAPS_VULKAN_RAY_SHADER_COMPILER)
  message(STATUS "[ViennaPS Vulkan Ray] no GLSL compiler; skipping ray smoke.")
  return()
endif()

if("${VIENNAPS_VULKAN_RAY_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_RAY_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -V ${VIENNAPS_VULKAN_RAY_SHADER}
            -o ${VIENNAPS_VULKAN_RAY_SPV}
    DEPENDS ${VIENNAPS_VULKAN_RAY_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating deterministic Vulkan ray reducer SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_RAY_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -o ${VIENNAPS_VULKAN_RAY_SPV}
            ${VIENNAPS_VULKAN_RAY_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_RAY_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating deterministic Vulkan ray reducer SPIR-V shader.")
endif()

add_custom_target(viennaps-vulkan-ray-reducer-spv DEPENDS ${VIENNAPS_VULKAN_RAY_SPV})
add_library(viennaps_vulkan_ray_reducer ray_reducer.cpp)
add_dependencies(viennaps_vulkan_ray_reducer viennaps-vulkan-ray-reducer-spv)
target_link_libraries(viennaps_vulkan_ray_reducer
                      PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_ray_reducer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_ray_reducer PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_ray_reducer PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_ray_reducer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_RAY_SPV_PATH "${VIENNAPS_VULKAN_RAY_SPV}")
add_executable(viennaps-vulkan-ray-reducer-smoke ray_reducer_smoke.cpp)
add_dependencies(viennaps-vulkan-ray-reducer-smoke viennaps-vulkan-ray-reducer-spv)
target_compile_features(viennaps-vulkan-ray-reducer-smoke PRIVATE cxx_std_20)
target_compile_definitions(viennaps-vulkan-ray-reducer-smoke PRIVATE
  "VIENNAPS_VULKAN_RAY_SPV_PATH=\"${VIENNAPS_VULKAN_RAY_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-ray-reducer-smoke PRIVATE
                      viennaps_vulkan_ray_reducer Vulkan::Vulkan)
if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  add_test(NAME viennaps-vulkan-ray-reducer-smoke
           COMMAND viennaps-vulkan-ray-reducer-smoke)
endif()
