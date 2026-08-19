project(neutralCpuReferenceDifferential LANGUAGES CXX)

option(VIENNAPS_NEUTRAL_ORACLE_MOD "Enable the Mod-only executor adapter" OFF)

add_executable(neutral_cpu_oracle_fixture neutral_cpu_oracle_fixture.cpp
               neutral_cpu_oracle_process.cpp)
target_link_libraries(neutral_cpu_oracle_fixture PRIVATE ViennaPS)
target_compile_features(neutral_cpu_oracle_fixture PRIVATE cxx_std_20)
if(VIENNAPS_NEUTRAL_ORACLE_MOD)
  target_compile_definitions(neutral_cpu_oracle_fixture
                             PRIVATE VIENNAPS_NEUTRAL_ORACLE_MOD=1)
endif()

add_executable(neutral_cpu_oracle_checker neutral_cpu_oracle_checker.cpp)
target_compile_features(neutral_cpu_oracle_checker PRIVATE cxx_std_20)

if(TARGET ViennaPS_Tests)
  add_dependencies(ViennaPS_Tests neutral_cpu_oracle_fixture
                   neutral_cpu_oracle_checker)
endif()
