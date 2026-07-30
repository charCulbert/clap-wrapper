if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR OR NOT DEFINED TEST_BINARY_ROOT)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR and TEST_BINARY_ROOT are required")
endif()

set(fixture_source
    "${CLAP_WRAPPER_SOURCE_DIR}/tests/cmake/clap-first-auv3-standalone-option")
file(REMOVE_RECURSE "${TEST_BINARY_ROOT}")

foreach(test_case default true false ios)
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

foreach(test_case invalid missing)
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
    if (result EQUAL 0)
        message(FATAL_ERROR
            "${test_case}: invalid AUV3_BUILD_STANDALONE unexpectedly configured")
    endif()

    set(diagnostic "${output}\n${error}")
    if (NOT diagnostic MATCHES "AUV3_BUILD_STANDALONE"
            OR NOT diagnostic MATCHES "TRUE or"
            OR NOT diagnostic MATCHES "FALSE")
        message(FATAL_ERROR
            "${test_case}: diagnostic was not actionable:\n${diagnostic}")
    endif()
endforeach()

message(STATUS "clap-first AUv3 standalone option configure checks passed")
