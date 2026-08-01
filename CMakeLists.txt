cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
project(
  ViennaPS
  LANGUAGES CXX C
  VERSION 4.6.2)

# --------------------------------------------------------------------------------------------------------
# Include guards
# --------------------------------------------------------------------------------------------------------

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif()

# --------------------------------------------------------------------------------------------------------
# Library switches
# --------------------------------------------------------------------------------------------------------

option(VIENNAPS_PRECOMPILE_HEADERS "Enable precompiled headers" OFF)
option(VIENNAPS_STATIC_BUILD "Build ViennaPS as static library" OFF)
option(VIENNAPS_ENABLE_SANITIZER "Enable sanitizers" OFF)
option(VIENNAPS_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(VIENNAPS_USE_VTK "Enable VTK file support" ON)
option(VIENNAPS_VTK_RENDERING "Enable VTK rendering support" ON)

option(VIENNAPS_USE_GPU "Enable GPU support" OFF)
option(VIENNAPS_ENABLE_VULKAN "Enable the experimental Vulkan compute backend" OFF)
option(VIENNAPS_BUILD_VULKAN_PROBE "Build the Vulkan device capability probe"
       ${VIENNAPS_ENABLE_VULKAN})
option(VIENNAPS_BUILD_VULKAN_SMOKE "Build the Vulkan compute smoke test"
       ${VIENNAPS_ENABLE_VULKAN})
option(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE
       "Build the Vulkan primitive differential smoke test" OFF)
option(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE
       "Build the Vulkan Level Set update differential smoke test" OFF)

option(VIENNAPS_BUILD_EXAMPLES "Build examples" OFF)
option(VIENNAPS_BUILD_TESTS "Build tests" OFF)

option(VIENNAPS_BUILD_PYTHON "Build python bindings" OFF)
option(VIENNAPS_PACKAGE_PYTHON "Build python bindings with intent to publish wheel" OFF)

# --------------------------------------------------------------------------------------------------------
# Library options
# --------------------------------------------------------------------------------------------------------

set(VIENNAPS_LOOKUP_DIRS
    ""
    CACHE STRING "Directories to account for when searching installed dependencies (VTK or embree)")
set(VIENNAPS_VTK_SOURCE_DIR
    "$ENV{VIENNAPS_VTK_SOURCE_DIR}"
    CACHE PATH "Use an existing VTK source tree instead of finding or downloading VTK")
set(VIENNAPS_VIENNALS_SOURCE_DIR
    "$ENV{VIENNAPS_VIENNALS_SOURCE_DIR}"
    CACHE PATH
          "Use an existing ViennaLS source tree instead of downloading ViennaLS")

list(APPEND CMAKE_PREFIX_PATH ${VIENNAPS_LOOKUP_DIRS})

# --------------------------------------------------------------------------------------------------------
# Global CMake Configuration
# └ We depend on the vtk python package for our python build and re-use
#   their existing vtk libraries, which are not versioned. Because we (possibly) pull in VTK ourselves
#   during build time though, we disable the soname versioning so that we can easily set the rpath to
#   link against the vtk-python libs
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_PACKAGE_PYTHON)
  message(
    STATUS
      "[ViennaPS] Using remote packages due to 'VIENNAPS_PACKAGE_PYTHON', the build will take a long time!"
  )

  # CIBUILDWHEEL is absolutely cursed and won't work when this is turned on.
  # I don't know why...
  if(NOT VIENNAPS_IS_CI)
    set(CPM_DOWNLOAD_ALL ON)
  endif()

  set(VIENNAPS_BUILD_PYTHON ON)
endif()

if(VIENNAPS_BUILD_PYTHON)
  set(CMAKE_PLATFORM_NO_VERSIONED_SONAME ON)
endif()

if(VIENNAPS_STATIC_BUILD)
  message(STATUS "[ViennaPS] Enabling precompiled headers for static build")
  set(VIENNAPS_PRECOMPILE_HEADERS ON)
  set(BUILD_SHARED_LIBS OFF)
