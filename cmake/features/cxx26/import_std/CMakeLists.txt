# Nested project for the import_std probe (cmake/SlFeatures.cmake runs it through try_compile PROJECT).
# SL_IMPORT_STD_GATE arrives as -D from the one place that defines it (SlFeatures.cmake).
cmake_minimum_required(VERSION 3.30)
if(SL_IMPORT_STD_GATE)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "${SL_IMPORT_STD_GATE}")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
project(SlImportStdProbe LANGUAGES CXX)
add_executable(sl_import_std main.cpp)
