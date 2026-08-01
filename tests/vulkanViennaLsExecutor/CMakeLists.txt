if(NOT TARGET viennaps_vulkan_levelset_update OR NOT TARGET ViennaLS)
  return()
endif()

project(vulkanViennaLsExecutor LANGUAGES CXX)

get_target_property(_VIENNAPS_LEVELSET_UPDATE_SPV
                    viennaps_vulkan_levelset_update
                    VIENNAPS_LEVELSET_UPDATE_SPV)
if(NOT _VIENNAPS_LEVELSET_UPDATE_SPV)
  message(FATAL_ERROR "Missing ViennaPS Level Set SPIR-V target property")
endif()
string(REPLACE "\\" "/" _VIENNAPS_LEVELSET_UPDATE_SPV
               "${_VIENNAPS_LEVELSET_UPDATE_SPV}")

add_executable(${PROJECT_NAME} "vulkanViennaLsExecutor.cpp")
add_dependencies(${PROJECT_NAME} viennaps-vulkan-levelset-update-spv)
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_LEVELSET_UPDATE_SPV}\"")
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE viennaps_vulkan_levelset_update ViennaLS ViennaCore)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
