add_subdirectory(clap-first-example)

find_package(Threads REQUIRED)
add_executable(parameter-flush-lifecycle-tests parameter-flush-lifecycle-tests.cpp)
target_include_directories(parameter-flush-lifecycle-tests PRIVATE
    ../src
    ${CLAP_SDK_ROOT}/include
)
target_compile_features(parameter-flush-lifecycle-tests PRIVATE cxx_std_17)
target_link_libraries(parameter-flush-lifecycle-tests PRIVATE Threads::Threads)
add_test(NAME parameter-flush-lifecycle COMMAND parameter-flush-lifecycle-tests)

add_executable(standalone-services-tests
    standalone-services-tests.cpp
    ../src/detail/standalone/standalone_services_core.cpp
    ../src/detail/standalone/standalone_settings.cpp
)
target_include_directories(standalone-services-tests PRIVATE
    ../src
    ../include
    ${CLAP_SDK_ROOT}/include
)
target_compile_features(standalone-services-tests PRIVATE cxx_std_17)
target_link_libraries(standalone-services-tests PRIVATE Threads::Threads)
add_test(NAME standalone-services COMMAND standalone-services-tests)

if (APPLE)
    add_subdirectory(clap-macos-embedded)

    add_executable(auv3-process-tests
        auv3-process-tests.mm
        ../src/detail/auv3/process.mm
    )
    add_executable(auv3-parameter-metadata-tests
        auv3-parameter-metadata-tests.mm
        ../src/detail/auv3/auv3_parameters.mm
    )
    target_include_directories(auv3-process-tests PRIVATE
        ..
        ../src
        ../include
        ${CLAP_SDK_ROOT}/include
    )
    target_include_directories(auv3-parameter-metadata-tests PRIVATE
        ..
        ../src
        ../include
        ${CLAP_SDK_ROOT}/include
    )
    target_compile_features(auv3-process-tests PRIVATE cxx_std_17)
    target_compile_features(auv3-parameter-metadata-tests PRIVATE cxx_std_17)
    target_compile_options(auv3-process-tests PRIVATE -fblocks)
    target_compile_options(auv3-parameter-metadata-tests PRIVATE -fblocks)
    target_link_libraries(auv3-process-tests PRIVATE
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
    )
    target_link_libraries(auv3-parameter-metadata-tests PRIVATE
        "-framework AudioToolbox"
        "-framework Foundation"
    )
    add_test(NAME auv3-process COMMAND auv3-process-tests)
    add_test(NAME auv3-parameter-metadata COMMAND auv3-parameter-metadata-tests)
endif()

add_test(
    NAME clap-sdk-resolution
    COMMAND ${CMAKE_COMMAND}
        -DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clap-sdk-resolution
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-clap-sdk-resolution-tests.cmake
)

add_test(
    NAME clap-first-auv3-standalone-option
    COMMAND ${CMAKE_COMMAND}
        -DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clap-first-auv3-standalone-option
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-clap-first-auv3-standalone-option-tests.cmake
)
