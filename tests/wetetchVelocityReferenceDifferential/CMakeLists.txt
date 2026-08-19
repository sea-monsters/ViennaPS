cmake_minimum_required(VERSION 3.20)
project(wetetchVelocityReferenceDifferential LANGUAGES CXX)

if(VIENNAPS_BUILD_TESTS OR BUILD_TESTING)
  include(CTest)
  find_program(_p5_powershell NAMES powershell pwsh REQUIRED)
  add_test(
    NAME wetetch-velocity-cpu-reference-differential
    COMMAND ${_p5_powershell} -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/run_paired_oracle.ps1
            -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR}/wetetch-velocity-cpu)
endif()
