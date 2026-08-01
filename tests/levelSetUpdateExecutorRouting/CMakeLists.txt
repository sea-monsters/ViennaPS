project(levelSetUpdateExecutorRouting LANGUAGES CXX)

add_executable(${PROJECT_NAME} "${PROJECT_NAME}.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaPS)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(${PROJECT_NAME} PROPERTIES TIMEOUT 15)

foreach(scenario IN ITEMS fallback-error fallback-throw fallback-invalid
                          strict-error strict-throw strict-invalid)
  string(REPLACE "-" "_" scenario_name "${scenario}")
  add_test(NAME "${PROJECT_NAME}_${scenario_name}"
           COMMAND $<TARGET_FILE:${PROJECT_NAME}> "${scenario}")
  set_tests_properties("${PROJECT_NAME}_${scenario_name}" PROPERTIES TIMEOUT 15)
endforeach()
