cmake_minimum_required(VERSION 3.12)
project(gdxglegacy_tests LANGUAGES C CXX)
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

if(WIN32)
    add_executable(gdxglegacy_movie_pool_tests
        movie_pool_test.cpp ../../games/gitadora/legacy_movie_pool.cpp)
    target_include_directories(gdxglegacy_movie_pool_tests PRIVATE ../..)
    add_test(NAME gdxglegacy_movie_reservation COMMAND gdxglegacy_movie_pool_tests)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_executable(gdxglegacy_movie_hook_tests movie_hook_test.cpp
            ../../games/gitadora/legacy_movie.cpp ../../games/gitadora/legacy_movie_pool.cpp)
        target_include_directories(gdxglegacy_movie_hook_tests PRIVATE ../..)
        target_compile_definitions(gdxglegacy_movie_hook_tests PRIVATE FMT_HEADER_ONLY)
        add_test(NAME gdxglegacy_movie_hook_cn COMMAND gdxglegacy_movie_hook_tests cn)
        add_test(NAME gdxglegacy_movie_hook_jp COMMAND gdxglegacy_movie_hook_tests jp)
        add_test(NAME gdxglegacy_movie_hook_unknown COMMAND gdxglegacy_movie_hook_tests cn timestamp)
        add_test(NAME gdxglegacy_movie_hook_bad_release COMMAND gdxglegacy_movie_hook_tests jp corrupt)
        add_test(NAME gdxglegacy_movie_hook_bad_game_release COMMAND gdxglegacy_movie_hook_tests cn game-release)
        add_test(NAME gdxglegacy_movie_hook_unknown_game COMMAND gdxglegacy_movie_hook_tests jp game-timestamp)
    endif()
    add_subdirectory(../../external/minhook ${CMAKE_CURRENT_BINARY_DIR}/minhook EXCLUDE_FROM_ALL)
    add_library(gdxg_codec_consumer SHARED codec_consumer.cpp)
    set_target_properties(gdxg_codec_consumer PROPERTIES PREFIX "")
    target_link_libraries(gdxg_codec_consumer PRIVATE ole32)
    add_library(gdxg_codec_creation_failure SHARED codec_failure.cpp codec_failure.def)
    target_link_libraries(gdxg_codec_creation_failure PRIVATE uuid)
    set_target_properties(gdxg_codec_creation_failure PROPERTIES PREFIX "")
    add_library(gdxg_codec_factory_failure SHARED codec_failure.cpp codec_failure.def)
    target_compile_definitions(gdxg_codec_factory_failure PRIVATE FAIL_FACTORY)
    target_link_libraries(gdxg_codec_factory_failure PRIVATE uuid)
    set_target_properties(gdxg_codec_factory_failure PROPERTIES PREFIX "")
    add_executable(gdxglegacy_codec_tests
        codec_test.cpp
        ../../games/gitadora/legacy_codec.cpp
        ../../util/detour.cpp
        ../../util/memutils.cpp
        ../../util/peb.cpp)
    target_include_directories(gdxglegacy_codec_tests PRIVATE ../..)
    target_compile_definitions(gdxglegacy_codec_tests PRIVATE FMT_HEADER_ONLY)
    target_link_libraries(gdxglegacy_codec_tests PRIVATE ole32 strmiids uuid psapi minhook)
    if(NOT MSVC)
        target_link_options(gdxg_codec_creation_failure PRIVATE -static)
        target_link_options(gdxg_codec_factory_failure PRIVATE -static)
        target_compile_options(gdxglegacy_codec_tests PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(gdxglegacy_codec_tests PRIVATE -Wl,--gc-sections)
    endif()
    # Integration cases supply a user-owned, matching x86 AX beside the test
    # executable. Keep proprietary codecs and machine COM registration out of CI.
endif()
