# Runs the opt-in PD5 Vulkan install/export acceptance in a caller-owned tree.
# Unlike the CPU/no-SDK gate, this scenario requires a Vulkan SDK and installs
# the reusable runtime plus the source/generated shader payload.

if(NOT DEFINED VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR)
  message(FATAL_ERROR
          "Set VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR to a caller-owned directory.")
endif()
if(NOT DEFINED VIENNAPS_SOURCE_DIR)
  cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH VIENNAPS_SOURCE_DIR)
endif()
if(NOT DEFINED VIENNAPS_VULKAN_INSTALL_EXPORT_GENERATOR)
  set(VIENNAPS_VULKAN_INSTALL_EXPORT_GENERATOR Ninja)
endif()

set(source_build_dir "${VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR}/source-build")
set(install_prefix "${VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR}/prefix")
set(consumer_build_dir "${VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR}/consumer-build")
set(consumer_source_dir "${VIENNAPS_SOURCE_DIR}/tests/vulkanInstallExportConsumer")
set(source_config_args)
if(DEFINED CPM_SOURCE_CACHE)
  list(APPEND source_config_args "-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}")
endif()
if(DEFINED CPM_ViennaLS_SOURCE)
  list(APPEND source_config_args "-DCPM_ViennaLS_SOURCE=${CPM_ViennaLS_SOURCE}")
endif()

file(MAKE_DIRECTORY "${VIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR}")

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    COMMAND_ECHO STDOUT
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
            "PD5 Vulkan install/export command failed with exit ${result}.")
  endif()
endfunction()

run_checked(
  "${CMAKE_COMMAND}" -S "${VIENNAPS_SOURCE_DIR}" -B "${source_build_dir}"
  -G "${VIENNAPS_VULKAN_INSTALL_EXPORT_GENERATOR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${install_prefix}
  -DCMAKE_FIND_USE_INSTALL_PREFIX=FALSE
  -DVIENNAPS_BUILD_TESTS=OFF
  -DVIENNAPS_USE_VTK=OFF
  -DVIENNAPS_VTK_RENDERING=OFF
  -DVIENNAPS_ENABLE_VULKAN=ON
  -DVIENNAPS_INSTALL_VULKAN_PAYLOAD=ON
  ${source_config_args})

run_checked("${CMAKE_COMMAND}" --build "${source_build_dir}"
            --target viennaps-vulkan-package-payload --parallel 4)
run_checked("${CMAKE_COMMAND}" --build "${source_build_dir}" --target embree
            --parallel 4)
run_checked("${CMAKE_COMMAND}" --install "${source_build_dir}"
            --config Release)
run_checked(
  "${CMAKE_COMMAND}" -DVIENNAPS_VULKAN_INSTALL_PREFIX=${install_prefix}
  -P "${VIENNAPS_SOURCE_DIR}/cmake/validate-pd5-vulkan-install-payload.cmake")
run_checked(
  "${CMAKE_COMMAND}" -S "${consumer_source_dir}" -B "${consumer_build_dir}"
  -G "${VIENNAPS_VULKAN_INSTALL_EXPORT_GENERATOR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_PREFIX_PATH=${install_prefix}
  -DBUILD_TESTING=ON)
run_checked("${CMAKE_COMMAND}" --build "${consumer_build_dir}" --parallel 2)
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build_dir}"
            --output-on-failure -C Release)

message(STATUS "PD5 Vulkan install/export consumer: PASS")