endif()

if(MSVC)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
    set(OpenMP_RUNTIME_MSVC
        "llvm"
        CACHE STRING "MSVC OpenMP runtime selected for FindOpenMP" FORCE)
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /openmp:llvm")
  endif()

  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/bigobj>")
endif()

# Reduce wheel size when packaging Python wheels.
# These flags are set before dependencies are added, so bundled dependencies use them too.
if(MSVC AND VIENNAPS_PACKAGE_PYTHON)
  string(APPEND CMAKE_C_FLAGS_RELEASE " /O2 /Gy /Gw")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2 /Gy /Gw")
  string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " /OPT:REF /OPT:ICF")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /OPT:REF /OPT:ICF")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /OPT:REF /OPT:ICF")
endif()

if((CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang") AND VIENNAPS_PACKAGE_PYTHON)
  string(APPEND CMAKE_C_FLAGS_RELEASE " -O3 -DNDEBUG -ffunction-sections -fdata-sections")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3 -DNDEBUG -ffunction-sections -fdata-sections")
  if(APPLE)
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " -Wl,-dead_strip")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " -Wl,-dead_strip")
  else()
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " -Wl,--gc-sections")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " -Wl,--gc-sections")
  endif()
endif()

# --------------------------------------------------------------------------------------------------------
# Setup Library
# --------------------------------------------------------------------------------------------------------

add_library(${PROJECT_NAME} INTERFACE)

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_20)
set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES CXX_STANDARD 20
             CXX_EXTENSIONS OFF
             CXX_STANDARD_REQUIRED ON
             WINDOWS_EXPORT_ALL_SYMBOLS ON)

# --------------------------------------------------------------------------------------------------------
# Include directories
# --------------------------------------------------------------------------------------------------------

# Generate version header
configure_file("${PROJECT_SOURCE_DIR}/cmake/psVersion.hpp.in"
               "${PROJECT_SOURCE_DIR}/include/viennaps/psVersion.hpp" @ONLY)

target_include_directories(
  ${PROJECT_NAME} INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/viennaps>
                            $<INSTALL_INTERFACE:include/viennaps-${PROJECT_VERSION}>)

# --------------------------------------------------------------------------------------------------------
# Setup Dependencies
# --------------------------------------------------------------------------------------------------------
include("cmake/cpm.cmake")
include("cmake/vtk.cmake")

CPMAddPackage(
  NAME ViennaCore
  VERSION 2.2.1
  GIT_REPOSITORY "https://github.com/ViennaTools/ViennaCore"
  EXCLUDE_FROM_ALL ${VIENNAPS_BUILD_PYTHON}
  OPTIONS "VIENNACORE_FORMAT_EXCLUDE build/" "VIENNACORE_USE_GPU ${VIENNAPS_USE_GPU}")

CPMAddPackage(
  NAME PackageProject
  VERSION 1.13.0
  GIT_REPOSITORY "https://github.com/TheLartians/PackageProject.cmake"
  EXCLUDE_FROM_ALL ${VIENNAPS_BUILD_PYTHON})

CPMAddPackage(
  NAME ViennaRay
  VERSION 4.3.1
  GIT_TAG benchmark
  GIT_REPOSITORY "https://github.com/ViennaTools/ViennaRay"
  EXCLUDE_FROM_ALL ${VIENNAPS_BUILD_PYTHON}
  OPTIONS "VIENNARAY_USE_GPU ${VIENNAPS_USE_GPU}")

if(VIENNAPS_USE_VTK)
  viennaps_prepare_vtk()
elseif(VIENNAPS_VTK_RENDERING)
  message(FATAL_ERROR "VIENNAPS_VTK_RENDERING requires VIENNAPS_USE_VTK=ON")
endif()

set(_VIENNAPS_HAD_CMAKE_SKIP_INSTALL_RULES FALSE)
if(DEFINED CMAKE_SKIP_INSTALL_RULES)
  set(_VIENNAPS_HAD_CMAKE_SKIP_INSTALL_RULES TRUE)
  set(_VIENNAPS_PREVIOUS_CMAKE_SKIP_INSTALL_RULES "${CMAKE_SKIP_INSTALL_RULES}")
