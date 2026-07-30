cmake_minimum_required(VERSION 3.21)
project(clap-sdk-resolution LANGUAGES C CXX)

if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR is required")
endif()

function(write_fake_clap_sdk root)
    file(MAKE_DIRECTORY "${root}/include/clap")
    file(WRITE "${root}/include/clap/clap.h" "/* configure-only CLAP fixture */\n")
    file(WRITE "${root}/CMakeLists.txt"
        "add_library(clap INTERFACE)\n"
        "target_include_directories(clap INTERFACE \"${root}/include\")\n")
endfunction()

set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
set(CPMAddPackage TRUE)

if (TEST_CASE STREQUAL "fallback")
    function(CPMAddPackage)
        cmake_parse_arguments(CPM "" "NAME;GIT_TAG;SOURCE_DIR" "" ${ARGN})
        if (NOT CPM_NAME STREQUAL "clap")
            message(FATAL_ERROR "Unexpected fallback dependency: ${CPM_NAME}")
        endif()
        if (NOT CPM_GIT_TAG STREQUAL "1.2.10")
            message(FATAL_ERROR "Expected CLAP fallback 1.2.10, got ${CPM_GIT_TAG}")
        endif()
        write_fake_clap_sdk("${CMAKE_CURRENT_BINARY_DIR}/${CPM_SOURCE_DIR}")
    endfunction()
elseif (TEST_CASE STREQUAL "explicit-root")
    set(CLAP_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/explicit-clap")
    write_fake_clap_sdk("${CLAP_SDK_ROOT}")
    function(CPMAddPackage)
        message(FATAL_ERROR "Explicit CLAP_SDK_ROOT must not download CLAP")
    endfunction()
elseif (TEST_CASE STREQUAL "parent-target")
    add_library(clap INTERFACE)
    set(CLAP_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/intentionally-unused")
    function(CPMAddPackage)
        message(FATAL_ERROR "An existing parent clap target must not download CLAP")
    endfunction()
elseif (TEST_CASE STREQUAL "invalid-root")
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF)
    set(CLAP_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/missing-clap")
else()
    message(FATAL_ERROR "Unknown TEST_CASE '${TEST_CASE}'")
endif()

add_subdirectory("${CLAP_WRAPPER_SOURCE_DIR}" clap-wrapper)

if (NOT TARGET clap OR NOT TARGET base-sdk-clap)
    message(FATAL_ERROR "clap-wrapper did not provide its expected CLAP targets")
endif()

if (TEST_CASE STREQUAL "parent-target")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/impl.cpp" "void fixture_impl() {}\n")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/entry.cpp" "void fixture_entry() {}\n")
    add_library(fixture-impl STATIC "${CMAKE_CURRENT_BINARY_DIR}/impl.cpp")
    make_clapfirst_plugins(
        TARGET_NAME fixture
        IMPL_TARGET fixture-impl
        ENTRY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/entry.cpp"
        PLUGIN_FORMATS CLAP
        COPY_AFTER_BUILD FALSE
    )
    if (NOT TARGET fixture_clap)
        message(FATAL_ERROR "make_clapfirst_plugins did not create its CLAP target")
    endif()
endif()
