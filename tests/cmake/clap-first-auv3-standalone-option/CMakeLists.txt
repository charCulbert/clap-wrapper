cmake_minimum_required(VERSION 3.21)
project(clap-first-auv3-standalone-option LANGUAGES CXX)

if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR OR NOT DEFINED TEST_CASE)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR and TEST_CASE are required")
endif()

# This fixture tests clap-first target selection without pretending that a
# non-Xcode generator can produce a loadable AUv3 bundle.
function(target_add_clap_configuration)
endfunction()

function(target_add_auv3_wrapper)
endfunction()

function(target_add_auv3_standalone_wrapper)
    cmake_parse_arguments(AUV3SA "" "TARGET" "" ${ARGN})
    target_sources(${AUV3SA_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fixture.cpp")
endfunction()

include("${CLAP_WRAPPER_SOURCE_DIR}/cmake/make_clapfirst.cmake")

add_library(fixture-impl STATIC fixture.cpp)

set(APPLE TRUE)
if (TEST_CASE STREQUAL "ios")
    set(CMAKE_SYSTEM_NAME iOS)
else()
    set(CMAKE_SYSTEM_NAME Darwin)
endif()
set(arguments
    TARGET_NAME fixture
    IMPL_TARGET fixture-impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fixture.cpp"
    PLUGIN_FORMATS CLAP AUV3
    COPY_AFTER_BUILD FALSE
    AUV2_MANUFACTURER_NAME "Fixture"
    AUV2_MANUFACTURER_CODE "Fxtr"
    AUV2_SUBTYPE_CODE "Test"
    AUV2_INSTRUMENT_TYPE "aumu"
)

if (TEST_CASE STREQUAL "true")
    list(APPEND arguments AUV3_BUILD_STANDALONE TRUE)
elseif (TEST_CASE STREQUAL "false")
    list(APPEND arguments AUV3_BUILD_STANDALONE FALSE)
elseif (TEST_CASE STREQUAL "invalid")
    list(APPEND arguments AUV3_BUILD_STANDALONE SOMETIMES)
elseif (TEST_CASE STREQUAL "missing")
    list(APPEND arguments AUV3_BUILD_STANDALONE)
elseif (TEST_CASE STREQUAL "ios")
elseif (NOT TEST_CASE STREQUAL "default")
    message(FATAL_ERROR "Unknown TEST_CASE '${TEST_CASE}'")
endif()

make_clapfirst_plugins(${arguments})

if (NOT TARGET fixture_auv3)
    message(FATAL_ERROR "${TEST_CASE}: AUv3 target was not created")
endif()

if (TEST_CASE STREQUAL "false" OR TEST_CASE STREQUAL "ios")
    if (TARGET fixture_auv3_standalone)
        message(FATAL_ERROR "${TEST_CASE}: AUv3 standalone target was unexpectedly created")
    endif()
elseif (NOT TARGET fixture_auv3_standalone)
    message(FATAL_ERROR "${TEST_CASE}: AUv3 standalone target was not created")
endif()
