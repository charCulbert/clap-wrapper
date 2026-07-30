add_subdirectory(clap-first-example)

if (APPLE)
    add_subdirectory(clap-macos-embedded)

    add_executable(auv3-process-tests
        auv3-process-tests.mm
        ../src/detail/auv3/process.mm
    )
    target_include_directories(auv3-process-tests PRIVATE
        ..
        ../src
        ${CLAP_SDK_ROOT}/include
    )
    target_compile_features(auv3-process-tests PRIVATE cxx_std_17)
    target_compile_options(auv3-process-tests PRIVATE -fblocks)
    target_link_libraries(auv3-process-tests PRIVATE
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
    )
    add_test(NAME auv3-process COMMAND auv3-process-tests)
endif()

add_test(
    NAME clap-sdk-resolution
    COMMAND ${CMAKE_COMMAND}
        -DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clap-sdk-resolution
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-clap-sdk-resolution-tests.cmake
)
