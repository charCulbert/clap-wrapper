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

add_executable(standalone-midi-mapping-tests standalone-midi-mapping-tests.cpp)
target_include_directories(standalone-midi-mapping-tests PRIVATE
    ../src
    ${CLAP_SDK_ROOT}/include
)
target_compile_features(standalone-midi-mapping-tests PRIVATE cxx_std_17)
target_link_libraries(standalone-midi-mapping-tests PRIVATE Threads::Threads)
add_test(NAME standalone-midi-mapping COMMAND standalone-midi-mapping-tests)

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
    target_include_directories(auv3-process-tests PRIVATE
        ..
        ../src
        ../include
        ${CLAP_SDK_ROOT}/include
    )
    target_compile_features(auv3-process-tests PRIVATE cxx_std_17)
    target_compile_options(auv3-process-tests PRIVATE -fblocks)
    target_link_libraries(auv3-process-tests PRIVATE
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreMIDI"
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

add_test(
    NAME clap-first-auv3-standalone-option
    COMMAND ${CMAKE_COMMAND}
        -DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clap-first-auv3-standalone-option
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-clap-first-auv3-standalone-option-tests.cmake
)