endif()

if(VIENNAPS_BUILD_PYTHON)
  set(CMAKE_SKIP_INSTALL_RULES TRUE)
endif()

if(VIENNAPS_VIENNALS_SOURCE_DIR AND NOT CPM_ViennaLS_SOURCE)
  get_filename_component(_VIENNAPS_VIENNALS_SOURCE_DIR
                         "${VIENNAPS_VIENNALS_SOURCE_DIR}" ABSOLUTE)
  if(NOT EXISTS "${_VIENNAPS_VIENNALS_SOURCE_DIR}/CMakeLists.txt")
    message(
      FATAL_ERROR
        "[ViennaPS] VIENNAPS_VIENNALS_SOURCE_DIR does not contain CMakeLists.txt: ${_VIENNAPS_VIENNALS_SOURCE_DIR}"
    )
  endif()
  set(CPM_ViennaLS_SOURCE
      "${_VIENNAPS_VIENNALS_SOURCE_DIR}"
      CACHE PATH "Local ViennaLS source override used by CPM")
  message(
    STATUS
      "[ViennaPS] Using ViennaLS source from VIENNAPS_VIENNALS_SOURCE_DIR.")
endif()

set(_VIENNAPS_VIENNALS_PATCH_ARGS "")
if(NOT CPM_ViennaLS_SOURCE)
  list(APPEND _VIENNAPS_VIENNALS_PATCH_ARGS
       PATCHES
       "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/viennals-v5.8.5-levelset-update-v2.patch"
       CUSTOM_CACHE_KEY
       "v5.8.5-levelset-update-v2")
endif()

CPMAddPackage(
  NAME ViennaLS
  GIT_REPOSITORY "https://github.com/ViennaTools/ViennaLS"
  GIT_TAG v5.8.5
  ${_VIENNAPS_VIENNALS_PATCH_ARGS}
  EXCLUDE_FROM_ALL ${VIENNAPS_BUILD_PYTHON}
  OPTIONS "VIENNALS_PRECOMPILE_HEADERS ${VIENNAPS_PRECOMPILE_HEADERS}"
          "VIENNALS_USE_VTK ${VIENNAPS_USE_VTK}"
          "VIENNALS_VTK_RENDERING ${VIENNAPS_VTK_RENDERING}" "VIENNALS_USE_GPU ${VIENNAPS_USE_GPU}")

if(_VIENNAPS_HAD_CMAKE_SKIP_INSTALL_RULES)
  set(CMAKE_SKIP_INSTALL_RULES "${_VIENNAPS_PREVIOUS_CMAKE_SKIP_INSTALL_RULES}")
else()
  unset(CMAKE_SKIP_INSTALL_RULES)
endif()
unset(_VIENNAPS_HAD_CMAKE_SKIP_INSTALL_RULES)
unset(_VIENNAPS_PREVIOUS_CMAKE_SKIP_INSTALL_RULES)

CPMAddPackage(
  NAME ViennaCS
  VERSION 2.0.1
  GIT_REPOSITORY "https://github.com/ViennaTools/ViennaCS"
  EXCLUDE_FROM_ALL ${VIENNAPS_BUILD_PYTHON})

target_link_libraries(${PROJECT_NAME} INTERFACE ViennaCore ViennaLS ViennaRay ViennaCS)

# --------------------------------------------------------------------------------------------------------
# Setup Sanitizer or Clang-Tidy
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_ENABLE_SANITIZER)
  if(VIENNAPS_USE_GPU)
    message(WARNING "[ViennaPS] Sanitizers cannot be used with GPU support enabled!")
  else()
    viennacore_enable_sanitizer()
    message(STATUS "[ViennaPS] Enabled Sanitizers")
  endif()
endif()

