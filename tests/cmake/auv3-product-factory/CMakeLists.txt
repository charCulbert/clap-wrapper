cmake_minimum_required(VERSION 3.21)

if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR OR NOT DEFINED TEST_CASE)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR and TEST_CASE are required")
endif()

project(auv3-product-factory LANGUAGES CXX OBJCXX)
if (TEST_CASE STREQUAL "swift-custom")
    enable_language(Swift)
    add_library(swift_factory_validation STATIC ProductSwiftFactory.swift)
endif()

function(macos_include_clap_in_bundle)
endfunction()

function(macos_bundle_flag)
endfunction()

function(target_copy_after_build)
endfunction()

set(CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR "${CLAP_WRAPPER_SOURCE_DIR}")
set(CLAP_WRAPPER_COPY_AFTER_BUILD FALSE)
if (TEST_CASE STREQUAL "ios-custom-host")
    set(CMAKE_SYSTEM_NAME iOS)
endif()
add_library(clap-wrapper-compile-options INTERFACE)
target_compile_features(clap-wrapper-compile-options INTERFACE cxx_std_17)
add_library(clap-wrapper-shared-detail INTERFACE)
add_library(macos_filesystem_support INTERFACE)
target_include_directories(clap-wrapper-shared-detail INTERFACE
    "${CLAP_WRAPPER_SOURCE_DIR}/src"
    "${CLAP_WRAPPER_SOURCE_DIR}/include"
    "${CLAP_WRAPPER_SOURCE_DIR}/../clap/include")
include("${CLAP_WRAPPER_SOURCE_DIR}/cmake/wrap_auv3.cmake")
include("${CLAP_WRAPPER_SOURCE_DIR}/cmake/wrap_auv3_standalone_ios.cmake")

add_executable(fixture)
set(arguments
    TARGET fixture
    OUTPUT_NAME Fixture
    BUNDLE_IDENTIFIER org.example.fixture.auv3
    BUNDLE_VERSION 1.0.0
    MANUFACTURER_NAME Fixture
    MANUFACTURER_CODE Fxtr
    SUBTYPE_CODE Test
    INSTRUMENT_TYPE aumu
)

if (TEST_CASE STREQUAL "custom")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm"
        FACTORY_CLASS ProductFactory)
elseif (TEST_CASE STREQUAL "ios-custom-host")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm"
        FACTORY_CLASS ProductFactory)
elseif (TEST_CASE STREQUAL "swift-custom")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductSwiftFactory.swift"
        FACTORY_CLASS ProductSwiftFactory)
elseif (TEST_CASE STREQUAL "missing-class")
    list(APPEND arguments FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm")
elseif (TEST_CASE STREQUAL "missing-source")
    list(APPEND arguments FACTORY_CLASS ProductFactory)
elseif (TEST_CASE STREQUAL "missing-source-value")
    list(APPEND arguments FACTORY_SOURCE)
elseif (TEST_CASE STREQUAL "missing-class-value")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm"
        FACTORY_CLASS)
elseif (TEST_CASE STREQUAL "invalid-class")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm"
        FACTORY_CLASS Product.Factory)
elseif (TEST_CASE STREQUAL "nonexistent-source")
    list(APPEND arguments
        FACTORY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/missing.mm"
        FACTORY_CLASS ProductFactory)
elseif (NOT TEST_CASE STREQUAL "default")
    message(FATAL_ERROR "Unknown TEST_CASE '${TEST_CASE}'")
endif()

target_add_auv3_wrapper(${arguments})

get_property(factoryClass TARGET fixture PROPERTY CLAP_WRAPPER_AUV3_FACTORY_CLASS)
get_property(factorySource TARGET fixture PROPERTY CLAP_WRAPPER_AUV3_FACTORY_SOURCE)
if (TEST_CASE STREQUAL "custom" OR TEST_CASE STREQUAL "ios-custom-host" OR TEST_CASE STREQUAL "swift-custom")
    if (TEST_CASE STREQUAL "custom" OR TEST_CASE STREQUAL "ios-custom-host")
        set(expectedFactoryClass ProductFactory)
        set(expectedFactorySource "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm")
    else()
        set(expectedFactoryClass ProductSwiftFactory)
        set(expectedFactorySource "${CMAKE_CURRENT_SOURCE_DIR}/ProductSwiftFactory.swift")
    endif()
    if (NOT factoryClass STREQUAL expectedFactoryClass)
        message(FATAL_ERROR "Custom factory class was not recorded on the AUv3 target")
    endif()
    if (NOT factorySource STREQUAL expectedFactorySource)
        message(FATAL_ERROR "Custom factory source was not recorded on the AUv3 target")
    endif()
    get_property(sources TARGET fixture PROPERTY SOURCES)
    if (NOT expectedFactorySource IN_LIST sources)
        message(FATAL_ERROR "Custom factory source was not added to the AUv3 target")
    endif()
elseif (factoryClass OR factorySource)
    message(FATAL_ERROR "Default AUv3 path unexpectedly recorded a product factory")
endif()

if (TEST_CASE STREQUAL "ios-custom-host")
    add_executable(fixture_host)
    target_add_auv3_standalone_ios_wrapper(
        TARGET fixture_host
        OUTPUT_NAME "Fixture Host"
        BUNDLE_IDENTIFIER org.example.fixture.host
        BUNDLE_VERSION 1.0.0
        AUV3_TARGET fixture
        AU_TYPE aumu
        AU_SUBTYPE Test
        AU_MANUFACTURER Fxtr)
    get_property(hostSources TARGET fixture_host PROPERTY SOURCES)
    if (NOT "${CMAKE_CURRENT_SOURCE_DIR}/ProductFactory.mm" IN_LIST hostSources)
        message(FATAL_ERROR "Custom factory source was not added to the static iOS host target")
    endif()
endif()
