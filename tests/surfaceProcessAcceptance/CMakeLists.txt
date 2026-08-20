project(surfaceProcessAcceptance LANGUAGES CXX)

add_executable(surface_process_acceptance_cpu
               surface_process_acceptance_cpu.cpp)
target_link_libraries(surface_process_acceptance_cpu PRIVATE ViennaPS)
target_compile_features(surface_process_acceptance_cpu PRIVATE cxx_std_20)

if(WIN32)
  viennacore_setup_embree_env(surface_process_acceptance_cpu
                              $<TARGET_FILE_DIR:surface_process_acceptance_cpu>)
  find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
  add_custom_command(
    TARGET surface_process_acceptance_cpu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb>
            $<TARGET_FILE_DIR:surface_process_acceptance_cpu>)
endif()

add_dependencies(ViennaPS_Tests surface_process_acceptance_cpu)
add_test(NAME surface_process_acceptance_cpu
         COMMAND $<TARGET_FILE:surface_process_acceptance_cpu>)
