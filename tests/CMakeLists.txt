add_custom_target(ViennaPS_Tests ALL)

if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${PROJECT_BINARY_DIR}/tests>)
  viennacore_setup_embree_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  viennacore_setup_vtk_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  viennacore_setup_tbb_env(ViennaPS_Tests ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

  # ---- P6 structural fix: self-provisioning per-executable runtime DLLs ---
  # The aggregate-only setup calls above fire solely when ViennaPS_Tests
  # builds, so focused `--target <one-test>` builds produced executables
  # that failed to launch (0xC0000135) until the aggregate happened to be
  # built first. Override add_executable for every test subdirectory so each
  # executable carries its own runtime-DLL copies. Functions defined here
  # remain visible inside the add_subdirectory scopes below, which keeps
  # TARGET-scoped custom commands legal (same-directory rule).
  function(add_executable _viennaps_name)
    _add_executable(${ARGV})
    viennacore_setup_embree_env(
      ${_viennaps_name} $<TARGET_FILE_DIR:${_viennaps_name}>)
    viennacore_setup_tbb_env(${_viennaps_name}
                             $<TARGET_FILE_DIR:${_viennaps_name}>)
    viennacore_setup_vtk_env(${_viennaps_name}
                             $<TARGET_FILE_DIR:${_viennaps_name}>)
    # embree4.dll hard-depends on tbb12.dll even in trees where no TBB CMake
    # target exists; copy from TBB::tbb when present, else locate a
    # redistributable copy best-effort.
    if(TARGET TBB::tbb)
      add_custom_command(
        TARGET ${_viennaps_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:${_viennaps_name}>)
    else()
        find_file(
          _VIENNAPS_TBB_DLL tbb12.dll
          HINTS "$ENV{CONDA_PREFIX}/Library/bin"
          PATH_SUFFIXES Library/bin)
      if(_VIENNAPS_TBB_DLL)
        add_custom_command(
          TARGET ${_viennaps_name} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${_VIENNAPS_TBB_DLL}"
                  $<TARGET_FILE_DIR:${_viennaps_name}>)
      endif()
      unset(_VIENNAPS_TBB_DLL CACHE)
    endif()
    # LLVM OpenMP runtime is not modelled as a CMake target; best-effort copy
    # from the installed VC redist so /openmp:llvm builds are launch-ready.
    find_file(
      _VIENNAPS_LIBOMP_DLL libomp140.x86_64.dll
      HINTS
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/14.44.35112/debug_nonredist/x64/Microsoft.VC143.OpenMP.LLVM"
      NO_DEFAULT_PATH)
    if(_VIENNAPS_LIBOMP_DLL)
      add_custom_command(
        TARGET ${_viennaps_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_VIENNAPS_LIBOMP_DLL}"
                $<TARGET_FILE_DIR:${_viennaps_name}>)
    endif()
    unset(_VIENNAPS_LIBOMP_DLL CACHE)
  endfunction()
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
