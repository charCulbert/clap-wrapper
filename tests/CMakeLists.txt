add_subdirectory(clap-first-example)

if (APPLE)
    add_subdirectory(clap-macos-embedded)
endif()

add_test(
    NAME clap-sdk-resolution
    COMMAND ${CMAKE_COMMAND}
        -DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clap-sdk-resolution
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-clap-sdk-resolution-tests.cmake
)
