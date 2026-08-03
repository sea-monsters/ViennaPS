set(VIENNAPS_LEVELSET_UPDATE_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/levelset_update.comp")
set(VIENNAPS_LEVELSET_UPDATE_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/levelset_update.comp.spv")
set(VIENNAPS_HRLE_CLASSIFICATION_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hrle_rebuild_classification.comp")
set(VIENNAPS_HRLE_CLASSIFICATION_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/hrle_rebuild_classification.comp.spv")
set(VIENNAPS_HRLE_ACTION_FLAGS_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hrle_rebuild_action_flags.comp")
set(VIENNAPS_HRLE_ACTION_FLAGS_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/hrle_rebuild_action_flags.comp.spv")
set(VIENNAPS_HRLE_COMPACT_SHADER
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hrle_rebuild_compact.comp")
set(VIENNAPS_HRLE_COMPACT_SPV
    "${CMAKE_CURRENT_BINARY_DIR}/hrle_rebuild_compact.comp.spv")

find_program(VIENNAPS_LEVELSET_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for the Vulkan Level Set update kernel")
if(NOT VIENNAPS_LEVELSET_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  if(WIN32)
    set(_VIENNAPS_LEVELSET_SDK_BIN "$ENV{VULKAN_SDK}/Bin")
  else()
    set(_VIENNAPS_LEVELSET_SDK_BIN "$ENV{VULKAN_SDK}/bin")
  endif()
  find_program(VIENNAPS_LEVELSET_SHADER_COMPILER
               NAMES glslc glslangValidator
               PATHS "${_VIENNAPS_LEVELSET_SDK_BIN}"
               NO_DEFAULT_PATH)
endif()

if(NOT VIENNAPS_LEVELSET_SHADER_COMPILER)
  message(STATUS
          "[ViennaPS Vulkan Level Set] no shader compiler found; skipping smoke.")
  return()
endif()

if("${VIENNAPS_LEVELSET_SHADER_COMPILER}" MATCHES "glslangValidator")
  add_custom_command(
    OUTPUT "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}" -V
            "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
            -o "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    COMMENT "Generating Vulkan Level Set update SPIR-V")
else()
  add_custom_command(
    OUTPUT "${VIENNAPS_LEVELSET_UPDATE_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}"
            -o "${VIENNAPS_LEVELSET_UPDATE_SPV}"
            "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SHADER}"
    COMMENT "Generating Vulkan Level Set update SPIR-V")
endif()

add_custom_target(viennaps-vulkan-levelset-update-spv
                  DEPENDS "${VIENNAPS_LEVELSET_UPDATE_SPV}")

if("${VIENNAPS_LEVELSET_SHADER_COMPILER}" MATCHES "glslangValidator")
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_CLASSIFICATION_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}" -V
            "${VIENNAPS_HRLE_CLASSIFICATION_SHADER}"
            -o "${VIENNAPS_HRLE_CLASSIFICATION_SPV}"
    DEPENDS "${VIENNAPS_HRLE_CLASSIFICATION_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild classification SPIR-V")
else()
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_CLASSIFICATION_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}"
            -o "${VIENNAPS_HRLE_CLASSIFICATION_SPV}"
            "${VIENNAPS_HRLE_CLASSIFICATION_SHADER}"
    DEPENDS "${VIENNAPS_HRLE_CLASSIFICATION_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild classification SPIR-V")
endif()

add_custom_target(viennaps-vulkan-hrle-classification-spv
                  DEPENDS "${VIENNAPS_HRLE_CLASSIFICATION_SPV}")

if("${VIENNAPS_LEVELSET_SHADER_COMPILER}" MATCHES "glslangValidator")
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}" -V
            "${VIENNAPS_HRLE_ACTION_FLAGS_SHADER}"
            -o "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}"
    DEPENDS "${VIENNAPS_HRLE_ACTION_FLAGS_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild action-flags SPIR-V")
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_COMPACT_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}" -V
            "${VIENNAPS_HRLE_COMPACT_SHADER}"
            -o "${VIENNAPS_HRLE_COMPACT_SPV}"
    DEPENDS "${VIENNAPS_HRLE_COMPACT_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild compaction SPIR-V")
else()
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}"
            -o "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}"
            "${VIENNAPS_HRLE_ACTION_FLAGS_SHADER}"
    DEPENDS "${VIENNAPS_HRLE_ACTION_FLAGS_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild action-flags SPIR-V")
  add_custom_command(
    OUTPUT "${VIENNAPS_HRLE_COMPACT_SPV}"
    COMMAND "${VIENNAPS_LEVELSET_SHADER_COMPILER}"
            -o "${VIENNAPS_HRLE_COMPACT_SPV}"
            "${VIENNAPS_HRLE_COMPACT_SHADER}"
    DEPENDS "${VIENNAPS_HRLE_COMPACT_SHADER}"
    COMMENT "Generating Vulkan HRLE rebuild compaction SPIR-V")
