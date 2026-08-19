cmake_minimum_required(VERSION 3.20)

project(ViennaPSVulkanInstallExportConsumer LANGUAGES CXX)

find_package(ViennaPS 4.6 CONFIG REQUIRED)

add_executable(viennaps-vulkan-install-export-consumer consumer.cpp)
target_compile_features(viennaps-vulkan-install-export-consumer PRIVATE cxx_std_20)
target_link_libraries(viennaps-vulkan-install-export-consumer
                      PRIVATE ViennaTools::viennaps_vulkan_compute_runtime)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME viennaps-vulkan-install-export-consumer
           COMMAND viennaps-vulkan-install-export-consumer)
endif()
