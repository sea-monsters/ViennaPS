cmake_minimum_required(VERSION 3.20)

project(ViennaPSInstallExportConsumer LANGUAGES CXX)

find_package(ViennaPS 4.6 CONFIG REQUIRED)

add_executable(viennaps-install-export-consumer consumer.cpp)
target_compile_features(viennaps-install-export-consumer PRIVATE cxx_std_20)
target_link_libraries(viennaps-install-export-consumer
                      PRIVATE ViennaTools::ViennaPS)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME viennaps-install-export-consumer
           COMMAND viennaps-install-export-consumer)
endif()
