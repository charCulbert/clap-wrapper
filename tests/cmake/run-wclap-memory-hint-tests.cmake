# Checks the memory.json hint helpers in cmake/wrap_wclap.cmake without a wasm toolchain.
if (NOT DEFINED CLAP_WRAPPER_SOURCE_DIR OR NOT DEFINED TEST_BINARY_ROOT)
    message(FATAL_ERROR "CLAP_WRAPPER_SOURCE_DIR and TEST_BINARY_ROOT are required")
endif()
include("${CLAP_WRAPPER_SOURCE_DIR}/cmake/wrap_wclap.cmake")

clap_wrapper_parse_byte_size("131072" bytes)
if (NOT bytes EQUAL 131072)
    message(FATAL_ERROR "plain bytes parsed as ${bytes}")
endif()
clap_wrapper_parse_byte_size("512KiB" bytes)
if (NOT bytes EQUAL 524288)
    message(FATAL_ERROR "512KiB parsed as ${bytes}")
endif()
clap_wrapper_parse_byte_size("64MiB" bytes)
if (NOT bytes EQUAL 67108864)
    message(FATAL_ERROR "64MiB parsed as ${bytes}")
endif()
clap_wrapper_parse_byte_size("1GiB" bytes)
if (NOT bytes EQUAL 1073741824)
    message(FATAL_ERROR "1GiB parsed as ${bytes}")
endif()

clap_wrapper_wclap_memory_hint_json(json MINIMUM 128KiB INITIAL 64MiB MAXIMUM 130MiB)
set(expected "{\n  \"minimumBytes\": 131072,\n  \"recommendedInitialBytes\": 67108864,\n  \"recommendedMaximumBytes\": 136314880,\n  \"shared\": true\n}\n")
if (NOT json STREQUAL expected)
    message(FATAL_ERROR "unexpected memory.json:\n${json}")
endif()

# Invalid inputs must fail configure with a message naming the problem. Each case
# runs in a child cmake -P so the FATAL_ERROR does not end this script.
set(fixture "${TEST_BINARY_ROOT}/invalid-case.cmake")
file(MAKE_DIRECTORY "${TEST_BINARY_ROOT}")
foreach (case
        "MINIMUM 1MiB INITIAL 2MiB|WCLAP_MEMORY_MAXIMUM"
        "MINIMUM 4MiB INITIAL 2MiB MAXIMUM 8MiB|MINIMUM <= INITIAL <= MAXIMUM"
        "MINIMUM 1MiB INITIAL 2MiB MAXIMUM 4GiB|below 4GiB"
        "MINIMUM 1024 INITIAL 2MiB MAXIMUM 8MiB|64KiB"
        "MINIMUM 1MB INITIAL 2MiB MAXIMUM 8MiB|not a byte size")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 arguments)
    list(GET parts 1 needle)
    file(WRITE "${fixture}"
        "include(\"${CLAP_WRAPPER_SOURCE_DIR}/cmake/wrap_wclap.cmake\")\n"
        "clap_wrapper_wclap_memory_hint_json(json ${arguments})\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${fixture}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if (result EQUAL 0)
        message(FATAL_ERROR "'${arguments}' was accepted")
    endif()
    if (NOT "${output}${error}" MATCHES "${needle}")
        message(FATAL_ERROR "'${arguments}' failed without naming '${needle}':\n${output}${error}")
    endif()
endforeach()

message(STATUS "WCLAP memory hint checks passed")
