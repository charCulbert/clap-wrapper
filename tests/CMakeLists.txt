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

if (TARGET base-sdk-vst3)
    add_executable(vst3-process-timing-tests
        vst3-process-timing-tests.cpp
        ../src/detail/vst3/process.cpp
        ../src/detail/vst3/parameter.cpp
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
    )
    target_include_directories(vst3-process-timing-tests PRIVATE
        ../src
        ../include
        ${CLAP_SDK_ROOT}/include
    )
    target_compile_features(vst3-process-timing-tests PRIVATE cxx_std_17)
    target_link_libraries(vst3-process-timing-tests PRIVATE
        clap
        base-sdk-vst3
        clap-wrapper-compile-options
    )
    if (APPLE)
        target_link_libraries(vst3-process-timing-tests PRIVATE "-framework CoreFoundation")
    endif()
    add_test(NAME vst3-process-timing COMMAND vst3-process-timing-tests)
endif()

if (APPLE)
    add_subdirectory(clap-macos-embedded)

    if (TARGET base-sdk-auv2)
        add_executable(auv2-midi-output-timing-tests auv2-midi-output-timing-tests.cpp)
        target_include_directories(auv2-midi-output-timing-tests PRIVATE
            ../src
            ../include
            ${CLAP_SDK_ROOT}/include
        )
        target_compile_features(auv2-midi-output-timing-tests PRIVATE cxx_std_17)
        target_link_libraries(auv2-midi-output-timing-tests PRIVATE
            clap
            base-sdk-auv2
            clap-wrapper-shared-detail
            clap-wrapper-compile-options
            "-framework AudioToolbox"
            "-framework CoreMIDI"
        )
        add_test(NAME auv2-midi-output-timing COMMAND auv2-midi-output-timing-tests)
    endif()

    add_executable(ios-auv3-midi-output-timing-tests ios-auv3-midi-output-timing-tests.cpp)
    target_compile_features(ios-auv3-midi-output-timing-tests PRIVATE cxx_std_17)
    add_test(NAME ios-auv3-midi-output-timing COMMAND ios-auv3-midi-output-timing-tests)

    add_executable(auv3-process-tests
        auv3-process-tests.mm
        ../src/detail/auv3/process.mm
    )
    add_executable(auv3-parameter-metadata-tests
        auv3-parameter-metadata-tests.mm
        ../src/detail/auv3/auv3_parameters.mm
    )
    # Runtime metadata parity is a Chardio integration test. Keep the
    # standalone clap-wrapper test suite independent of Chardio's optional
    # header and only register this target when the sibling checkout exists.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../chardio/clap/chardio_CLAPAUv3Metadata.h")
        add_executable(auv3-runtime-parity-tests
            auv3-runtime-parity-tests.mm
            ../src/detail/auv3/auv3_audiounit.mm
            ../src/detail/auv3/auv3_parameters.mm
            ../src/detail/auv3/process.mm
            ../src/clap_proxy.cpp
            ../src/detail/clap/fsutil.cpp
            ../src/detail/os/macos.mm
        )
    endif()
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
    if (TARGET auv3-runtime-parity-tests)
        target_include_directories(auv3-runtime-parity-tests PRIVATE
            ../../../
            ..
            ../src
            ../include
            ${CLAP_SDK_ROOT}/include
        )
    endif()
    target_compile_features(auv3-process-tests PRIVATE cxx_std_17)
    target_compile_features(auv3-parameter-metadata-tests PRIVATE cxx_std_17)
    if (TARGET auv3-runtime-parity-tests)
        target_compile_features(auv3-runtime-parity-tests PRIVATE cxx_std_17)
        target_compile_definitions(auv3-runtime-parity-tests PRIVATE
            CLAP_WRAPPER_VERSION="${CLAP_WRAPPER_VERSION}"
        )
    endif()
    target_compile_options(auv3-process-tests PRIVATE -fblocks)
    target_compile_options(auv3-parameter-metadata-tests PRIVATE -fblocks)
    if (TARGET auv3-runtime-parity-tests)
        target_compile_options(auv3-runtime-parity-tests PRIVATE -fblocks -fobjc-arc)
    endif()
    target_link_libraries(auv3-process-tests PRIVATE
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
    )
    target_link_libraries(auv3-parameter-metadata-tests PRIVATE
        "-framework AudioToolbox"
        "-framework Foundation"
    )
    if (TARGET auv3-runtime-parity-tests)
        target_link_libraries(auv3-runtime-parity-tests PRIVATE
            clap
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework CoreAudioKit"
            "-framework CoreMIDI"
            "-framework Foundation"
            "-framework AppKit"
        )
    endif()
    add_test(NAME auv3-process COMMAND auv3-process-tests)
    add_test(NAME auv3-parameter-metadata COMMAND auv3-parameter-metadata-tests)
    if (TARGET auv3-runtime-parity-tests)
        add_test(NAME auv3-runtime-parity COMMAND auv3-runtime-parity-tests)
    endif()
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
