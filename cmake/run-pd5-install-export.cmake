# Runs the PD5 install/export acceptance scenario in a caller-owned work tree.
# The consumer is configured only through the installed prefix, not the source
# tree, so dependency export errors cannot be masked by build-tree targets.

if(NOT DEFINED VIENNAPS_INSTALL_EXPORT_WORK_DIR)
  message(FATAL_ERROR
          "Set VIENNAPS_INSTALL_EXPORT_WORK_DIR to a caller-owned directory.")
endif()

if(NOT DEFINED VIENNAPS_SOURCE_DIR)
  cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH VIENNAPS_SOURCE_DIR)
endif()

if(NOT DEFINED VIENNAPS_INSTALL_EXPORT_GENERATOR)
  set(VIENNAPS_INSTALL_EXPORT_GENERATOR Ninja)
endif()

set(source_build_dir "${VIENNAPS_INSTALL_EXPORT_WORK_DIR}/source-build")
set(install_prefix "${VIENNAPS_INSTALL_EXPORT_WORK_DIR}/prefix")
set(consumer_build_dir "${VIENNAPS_INSTALL_EXPORT_WORK_DIR}/consumer-build")
set(consumer_source_dir "${VIENNAPS_SOURCE_DIR}/tests/installExportConsumer")
set(source_config_args)
if(DEFINED CPM_SOURCE_CACHE)
  # This acceptance scenario starts a fresh source build.  Preserve a
  # caller-provided, content-addressed CPM cache so offline/restricted
  # environments exercise the same dependency bootstrap contract as the
  # primary build, without recording a machine path in the project.
  list(APPEND source_config_args "-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}")
endif()
if(DEFINED CPM_ViennaLS_SOURCE)
  list(APPEND source_config_args "-DCPM_ViennaLS_SOURCE=${CPM_ViennaLS_SOURCE}")
endif()

file(MAKE_DIRECTORY "${VIENNAPS_INSTALL_EXPORT_WORK_DIR}")

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    COMMAND_ECHO STDOUT
    RESULT_VARIABLE result
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "PD5 install/export command failed with exit ${result}.")
  endif()
endfunction()

run_checked(
  "${CMAKE_COMMAND}" -S "${VIENNAPS_SOURCE_DIR}" -B "${source_build_dir}"
  -G "${VIENNAPS_INSTALL_EXPORT_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${install_prefix} -DCMAKE_FIND_USE_INSTALL_PREFIX=FALSE
  -DVIENNAPS_BUILD_TESTS=OFF
  -DVIENNAPS_USE_VTK=OFF -DVIENNAPS_VTK_RENDERING=OFF
  -DVIENNAPS_ENABLE_VULKAN=OFF -DVIENNAPS_BUILD_VULKAN_PROBE=OFF
  -DVIENNAPS_BUILD_VULKAN_SMOKE=OFF
  ${source_config_args}
)
set(source_build_command "${CMAKE_COMMAND}" --build "${source_build_dir}"
                         --config Release)
if(DEFINED VIENNAPS_INSTALL_EXPORT_PARALLEL)
  if(NOT VIENNAPS_INSTALL_EXPORT_PARALLEL MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
            "VIENNAPS_INSTALL_EXPORT_PARALLEL must be a positive integer.")
  endif()
  list(APPEND source_build_command --parallel
       "${VIENNAPS_INSTALL_EXPORT_PARALLEL}")
endif()
run_checked(${source_build_command})
run_checked("${CMAKE_COMMAND}" --install "${source_build_dir}" --config Release)
run_checked(
  "${CMAKE_COMMAND}" -S "${consumer_source_dir}" -B "${consumer_build_dir}"
  -G "${VIENNAPS_INSTALL_EXPORT_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_PREFIX_PATH=${install_prefix}
)
run_checked("${CMAKE_COMMAND}" --build "${consumer_build_dir}" --config Release)
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build_dir}"
            --output-on-failure -C Release)

message(STATUS "PD5 install/export consumer: PASS")
