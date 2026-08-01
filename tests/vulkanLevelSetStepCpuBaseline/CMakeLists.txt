project(vulkanLevelSetStepCpuBaseline LANGUAGES CXX)

add_executable(${PROJECT_NAME} "vulkanLevelSetStepCpuBaseline.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaLS ViennaCore)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
