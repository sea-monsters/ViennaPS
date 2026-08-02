if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  return()
endif()

set(VIENNAPS_VULKAN_RAY_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ray_reducer.comp")
set(VIENNAPS_VULKAN_RAY_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/ray_reducer.comp.spv")
set(VIENNAPS_VULKAN_TRIANGLE_HIT_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle_hit.comp")
set(VIENNAPS_VULKAN_TRIANGLE_HIT_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/triangle_hit.comp.spv")
set(VIENNAPS_VULKAN_RAY_HIT_BATCH_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ray_hit_batch.comp")
set(VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/ray_hit_batch.comp.spv")

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
    OUTPUT ${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -V
            ${VIENNAPS_VULKAN_TRIANGLE_HIT_SHADER} -o
            ${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV}
    DEPENDS ${VIENNAPS_VULKAN_TRIANGLE_HIT_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan triangle-hit SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -o
            ${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV}
            ${VIENNAPS_VULKAN_TRIANGLE_HIT_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_TRIANGLE_HIT_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan triangle-hit SPIR-V shader.")
endif()
add_custom_target(viennaps-vulkan-triangle-hit-spv
                  DEPENDS ${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV})

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

add_library(viennaps_vulkan_triangle_hit triangle_hit.cpp)
add_dependencies(viennaps_vulkan_triangle_hit viennaps-vulkan-triangle-hit-spv)
target_link_libraries(viennaps_vulkan_triangle_hit PUBLIC
                      viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_triangle_hit PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_triangle_hit PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_triangle_hit PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_triangle_hit PRIVATE
                         -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(viennaps-vulkan-triangle-hit-smoke triangle_hit_smoke.cpp)
add_dependencies(viennaps-vulkan-triangle-hit-smoke
                 viennaps-vulkan-triangle-hit-spv)
target_compile_features(viennaps-vulkan-triangle-hit-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-triangle-hit-smoke PRIVATE /EHsc)
endif()
string(REPLACE "\\" "/" VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH
       "${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV}")
target_compile_definitions(viennaps-vulkan-triangle-hit-smoke PRIVATE
  "VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH=\"${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-triangle-hit-smoke PRIVATE
                      viennaps_vulkan_triangle_hit)
if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  add_test(NAME viennaps-vulkan-triangle-hit-smoke
           COMMAND viennaps-vulkan-triangle-hit-smoke)
endif()

if("${VIENNAPS_VULKAN_RAY_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -V
            ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SHADER} -o
            ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV}
    DEPENDS ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan ray hit batch SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV}
    COMMAND ${VIENNAPS_VULKAN_RAY_SHADER_COMPILER} -o
            ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV}
            ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan ray hit batch SPIR-V shader.")
endif()
add_custom_target(viennaps-vulkan-ray-hit-batch-spv
                  DEPENDS ${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV})

add_library(viennaps_vulkan_ray_hit_batch ray_hit_batch.cpp)
add_dependencies(viennaps_vulkan_ray_hit_batch
                 viennaps-vulkan-ray-hit-batch-spv)
target_link_libraries(viennaps_vulkan_ray_hit_batch PUBLIC
                      viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_ray_hit_batch PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_ray_hit_batch PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_ray_hit_batch PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_ray_hit_batch PRIVATE
                         -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH
       "${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV}")
add_executable(viennaps-vulkan-ray-hit-batch-smoke ray_hit_batch_smoke.cpp)
add_dependencies(viennaps-vulkan-ray-hit-batch-smoke
                 viennaps-vulkan-ray-hit-batch-spv
                 viennaps-vulkan-triangle-hit-spv)
target_compile_features(viennaps-vulkan-ray-hit-batch-smoke PRIVATE cxx_std_20)
target_compile_definitions(viennaps-vulkan-ray-hit-batch-smoke PRIVATE
  "VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH=\"${VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH}\""
  "VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH=\"${VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-ray-hit-batch-smoke PRIVATE
                      viennaps_vulkan_ray_hit_batch
                      viennaps_vulkan_triangle_hit Vulkan::Vulkan)
if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  add_test(NAME viennaps-vulkan-ray-hit-batch-smoke
           COMMAND viennaps-vulkan-ray-hit-batch-smoke)
endif()
