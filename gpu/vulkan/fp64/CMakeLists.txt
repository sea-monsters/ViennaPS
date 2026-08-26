# P6-A4 FP64 kernels: compiled ONLY behind VIENNAPS_ENABLE_VULKAN_FP64.
# The local development device reports shaderFloat64 = false, so every
# artifact here is classified DEVICE-PENDING-HARDWARE; the guard smoke is
# the only always-runnable piece and it exercises pure policy logic.

set(VIENNAPS_FP64_SHADERS
    shaders/la_spmv_f64.comp
    shaders/la_axpy_f64.comp
    shaders/la_dot_f64.comp)

find_program(VIENNAPS_FP64_SHADER_COMPILER
             NAMES glslc glslangValidator
             DOC "Shader compiler used for FP64 kernel validation")

if(NOT VIENNAPS_FP64_SHADER_COMPILER AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}")
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _VIENNAPS_FP64_SDK)
  find_program(VIENNAPS_FP64_SHADER_COMPILER
               NAMES glslc glslangValidator
               PATHS "${_VIENNAPS_FP64_SDK}/Bin" "${_VIENNAPS_FP64_SDK}/bin")
endif()

# Static SPIR-V validation receipts: each shader must compile clean. This is
# the only verification possible without an FP64-capable device.
foreach(_viennaps_fp64_shader IN LISTS VIENNAPS_FP64_SHADERS)
  if(NOT VIENNAPS_FP64_SHADER_COMPILER)
    break()
  endif()
  get_filename_component(_viennaps_fp64_name "${_viennaps_fp64_shader}" NAME)
  set(_viennaps_fp64_spv
      "${CMAKE_CURRENT_BINARY_DIR}/${_viennaps_fp64_name}.spv")
  string(REPLACE ".comp" "" _viennaps_fp64_target "viennaps-fp64-${_viennaps_fp64_name}")
  if("${VIENNAPS_FP64_SHADER_COMPILER}" MATCHES ".*glslangValidator.*")
    add_custom_command(
      OUTPUT ${_viennaps_fp64_spv}
      COMMAND ${VIENNAPS_FP64_SHADER_COMPILER} -V
              ${CMAKE_CURRENT_SOURCE_DIR}/${_viennaps_fp64_shader}
              -o ${_viennaps_fp64_spv}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_viennaps_fp64_shader}
      COMMENT "Validating FP64 kernel ${_viennaps_fp64_name} (SPIR-V).")
  else()
    add_custom_command(
      OUTPUT ${_viennaps_fp64_spv}
      COMMAND ${VIENNAPS_FP64_SHADER_COMPILER}
              -o ${_viennaps_fp64_spv}
              ${CMAKE_CURRENT_SOURCE_DIR}/${_viennaps_fp64_shader}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_viennaps_fp64_shader}
      COMMENT "Validating FP64 kernel ${_viennaps_fp64_name} (SPIR-V).")
  endif()
  add_custom_target(${_viennaps_fp64_target}-spv DEPENDS ${_viennaps_fp64_spv})
  list(APPEND VIENNAPS_FP64_SPV_TARGETS ${_viennaps_fp64_target}-spv)
endforeach()

add_executable(viennaps-vulkan-fp64-smoke fp64_smoke.cpp)
target_compile_features(viennaps-vulkan-fp64-smoke PRIVATE cxx_std_20)
target_include_directories(viennaps-vulkan-fp64-smoke PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../../include/viennaps")
target_compile_definitions(viennaps-vulkan-fp64-smoke PRIVATE
                           VIENNAPS_ENABLE_VULKAN_FP64=1)
target_link_libraries(viennaps-vulkan-fp64-smoke PRIVATE Vulkan::Vulkan)
if(VIENNAPS_FP64_SPV_TARGETS)
  add_dependencies(viennaps-vulkan-fp64-smoke ${VIENNAPS_FP64_SPV_TARGETS})
endif()

if(BUILD_TESTING OR VIENNAPS_BUILD_TESTS)
  add_test(NAME viennaps-vulkan-fp64-smoke
           COMMAND viennaps-vulkan-fp64-smoke)
  set_tests_properties(viennaps-vulkan-fp64-smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "fp64 smoke PASS"
    FAIL_REGULAR_EXPRESSION "FAIL")
endif()