endif()

add_custom_target(viennaps-vulkan-hrle-compaction-spv
                  DEPENDS "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}"
                          "${VIENNAPS_HRLE_COMPACT_SPV}")

add_library(viennaps_vulkan_levelset_update
            levelset_update.cpp
            hrle_rebuild_classification.cpp)
set_property(TARGET viennaps_vulkan_levelset_update
             PROPERTY VIENNAPS_LEVELSET_UPDATE_SPV
                      "${VIENNAPS_LEVELSET_UPDATE_SPV}")
set_property(TARGET viennaps_vulkan_levelset_update
             PROPERTY VIENNAPS_HRLE_CLASSIFICATION_SPV
                      "${VIENNAPS_HRLE_CLASSIFICATION_SPV}")
target_include_directories(viennaps_vulkan_levelset_update
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_LIST_DIR}/../../../include/viennaps")
target_link_libraries(viennaps_vulkan_levelset_update
  PUBLIC viennaps_vulkan_compute_runtime Vulkan::Vulkan)
target_compile_features(viennaps_vulkan_levelset_update PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(viennaps_vulkan_levelset_update PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps_vulkan_levelset_update
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" _VIENNAPS_LEVELSET_UPDATE_SPV_PATH
               "${VIENNAPS_LEVELSET_UPDATE_SPV}")
add_executable(viennaps-vulkan-levelset-update-smoke
               levelset_update_smoke.cpp)
add_dependencies(viennaps-vulkan-levelset-update-smoke
                 viennaps-vulkan-levelset-update-spv)
target_compile_definitions(viennaps-vulkan-levelset-update-smoke PRIVATE
  "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_LEVELSET_UPDATE_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-levelset-update-smoke
  PRIVATE viennaps_vulkan_levelset_update)
target_compile_features(viennaps-vulkan-levelset-update-smoke PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-levelset-update-smoke PRIVATE /W4 /WX)
else()
  target_compile_options(viennaps-vulkan-levelset-update-smoke
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

string(REPLACE "\\" "/" _VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH
               "${VIENNAPS_HRLE_CLASSIFICATION_SPV}")
add_executable(viennaps-vulkan-hrle-classification-smoke
               hrle_rebuild_classification_smoke.cpp)
add_dependencies(viennaps-vulkan-hrle-classification-smoke
                 viennaps-vulkan-hrle-classification-spv)
target_compile_definitions(viennaps-vulkan-hrle-classification-smoke PRIVATE
  "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-hrle-classification-smoke
  PRIVATE ViennaPS viennaps_vulkan_levelset_update)
target_compile_features(viennaps-vulkan-hrle-classification-smoke
                        PRIVATE cxx_std_20)
if(WIN32)
  find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
  add_custom_command(
    TARGET viennaps-vulkan-hrle-classification-smoke
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:viennaps-vulkan-hrle-classification-smoke>
            $<TARGET_FILE:TBB::tbb>
            $<TARGET_FILE_DIR:viennaps-vulkan-hrle-classification-smoke>
    COMMAND_EXPAND_LISTS)
endif()
if(MSVC)
  target_compile_options(viennaps-vulkan-hrle-classification-smoke
                         PRIVATE /W4)
else()
  target_compile_options(viennaps-vulkan-hrle-classification-smoke
                         PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TARGET viennaps_vulkan_reduction_scan_primitives)
  get_property(_VIENNAPS_REDUCTION_SCAN_SPV
    TARGET viennaps_vulkan_reduction_scan_primitives
    PROPERTY VIENNAPS_REDUCTION_SCAN_SPV)
  if(NOT _VIENNAPS_REDUCTION_SCAN_SPV)
    message(FATAL_ERROR
      "Vulkan reduction/scan target does not publish its SPIR-V artifact")
  endif()

  add_library(viennaps_vulkan_hrle_compaction
              hrle_rebuild_compaction.cpp
              hrle_rebuild_pipeline.hpp)
  set_property(TARGET viennaps_vulkan_hrle_compaction
               PROPERTY VIENNAPS_HRLE_ACTION_FLAGS_SPV
                        "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}")
  set_property(TARGET viennaps_vulkan_hrle_compaction
               PROPERTY VIENNAPS_HRLE_COMPACT_SPV
                        "${VIENNAPS_HRLE_COMPACT_SPV}")
  target_include_directories(viennaps_vulkan_hrle_compaction
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
           "${CMAKE_CURRENT_LIST_DIR}/../../../include/viennaps")
  target_link_libraries(viennaps_vulkan_hrle_compaction
    PUBLIC viennaps_vulkan_reduction_scan_primitives)
  target_compile_features(viennaps_vulkan_hrle_compaction PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(viennaps_vulkan_hrle_compaction PRIVATE /W4 /WX)
  else()
    target_compile_options(viennaps_vulkan_hrle_compaction
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  string(REPLACE "\\" "/" _VIENNAPS_REDUCTION_SCAN_SPV_PATH
                 "${_VIENNAPS_REDUCTION_SCAN_SPV}")
  string(REPLACE "\\" "/" _VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH
                 "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}")
  string(REPLACE "\\" "/" _VIENNAPS_HRLE_COMPACT_SPV_PATH
                 "${VIENNAPS_HRLE_COMPACT_SPV}")
  add_executable(viennaps-vulkan-hrle-compaction-smoke
                 hrle_rebuild_compaction_smoke.cpp)
  add_dependencies(viennaps-vulkan-hrle-compaction-smoke
                   viennaps-vulkan-hrle-classification-spv
                   viennaps-vulkan-reduction-scan-smoke-spv
                   viennaps-vulkan-hrle-compaction-spv)
  target_compile_definitions(viennaps-vulkan-hrle-compaction-smoke PRIVATE
    "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH}\""
    "VIENNAPS_REDUCTION_SCAN_SPV_PATH=\"${_VIENNAPS_REDUCTION_SCAN_SPV_PATH}\""
    "VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH=\"${_VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH}\""
    "VIENNAPS_HRLE_COMPACT_SPV_PATH=\"${_VIENNAPS_HRLE_COMPACT_SPV_PATH}\"")
  target_link_libraries(viennaps-vulkan-hrle-compaction-smoke
    PRIVATE ViennaPS viennaps_vulkan_levelset_update
            viennaps_vulkan_hrle_compaction)
  target_compile_features(viennaps-vulkan-hrle-compaction-smoke
                          PRIVATE cxx_std_20)
  if(WIN32)
    find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
    add_custom_command(
      TARGET viennaps-vulkan-hrle-compaction-smoke
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:viennaps-vulkan-hrle-compaction-smoke>
              $<TARGET_FILE:TBB::tbb>
              $<TARGET_FILE_DIR:viennaps-vulkan-hrle-compaction-smoke>
      COMMAND_EXPAND_LISTS)
  endif()
  if(MSVC)
    target_compile_options(viennaps-vulkan-hrle-compaction-smoke PRIVATE /W4)
  else()
    target_compile_options(viennaps-vulkan-hrle-compaction-smoke
                           PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  add_executable(viennaps-vulkan-hrle-rebuild-executor-smoke
                 viennals_rebuild_executor_smoke.cpp)
  add_dependencies(viennaps-vulkan-hrle-rebuild-executor-smoke
                   viennaps-vulkan-hrle-classification-spv
                   viennaps-vulkan-reduction-scan-smoke-spv
                   viennaps-vulkan-hrle-compaction-spv)
  target_compile_definitions(viennaps-vulkan-hrle-rebuild-executor-smoke PRIVATE
    "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH}\""
    "VIENNAPS_REDUCTION_SCAN_SPV_PATH=\"${_VIENNAPS_REDUCTION_SCAN_SPV_PATH}\""
    "VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH=\"${_VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH}\""
    "VIENNAPS_HRLE_COMPACT_SPV_PATH=\"${_VIENNAPS_HRLE_COMPACT_SPV_PATH}\"")
  target_link_libraries(viennaps-vulkan-hrle-rebuild-executor-smoke
    PRIVATE ViennaPS viennaps_vulkan_levelset_update
            viennaps_vulkan_hrle_compaction)
  target_compile_features(viennaps-vulkan-hrle-rebuild-executor-smoke
                          PRIVATE cxx_std_20)
  if(WIN32)
    find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
    add_custom_command(
      TARGET viennaps-vulkan-hrle-rebuild-executor-smoke
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:viennaps-vulkan-hrle-rebuild-executor-smoke>
              $<TARGET_FILE:TBB::tbb>
              $<TARGET_FILE_DIR:viennaps-vulkan-hrle-rebuild-executor-smoke>
      COMMAND_EXPAND_LISTS)
  endif()
  if(MSVC)
    target_compile_options(viennaps-vulkan-hrle-rebuild-executor-smoke
                           PRIVATE /W4)
  else()
    target_compile_options(viennaps-vulkan-hrle-rebuild-executor-smoke
                           PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

add_executable(viennaps-vulkan-levelset-process-controller-smoke
               levelset_process_controller_smoke.cpp)
add_dependencies(viennaps-vulkan-levelset-process-controller-smoke
                 viennaps-vulkan-levelset-update-spv
                 viennaps-vulkan-hrle-classification-spv
                 viennaps-vulkan-reduction-scan-smoke-spv
                 viennaps-vulkan-hrle-compaction-spv)
string(REPLACE "\\" "/" _VIENNAPS_REDUCTION_SCAN_SPV_PATH
               "${_VIENNAPS_REDUCTION_SCAN_SPV}")
string(REPLACE "\\" "/" _VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH
               "${VIENNAPS_HRLE_CLASSIFICATION_SPV}")
string(REPLACE "\\" "/" _VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH
               "${VIENNAPS_HRLE_ACTION_FLAGS_SPV}")
string(REPLACE "\\" "/" _VIENNAPS_HRLE_COMPACT_SPV_PATH
               "${VIENNAPS_HRLE_COMPACT_SPV}")
target_compile_definitions(
  viennaps-vulkan-levelset-process-controller-smoke PRIVATE
  "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_LEVELSET_UPDATE_SPV_PATH}\""
  "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH}\""
  "VIENNAPS_REDUCTION_SCAN_SPV_PATH=\"${_VIENNAPS_REDUCTION_SCAN_SPV_PATH}\""
  "VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH=\"${_VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH}\""
  "VIENNAPS_HRLE_COMPACT_SPV_PATH=\"${_VIENNAPS_HRLE_COMPACT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-levelset-process-controller-smoke
  PRIVATE ViennaPS viennaps_vulkan_levelset_update
          viennaps_vulkan_hrle_compaction)
target_compile_features(viennaps-vulkan-levelset-process-controller-smoke
  PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(viennaps-vulkan-levelset-process-controller-smoke
                         PRIVATE /W4)
else()
  target_compile_options(viennaps-vulkan-levelset-process-controller-smoke
                         PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(viennaps-vulkan-levelset-process-controller-execution-smoke
               levelset_process_controller_execution_smoke.cpp)
add_dependencies(viennaps-vulkan-levelset-process-controller-execution-smoke
                 viennaps-vulkan-levelset-update-spv
                 viennaps-vulkan-hrle-classification-spv
                 viennaps-vulkan-reduction-scan-smoke-spv
                 viennaps-vulkan-hrle-compaction-spv)
target_compile_definitions(
  viennaps-vulkan-levelset-process-controller-execution-smoke PRIVATE
  "VIENNAPS_LEVELSET_UPDATE_SPV_PATH=\"${_VIENNAPS_LEVELSET_UPDATE_SPV_PATH}\""
  "VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH=\"${_VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH}\""
  "VIENNAPS_REDUCTION_SCAN_SPV_PATH=\"${_VIENNAPS_REDUCTION_SCAN_SPV_PATH}\""
  "VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH=\"${_VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH}\""
  "VIENNAPS_HRLE_COMPACT_SPV_PATH=\"${_VIENNAPS_HRLE_COMPACT_SPV_PATH}\"")
target_link_libraries(viennaps-vulkan-levelset-process-controller-execution-smoke
  PRIVATE ViennaPS viennaps_vulkan_levelset_update
          viennaps_vulkan_hrle_compaction)
target_compile_features(viennaps-vulkan-levelset-process-controller-execution-smoke
                         PRIVATE cxx_std_20)
if(WIN32)
  find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
  add_custom_command(
    TARGET viennaps-vulkan-levelset-process-controller-execution-smoke
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:viennaps-vulkan-levelset-process-controller-execution-smoke>
            $<TARGET_FILE:TBB::tbb>
            $<TARGET_FILE_DIR:viennaps-vulkan-levelset-process-controller-execution-smoke>
    COMMAND_EXPAND_LISTS)
endif()
if(MSVC)
  target_compile_options(viennaps-vulkan-levelset-process-controller-execution-smoke
                          PRIVATE /W4)
else()
  target_compile_options(viennaps-vulkan-levelset-process-controller-execution-smoke
                         PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE AND TARGET ViennaPS)
  add_executable(viennaps-vulkan-levelset-surface-composition-smoke
                 levelset_surface_composition_smoke.cpp)
  target_link_libraries(viennaps-vulkan-levelset-surface-composition-smoke
                        PRIVATE ViennaPS viennaps_vulkan_compute_runtime)
  target_compile_features(viennaps-vulkan-levelset-surface-composition-smoke
                          PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(viennaps-vulkan-levelset-surface-composition-smoke
                            PRIVATE /W4)
  else()
    target_compile_options(viennaps-vulkan-levelset-surface-composition-smoke
                           PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  add_executable(viennaps-vulkan-levelset-deployment-session-smoke
                 levelset_deployment_session_smoke.cpp)
  target_link_libraries(viennaps-vulkan-levelset-deployment-session-smoke
                        PRIVATE ViennaPS viennaps_vulkan_compute_runtime)
  target_compile_features(viennaps-vulkan-levelset-deployment-session-smoke
                           PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(viennaps-vulkan-levelset-deployment-session-smoke
                           PRIVATE /W4)
  else()
    target_compile_options(viennaps-vulkan-levelset-deployment-session-smoke
                           PRIVATE -Wall -Wextra -Wpedantic)
  endif()
elseif(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
  message(STATUS
          "[ViennaPS Vulkan Level Set] deployment-session smoke requires the "
          "top-level ViennaPS target; skipping standalone GPU build.")
endif()

if((BUILD_TESTING OR VIENNAPS_BUILD_TESTS) AND
   VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
  add_test(NAME viennaps-vulkan-levelset-update-smoke
           COMMAND viennaps-vulkan-levelset-update-smoke)
  add_test(NAME viennaps-vulkan-hrle-classification-smoke
           COMMAND viennaps-vulkan-hrle-classification-smoke)
  if(TARGET viennaps-vulkan-hrle-compaction-smoke)
    add_test(NAME viennaps-vulkan-hrle-compaction-smoke
             COMMAND viennaps-vulkan-hrle-compaction-smoke)
  endif()
  if(TARGET viennaps-vulkan-hrle-rebuild-executor-smoke)
    add_test(NAME viennaps-vulkan-hrle-rebuild-executor-smoke
             COMMAND viennaps-vulkan-hrle-rebuild-executor-smoke)
    set_tests_properties(viennaps-vulkan-hrle-rebuild-executor-smoke
                         PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=2" TIMEOUT 30)
  endif()
  if(TARGET viennaps-vulkan-levelset-deployment-session-smoke)
    add_test(NAME viennaps-vulkan-levelset-deployment-session-smoke
             COMMAND viennaps-vulkan-levelset-deployment-session-smoke)
  endif()
  if(TARGET viennaps-vulkan-levelset-surface-composition-smoke)
    add_test(NAME viennaps-vulkan-levelset-surface-composition-smoke
             COMMAND viennaps-vulkan-levelset-surface-composition-smoke)
  endif()
  if(TARGET viennaps-vulkan-levelset-process-controller-smoke)
    add_test(NAME viennaps-vulkan-levelset-process-controller-smoke
             COMMAND viennaps-vulkan-levelset-process-controller-smoke)
  endif()
  if(TARGET viennaps-vulkan-levelset-process-controller-execution-smoke)
    add_test(NAME viennaps-vulkan-levelset-process-controller-execution-smoke
             COMMAND viennaps-vulkan-levelset-process-controller-execution-smoke)
  endif()
endif()
