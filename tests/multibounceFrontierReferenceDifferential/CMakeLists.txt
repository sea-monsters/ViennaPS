project(multibounceFrontierReferenceDifferential LANGUAGES CXX)

set(P5_REFERENCE_VIENNAPS_SOURCE_DIR "" CACHE PATH
    "Reference ViennaPS checkout for the independent emitter")
set(P5_REFERENCE_VIENNARAY_SOURCE_DIR "" CACHE PATH
    "Instrumented ViennaRay checkout for the reference emitter")

add_executable(multibounce_frontier_trace_checker trace_checker.cpp)
target_compile_features(multibounce_frontier_trace_checker PRIVATE cxx_std_20)

if(TARGET ViennaPS)
  set(_p5_emitter_target multibounce_frontier_mod_emitter)
  set(_p5_emitter_definition "")
  if(VIENNAPS_P5_REFERENCE_EMITTER)
    set(_p5_emitter_target multibounce_frontier_reference_emitter)
    set(_p5_emitter_definition VIENNAPS_P5_REFERENCE_EMITTER=1)
  endif()

  add_executable(${_p5_emitter_target} trace_emitter.cpp)
  target_link_libraries(${_p5_emitter_target} PRIVATE ViennaPS)
  target_compile_features(${_p5_emitter_target} PRIVATE cxx_std_20)
  target_compile_definitions(${_p5_emitter_target} PRIVATE
    VIENNAPS_P5_TRACE_OBSERVER=1 ${_p5_emitter_definition})
  target_include_directories(${_p5_emitter_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(VIENNAPS_BUILD_TESTS OR BUILD_TESTING)
  include(CTest)
  find_program(_p5_powershell NAMES powershell pwsh REQUIRED)
  add_test(
    NAME multibounce-frontier-reference-differential
    COMMAND ${_p5_powershell} -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/run_paired_oracle.ps1
            -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR}/p5-trace)
endif()