if(VIENNAPS_ENABLE_CLANG_TIDY)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "clang-tidy not found")
  endif()

  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}" "--quiet" "--warnings-as-errors=*")
endif()

# --------------------------------------------------------------------------------------------------------
# Setup Shared/Static Library
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_PRECOMPILE_HEADERS)
  set(VIENNAPS_LINKAGE SHARED)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

  if(VIENNAPS_STATIC_BUILD)
    set(VIENNAPS_LINKAGE STATIC)
  endif()

  set(LIB_NAME "viennaps")
  add_library(${LIB_NAME} ${VIENNAPS_LINKAGE})

  target_link_libraries(${LIB_NAME} PRIVATE ${PROJECT_NAME})
  target_compile_definitions(${LIB_NAME} PRIVATE VIENNAPS_USE_PRECOMPILED=1
                                                 VIENNACORE_COMPILE_SHARED_LIB=1)
  target_sources(
    ${LIB_NAME} PRIVATE "lib/specMain.cpp" "lib/specGeometries.cpp" "lib/specModelsEmulation.cpp"
                        "lib/specModelsSimulation.cpp" "lib/specProcess.cpp")

  set_target_properties(
    ${LIB_NAME}
    PROPERTIES VERSION ${PROJECT_VERSION} # full x.y.z appears in filename
               SOVERSION ${PROJECT_VERSION_MAJOR} # major version for SONAME
               WINDOWS_EXPORT_ALL_SYMBOLS ON)

  target_link_libraries(${PROJECT_NAME} INTERFACE ${LIB_NAME})
  target_compile_definitions(${PROJECT_NAME} INTERFACE VIENNAPS_USE_PRECOMPILED)

  install(
    TARGETS ${LIB_NAME}
    EXPORT ViennaPSTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin)
endif()

# --------------------------------------------------------------------------------------------------------
# Setup GPU
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_USE_GPU)
  message(STATUS "[ViennaPS] Enabled GPU Support")
  add_subdirectory(gpu)
else()
  message(STATUS "[ViennaPS] Disabled GPU Support")
  set(VIENNAPS_USE_GPU OFF)
endif()

# Vulkan is independent of the existing CUDA/OptiX path. The probe can also be
# built as a diagnostic stub when Vulkan development headers are unavailable,
# so enabling it never turns on CUDA or breaks a CPU-only build.
include("cmake/ViennaPSVulkan.cmake")
viennaps_configure_vulkan_probe()

if(VIENNAPS_BUILD_VULKAN_PROBE OR VIENNAPS_BUILD_VULKAN_SMOKE OR
   VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OR
   VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
  add_subdirectory(gpu/vulkan)
endif()

# --------------------------------------------------------------------------------------------------------
# Setup Examples
# --------------------------------------------------------------------------------------------------------

include("cmake/ViennaPSUtils.cmake")

if(VIENNAPS_BUILD_EXAMPLES)
  message(STATUS "[ViennaPS] Building Examples")
  add_subdirectory(examples)
endif()

# --------------------------------------------------------------------------------------------------------
# Setup Tests
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_BUILD_TESTS)
  message(STATUS "[ViennaPS] Building Tests")

  enable_testing()
  add_subdirectory(tests)
endif()

# --------------------------------------------------------------------------------------------------------
# Setup Python Bindings
# --------------------------------------------------------------------------------------------------------

if(VIENNAPS_BUILD_PYTHON)
  message(STATUS "[ViennaPS] Building Python Bindings")
  add_subdirectory(python)
  return()
endif()

# --------------------------------------------------------------------------------------------------------
# Install Target
# --------------------------------------------------------------------------------------------------------

packageProject(
  NAME ${PROJECT_NAME} NAMESPACE ViennaTools
  VERSION ${PROJECT_VERSION}
  BINARY_DIR ${PROJECT_BINARY_DIR}
  INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include/viennaps
  INCLUDE_DESTINATION include/viennaps-${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  DEPENDENCIES "ViennaCore;ViennaLS;ViennaRay;ViennaCS")
