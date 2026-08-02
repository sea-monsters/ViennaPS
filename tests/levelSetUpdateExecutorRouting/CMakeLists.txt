project(levelSetUpdateExecutorRouting LANGUAGES CXX)

add_executable(${PROJECT_NAME} "${PROJECT_NAME}.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaPS)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

if(WIN32)
  viennacore_setup_embree_env(${PROJECT_NAME}
                              $<TARGET_FILE_DIR:${PROJECT_NAME}>)
  find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
  add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>)
endif()

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(${PROJECT_NAME} PROPERTIES TIMEOUT 15)

foreach(scenario IN ITEMS fallback-error fallback-throw fallback-invalid
                          strict-error strict-throw strict-invalid process-strict
                          rebuild-fallback rebuild-fallback-error
                          rebuild-fallback-throw rebuild-fallback-invalid
                          rebuild-strict-error rebuild-strict-throw
                          rebuild-strict-invalid)
  string(REPLACE "-" "_" scenario_name "${scenario}")
  add_test(NAME "${PROJECT_NAME}_${scenario_name}"
           COMMAND $<TARGET_FILE:${PROJECT_NAME}> "${scenario}")
  set_tests_properties("${PROJECT_NAME}_${scenario_name}" PROPERTIES TIMEOUT 15)
endforeach()
