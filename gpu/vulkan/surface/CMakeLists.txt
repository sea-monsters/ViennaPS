if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  message(STATUS
          "[ViennaPS Vulkan Surface] Vulkan SDK not available; skipping surface module.")
  return()
endif()

set(VIENNAPS_VULKAN_SURFACE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neutral_transport_surface.comp")
set(VIENNAPS_VULKAN_SURFACE_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/neutral_transport_surface.comp.spv")
set(VIENNAPS_VULKAN_GRAPH_DIFFUSION_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/graph_diffusion.comp")
set(VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/graph_diffusion.comp.spv")
set(VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/coverage_delta_metric.comp")
set(VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/coverage_delta_metric.comp.spv")

find_program(VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for the Vulkan surface module")
if(NOT VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VIENNAPS_VULKAN_SDK_PATH)
  string(STRIP "${VIENNAPS_VULKAN_SDK_PATH}" VIENNAPS_VULKAN_SDK_PATH)
  if(WIN32)
    set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/Bin")
  else()
    set(_VIENNAPS_VULKAN_BIN_DIR "${VIENNAPS_VULKAN_SDK_PATH}/bin")
  endif()
  find_program(VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER
               NAMES glslc glslangValidator PATHS "${_VIENNAPS_VULKAN_BIN_DIR}")
endif()

if(NOT VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER)
  message(STATUS
          "[ViennaPS Vulkan Surface] Vulkan SDK detected but no shader compiler found; skipping surface module.")
  return()
endif()

if("${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER} -V
            ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SHADER} -o
            ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}
    DEPENDS ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan coverage delta metric SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}
            -o ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}
            ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan coverage delta metric SPIR-V shader.")
endif()

add_custom_target(viennaps-vulkan-coverage-delta-metric-spv
  DEPENDS ${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV})

if("${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER} -V
            ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SHADER} -o
            ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}
    DEPENDS ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan graph diffusion SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}
            -o ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}
            ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan graph diffusion SPIR-V shader.")
endif()

add_custom_target(viennaps-vulkan-graph-diffusion-spv
  DEPENDS ${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV})

if("${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_SURFACE_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER} -V
            ${VIENNAPS_VULKAN_SURFACE_SHADER} -o ${VIENNAPS_VULKAN_SURFACE_SPV}
    DEPENDS ${VIENNAPS_VULKAN_SURFACE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan neutral transport surface SPIR-V shader.")
else()
  add_custom_command(
    OUTPUT ${VIENNAPS_VULKAN_SURFACE_SPV}
    COMMAND ${VIENNAPS_VULKAN_SURFACE_SHADER_COMPILER}
            -o ${VIENNAPS_VULKAN_SURFACE_SPV} ${VIENNAPS_VULKAN_SURFACE_SHADER}
    DEPENDS ${VIENNAPS_VULKAN_SURFACE_SHADER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Vulkan neutral transport surface SPIR-V shader.")
endif()

add_custom_target(viennaps-vulkan-neutral-transport-surface-spv
  DEPENDS ${VIENNAPS_VULKAN_SURFACE_SPV})

add_library(viennaps_vulkan_surface neutral_transport_surface.cpp
            neutral_transport_velocity_executor.cpp)
set_property(TARGET viennaps_vulkan_surface
             PROPERTY VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV
                      "${VIENNAPS_VULKAN_SURFACE_SPV}")
add_dependencies(viennaps_vulkan_surface
                 viennaps-vulkan-neutral-transport-surface-spv)
target_link_libraries(viennaps_vulkan_surface
                      PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_surface
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                  "${CMAKE_CURRENT_LIST_DIR}/../../../include/viennaps")
target_compile_features(viennaps_vulkan_surface PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_surface PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_surface
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(viennaps_vulkan_graph_diffusion graph_diffusion.cpp)
set_property(TARGET viennaps_vulkan_graph_diffusion
             PROPERTY VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV
                      "${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}")
add_dependencies(viennaps_vulkan_graph_diffusion
                 viennaps-vulkan-graph-diffusion-spv)
target_link_libraries(viennaps_vulkan_graph_diffusion
                      PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_graph_diffusion
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_graph_diffusion PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_graph_diffusion PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_graph_diffusion
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(viennaps_vulkan_surface_diffusion surface_diffusion_executor.cpp)
target_link_libraries(viennaps_vulkan_surface_diffusion
                      PUBLIC viennaps_vulkan_graph_diffusion Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_surface_diffusion
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                  "${CMAKE_CURRENT_LIST_DIR}/../../../include/viennaps")
target_compile_features(viennaps_vulkan_surface_diffusion PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_surface_diffusion PRIVATE /W4 /WX
                         /EHsc)
else()
  target_compile_options(viennaps_vulkan_surface_diffusion
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(viennaps_vulkan_coverage_delta_metric
            coverage_delta_metric.cpp coverage_delta_executor.cpp)
set_property(TARGET viennaps_vulkan_coverage_delta_metric
             PROPERTY VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV
                      "${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}")
add_dependencies(viennaps_vulkan_coverage_delta_metric
                 viennaps-vulkan-coverage-delta-metric-spv)
target_link_libraries(viennaps_vulkan_coverage_delta_metric
                      PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_coverage_delta_metric
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                  "${CMAKE_CURRENT_LIST_DIR}/../../../include/viennaps")
target_compile_features(viennaps_vulkan_coverage_delta_metric PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_coverage_delta_metric PRIVATE /W4 /WX
                         /EHsc)
else()
  target_compile_options(viennaps_vulkan_coverage_delta_metric
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(viennaps-vulkan-process-deployment-binding-smoke
               process_deployment_binding_smoke.cpp)
add_dependencies(viennaps-vulkan-process-deployment-binding-smoke
                 viennaps-vulkan-graph-diffusion-spv
                 viennaps-vulkan-coverage-delta-metric-spv
                 viennaps-vulkan-neutral-transport-surface-spv)
target_compile_features(viennaps-vulkan-process-deployment-binding-smoke
                         PRIVATE cxx_std_20)
string(REPLACE "\\" "/" VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH
               "${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}")
string(REPLACE "\\" "/" VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH
               "${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}")
string(REPLACE "\\" "/" VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH
               "${VIENNAPS_VULKAN_SURFACE_SPV}")
target_compile_definitions(
  viennaps-vulkan-process-deployment-binding-smoke PRIVATE
  "VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH=\"${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH}\""
  "VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH=\"${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH}\""
  "VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH=\"${VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-process-deployment-binding-smoke
                      PRIVATE ViennaPS viennaps_vulkan_surface_diffusion
                              viennaps_vulkan_coverage_delta_metric
                              viennaps_vulkan_surface Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-process-deployment-binding-smoke
           COMMAND viennaps-vulkan-process-deployment-binding-smoke)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH
               "${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}")
add_executable(viennaps-vulkan-graph-diffusion-smoke
               graph_diffusion_smoke.cpp)
add_dependencies(viennaps-vulkan-graph-diffusion-smoke
                 viennaps-vulkan-graph-diffusion-spv)
target_compile_features(viennaps-vulkan-graph-diffusion-smoke PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-graph-diffusion-smoke PRIVATE
  "VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH=\"${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-graph-diffusion-smoke
                      PRIVATE viennaps_vulkan_graph_diffusion Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-graph-diffusion-smoke
           COMMAND viennaps-vulkan-graph-diffusion-smoke)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH
               "${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV}")
add_executable(viennaps-vulkan-surface-diffusion-executor-smoke
               surface_diffusion_executor_smoke.cpp)
add_dependencies(viennaps-vulkan-surface-diffusion-executor-smoke
                 viennaps-vulkan-graph-diffusion-spv)
target_compile_features(viennaps-vulkan-surface-diffusion-executor-smoke
                         PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-surface-diffusion-executor-smoke PRIVATE
  "VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH=\"${VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-surface-diffusion-executor-smoke
                      PRIVATE viennaps_vulkan_surface_diffusion Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-surface-diffusion-executor-smoke
           COMMAND viennaps-vulkan-surface-diffusion-executor-smoke)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH
               "${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV}")
add_executable(viennaps-vulkan-coverage-delta-metric-smoke
               coverage_delta_metric_smoke.cpp)
add_dependencies(viennaps-vulkan-coverage-delta-metric-smoke
                 viennaps-vulkan-coverage-delta-metric-spv)
target_compile_features(viennaps-vulkan-coverage-delta-metric-smoke PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-coverage-delta-metric-smoke PRIVATE
  "VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH=\"${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-coverage-delta-metric-smoke
                      PRIVATE viennaps_vulkan_coverage_delta_metric Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-coverage-delta-metric-smoke
           COMMAND viennaps-vulkan-coverage-delta-metric-smoke)
endif()

add_executable(viennaps-vulkan-coverage-delta-executor-smoke
               coverage_delta_executor_smoke.cpp)
add_dependencies(viennaps-vulkan-coverage-delta-executor-smoke
                 viennaps-vulkan-coverage-delta-metric-spv)
target_compile_features(viennaps-vulkan-coverage-delta-executor-smoke
                         PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-coverage-delta-executor-smoke PRIVATE
  "VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH=\"${VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-coverage-delta-executor-smoke
                      PRIVATE viennaps_vulkan_coverage_delta_metric Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-coverage-delta-executor-smoke
           COMMAND viennaps-vulkan-coverage-delta-executor-smoke)
endif()

string(REPLACE "\\" "/" VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH
               "${VIENNAPS_VULKAN_SURFACE_SPV}")
add_executable(viennaps-vulkan-neutral-transport-surface-smoke
               neutral_transport_surface_smoke.cpp)
add_dependencies(viennaps-vulkan-neutral-transport-surface-smoke
                 viennaps-vulkan-neutral-transport-surface-spv)
target_compile_features(viennaps-vulkan-neutral-transport-surface-smoke
                         PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-neutral-transport-surface-smoke PRIVATE
  "VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH=\"${VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-neutral-transport-surface-smoke
                      PRIVATE viennaps_vulkan_surface Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-neutral-transport-surface-smoke
           COMMAND viennaps-vulkan-neutral-transport-surface-smoke)
endif()

add_executable(viennaps-vulkan-neutral-transport-velocity-executor-smoke
               neutral_transport_velocity_executor_smoke.cpp)
add_dependencies(viennaps-vulkan-neutral-transport-velocity-executor-smoke
                 viennaps-vulkan-neutral-transport-surface-spv)
target_compile_features(viennaps-vulkan-neutral-transport-velocity-executor-smoke
                         PRIVATE cxx_std_20)
target_compile_definitions(
  viennaps-vulkan-neutral-transport-velocity-executor-smoke PRIVATE
  "VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH=\"${VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-neutral-transport-velocity-executor-smoke
                      PRIVATE viennaps_vulkan_surface Vulkan::Vulkan)

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
  add_test(NAME viennaps-vulkan-neutral-transport-velocity-executor-smoke
           COMMAND viennaps-vulkan-neutral-transport-velocity-executor-smoke)
endif()
