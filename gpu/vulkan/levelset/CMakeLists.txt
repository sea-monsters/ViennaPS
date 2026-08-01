set(VIENNAPS_LEVELSET_UPDATE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/levelset_update.comp")
set(VIENNAPS_LEVELSET_UPDATE_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/levelset_update.comp.spv")

find_program(VIENNAPS_LEVELSET_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for the Vulkan Level Set update kernel")
if(NOT VIENNAPS_LEVELSET_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  if(WIN32)
    set(_VIENNAPS_LEVELSET_SDK_BIN "$ENV{VULKAN_SDK}/Bin")
  else()
    set(_VIENNAPS_LEVELSET_SDK_BIN "$ENV{VULKAN_SDK}/bin")
  endif()
  find_program(VIENNAPS_LEVELSET_SHADER_COMPILER
               NAMES glslc glslangValidator
               PATHS "${_VIENNAPS_LEVELSET_SDK_BIN}"
               NO_DEFAULT_PATH)
endif()

if(NOT VIENNAPS_LEVELSET_SHADER_COMPILER)
  message(STATUS
          "[ViennaPS Vulkan Level Set] no shader compiler found; skipping smoke.")
  return()
endif()

if("${VIENNAPS_LEVELSET_SHADER_COMPILER}" MATCHES "glslangValidator")
  add_custom_command(
    OUTPUT "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}" -V
            "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
            -o "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    COMMENT "Generating Vulkan Level Set update SPIR-V")
else()
  add_custom_command(
    OUTPUT "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}"
            -o "${VIENNAPS_LEVELSET_UPDATE_SPV}"
            "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    COMMENT "Generating Vulkan Level Set update SPIR-V")
endif()

add_custom_target(viennaps-vulkan-levelset-update-spv
                  DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SPV}")

add_library(viennaps_vulkan_levelset_update levelset_update.cpp)
target_include_directories(viennaps_vulkan_levelset_update
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(viennaps_vulkan_levelset_update
  PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_compile_features(viennaps_vulkan_levelset_update PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_levelset_update PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_levelset_update
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" _VIENNAPS_LEVELSET_UPDATE_SPV_PATH
               "${VIENNAPS_LEVELSET_UPDATE_SPV}")
add_executable(viennaps-vulkan-levelset-update-smoke
               levelset_update_smoke.cpp)
add_dependencies(viennaps-vulkan-levelset-update-smoke
                 viennaps-vulkan-levelset-update-spv)
target_compile_definitions(viennaps-vulkan-levelset-update-smoke PRIVATE
  "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_LEVELSET_UPDATE_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-levelset-update-smoke
  PRIVATE viennaps_vulkan_levelset_update)
target_compile_features(viennaps-vulkan-levelset-update-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-levelset-update-smoke PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps-vulkan-levelset-update-smoke
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_test(NAME viennaps-vulkan-levelset-update-smoke
           COMMAND viennaps-vulkan-levelset-update-smoke)
endif()
