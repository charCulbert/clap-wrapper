if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR OR NOT DEFINED TEST_BINARY_ROOT)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR and TEST_BINARY_ROOT are required")
endif()

set(fixture_source
    "${CLAP_WRAPPER_SOURCE_DIR}/tests/cmake/clap-sdk-resolution")
file(REMOVE_RECURSE "${TEST_BINARY_ROOT}")

foreach(test_case fallback explicit-root parent-target)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${fixture_source}"
            -B "${TEST_BINARY_ROOT}/${test_case}"
            "-DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_SOURCE_DIR}"
            "-DTEST_CASE=${test_case}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
        message(FATAL_ERROR
            "${test_case} configure failed:\n${output}\n${error}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${fixture_source}"
        -B "${TEST_BINARY_ROOT}/invalid-root"
        "-DCLAP_WRAPPER_SOURCE_DIR=${CLAP_WRAPPER_SOURCE_DIR}"
        -DTEST_CASE=invalid-root
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if (result EQUAL 0)
    message(FATAL_ERROR "Invalid CLAP_SDK_ROOT unexpectedly configured")
endif()

set(diagnostic "${output}\n${error}")
if (NOT diagnostic MATCHES "CLAP_SDK_ROOT does not point to a valid CLAP SDK"
    OR NOT diagnostic MATCHES "include/clap/clap.h")
    message(FATAL_ERROR "Invalid-root diagnostic was not actionable:\n${diagnostic}")
endif()

message(STATUS "CLAP SDK resolution configure checks passed")
