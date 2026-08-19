project(advectionInnerLoopGuard LANGUAGES CXX)

add_executable(${PROJECT_NAME} "${PROJECT_NAME}.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaPS)

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(${PROJECT_NAME} PROPERTIES TIMEOUT 15
                                               ENVIRONMENT "OMP_NUM_THREADS=1")

foreach(scenario IN ITEMS rk2-no-progress rk3-no-progress recovery rk2-max
                          rk3-max)
  string(REPLACE "-" "_" scenario_name "${scenario}")
  add_test(NAME "${PROJECT_NAME}_${scenario_name}"
           COMMAND $<TARGET_FILE:${PROJECT_NAME}> "${scenario}")
  set_tests_properties("${PROJECT_NAME}_${scenario_name}"
                       PROPERTIES TIMEOUT 15
                                  ENVIRONMENT "OMP_NUM_THREADS=1")
endforeach()
