project(levelSetRebuildHandledMatrix LANGUAGES CXX)

add_executable(${PROJECT_NAME} "${PROJECT_NAME}.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaPS)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})

add_test(NAME ${PROJECT_NAME}-3d COMMAND $<TARGET_FILE:${PROJECT_NAME}> 3d)
add_test(NAME ${PROJECT_NAME}-multi-segment
         COMMAND $<TARGET_FILE:${PROJECT_NAME}> multi-segment)
set_tests_properties(${PROJECT_NAME}-3d ${PROJECT_NAME}-multi-segment
                     PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=2" TIMEOUT 20)
