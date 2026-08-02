if(NOT VIENNAPS_VULKAN_SDK_AVAILABLE)
  message(STATUS
          "[ViennaPS Vulkan Surface] Vulkan SDK not available; skipping surface module.")
  return()
endif()

set(VIENNAPS_VULKAN_SURFACE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neutral_transport_surface.comp")
set(VIENNAPS_VULKAN_SURFACE_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/neutral_transport_surface.comp.spv")

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

add_library(viennaps_vulkan_surface neutral_transport_surface.cpp)
add_dependencies(viennaps_vulkan_surface
                 viennaps-vulkan-neutral-transport-surface-spv)
target_link_libraries(viennaps_vulkan_surface
                      PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_include_directories(viennaps_vulkan_surface
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(viennaps_vulkan_surface PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_surface PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_surface
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
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

if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  add_test(NAME viennaps-vulkan-neutral-transport-surface-smoke
           COMMAND viennaps-vulkan-neutral-transport-surface-smoke)
endif()
