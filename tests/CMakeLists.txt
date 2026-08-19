add_custom_target(ViennaPS_Tests ALL)

if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${PROJECT_BINARY_DIR}/tests>)
  viennacore_setup_embree_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  viennacore_setup_vtk_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  viennacore_setup_tbb_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

# Both install/export consumer fixtures are standalone downstream projects.
# Adding either to
# the in-tree test graph makes its `find_package(ViennaPS)` resolve the
# incomplete build-tree config before `ViennaPSTargets.cmake` exists.  The
# PD5 script configures it only after `cmake --install`, which is the contract
# this fixture is intended to verify.
file(GLOB _viennaps_test_entries CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/*")
foreach(_viennaps_test_entry IN LISTS _viennaps_test_entries)
  if(NOT IS_DIRECTORY "${_viennaps_test_entry}")
    message(STATUS "Skipping non directory '${_viennaps_test_entry}'")
    continue()
  endif()

  get_filename_component(_viennaps_test_name "${_viennaps_test_entry}" NAME)
  if(_viennaps_test_name STREQUAL "installExportConsumer" OR
     _viennaps_test_name STREQUAL "vulkanInstallExportConsumer")
    message(STATUS "Skipping standalone install/export consumer fixture")
    continue()
  endif()

  message(STATUS "Adding ${_viennaps_test_entry}")
  add_subdirectory("${_viennaps_test_entry}")
endforeach()
