project(vulkanLevelSetControllerExecution LANGUAGES CXX)

if(TARGET viennaps-vulkan-levelset-process-controller-execution-smoke)
  add_dependencies(
    ViennaPS_Tests viennaps-vulkan-levelset-process-controller-execution-smoke)
  add_test(
    NAME vulkanLevelSetControllerExecution
    COMMAND
      $<TARGET_FILE:viennaps-vulkan-levelset-process-controller-execution-smoke>)
  set_tests_properties(vulkanLevelSetControllerExecution PROPERTIES TIMEOUT 15)
endif()
