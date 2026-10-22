cmake_minimum_required(VERSION 3.12)
project(gdxglegacy_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
enable_testing()
add_executable(gdxglegacy_tests
    profile_test.cpp
    ../../games/gitadora/legacy_profile.cpp
    ../../external/tinyxml2/tinyxml2.cpp)
target_include_directories(gdxglegacy_tests PRIVATE ../..)
add_test(NAME gdxglegacy_profile_and_buffer COMMAND gdxglegacy_tests)
add_executable(gdxglegacy_input_tests input_test.cpp)
target_include_directories(gdxglegacy_input_tests PRIVATE ../..)
add_test(NAME gdxglegacy_raw_input_and_buffer COMMAND gdxglegacy_input_tests)
