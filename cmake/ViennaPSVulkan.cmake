#[[
Vulkan probe build helpers.

This module intentionally provides only detection and hook utilities.
Root-level CMake wiring is done by the caller.
]]
include_guard(GLOBAL)

set(VIENNAPS_VULKAN_DEFAULT_NOTE "not-detected")

function(viennaps_configure_vulkan_probe)
  if(NOT DEFINED VIENNAPS_ENABLE_VULKAN)
    set(VIENNAPS_ENABLE_VULKAN OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PROBE)
    set(VIENNAPS_BUILD_VULKAN_PROBE OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_SMOKE)
    set(VIENNAPS_BUILD_VULKAN_SMOKE OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE)
    set(VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE)
    set(VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_RAY_SMOKE)
    set(VIENNAPS_BUILD_VULKAN_RAY_SMOKE OFF)
  endif()
  if(NOT DEFINED VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
    set(VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE OFF)
  endif()

  if(NOT VIENNAPS_ENABLE_VULKAN AND NOT VIENNAPS_BUILD_VULKAN_PROBE AND
     NOT VIENNAPS_BUILD_VULKAN_SMOKE AND
     NOT VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE AND
     NOT VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE AND
     NOT VIENNAPS_BUILD_VULKAN_RAY_SMOKE AND
     NOT VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE)
    set(VIENNAPS_VULKAN_SDK_AVAILABLE OFF PARENT_SCOPE)
    set(VIENNAPS_VULKAN_PROBE_NOTE
        "Vulkan support tools intentionally disabled by CMake options."
        PARENT_SCOPE)
    message(STATUS
            "[ViennaPS Vulkan Probe] disabled by CMake options: "
            "VIENNAPS_ENABLE_VULKAN=OFF, VIENNAPS_BUILD_VULKAN_PROBE=OFF, and "
            "VIENNAPS_BUILD_VULKAN_SMOKE=OFF, and "
            "VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE=OFF, and "
            "VIENNAPS_BUILD_VULKAN_LEVELSET_SMOKE=OFF, and "
            "VIENNAPS_BUILD_VULKAN_RAY_SMOKE=OFF, and "
            "VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE=OFF.")
    return()
  endif()

  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    set(VIENNAPS_VULKAN_SDK_AVAILABLE ON PARENT_SCOPE)
    set(VIENNAPS_VULKAN_PROBE_NOTE "Vulkan headers/libs detected at configure time."
        PARENT_SCOPE)
    set(VIENNAPS_VULKAN_TARGET Vulkan::Vulkan PARENT_SCOPE)
    message(STATUS "[ViennaPS Vulkan Probe] Vulkan SDK detected; probing target available.")
    return()
  endif()

  set(VIENNAPS_VULKAN_SDK_AVAILABLE OFF PARENT_SCOPE)
  set(VIENNAPS_VULKAN_PROBE_NOTE "Vulkan headers/libs not found (no usable Vulkan SDK)."
                                  PARENT_SCOPE)
  message(STATUS
          "[ViennaPS Vulkan Probe] Vulkan SDK not found; probe will run in diagnostic fallback mode.")
endfunction()
