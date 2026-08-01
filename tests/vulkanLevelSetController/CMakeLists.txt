project(vulkanLevelSetController LANGUAGES CXX)

if(TARGET viennaps-vulkan-levelset-process-controller-smoke)
  add_dependencies(ViennaPS_Tests
                   viennaps-vulkan-levelset-process-controller-smoke)
  add_test(
    NAME vulkanLevelSetController
    COMMAND $<TARGET_FILE:viennaps-vulkan-levelset-process-controller-smoke>)
endif()
