project(probeProfileAdapter LANGUAGES CXX)

add_executable(${PROJECT_NAME} "probeProfileAdapter.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ViennaPS)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_dependencies(ViennaPS_Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
