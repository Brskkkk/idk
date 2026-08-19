cmake_minimum_required(VERSION 3.22.1)
project(frameflow LANGUAGES CXX)

add_library(frameflow SHARED frameflow.cpp)

target_compile_features(frameflow PRIVATE cxx_std_20)
target_compile_options(frameflow PRIVATE -O3 -ffast-math -fno-exceptions -fno-rtti)

target_link_libraries(frameflow PRIVATE android log)
