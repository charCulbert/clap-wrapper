# This adds a WCLAP target, but must only be called when building WASM (with Emscripten or WASI-SDK)

# Parses a byte size written as plain bytes or with a binary suffix:
# "134217728", "128MiB", "512KiB", "1GiB". Sets ${output_variable} in the caller.
function(clap_wrapper_parse_byte_size input output_variable)
    if ("${input}" MATCHES "^([0-9]+)(KiB|MiB|GiB)?$")
        set(number "${CMAKE_MATCH_1}")
        set(unit "${CMAKE_MATCH_2}")
        if (unit STREQUAL "KiB")
            math(EXPR number "${number} * 1024")
        elseif (unit STREQUAL "MiB")
            math(EXPR number "${number} * 1048576")
        elseif (unit STREQUAL "GiB")
            math(EXPR number "${number} * 1073741824")
        endif()
        set(${output_variable} "${number}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
                "clap-wrapper: '${input}' is not a byte size; use bytes or a KiB/MiB/GiB suffix, e.g. 64MiB")
    endif()
endfunction()

# Builds the memory.json hint a WCLAP archive can carry for browser hosts
# (minimumBytes / recommendedInitialBytes / recommendedMaximumBytes, shared).
# All three sizes are required; a host only reads the file when it is complete.
# Sets ${output_variable} to the JSON text in the caller.
function(clap_wrapper_wclap_memory_hint_json output_variable)
    set(oneValueArgs MINIMUM INITIAL MAXIMUM)
    cmake_parse_arguments(MEM "" "${oneValueArgs}" "" ${ARGN})
    foreach (key IN ITEMS MINIMUM INITIAL MAXIMUM)
        if (NOT DEFINED MEM_${key} OR MEM_${key} STREQUAL "")
            message(FATAL_ERROR
                    "clap-wrapper: the WCLAP memory hint needs all of WCLAP_MEMORY_MINIMUM, "
                    "WCLAP_MEMORY_INITIAL and WCLAP_MEMORY_MAXIMUM (missing ${key})")
        endif()
        clap_wrapper_parse_byte_size("${MEM_${key}}" MEM_${key}_BYTES)
    endforeach()
    set(page 65536)
    set(wasm32_limit 4294967296)
    if (MEM_MINIMUM_BYTES LESS page)
        message(FATAL_ERROR
                "clap-wrapper: WCLAP_MEMORY_MINIMUM must be at least one 64KiB WebAssembly page")
    endif()
    if (MEM_INITIAL_BYTES LESS MEM_MINIMUM_BYTES OR MEM_MAXIMUM_BYTES LESS MEM_INITIAL_BYTES)
        message(FATAL_ERROR
                "clap-wrapper: WCLAP memory hint must satisfy MINIMUM <= INITIAL <= MAXIMUM "
                "(got ${MEM_MINIMUM_BYTES} <= ${MEM_INITIAL_BYTES} <= ${MEM_MAXIMUM_BYTES})")
    endif()
    if (MEM_MAXIMUM_BYTES GREATER_EQUAL wasm32_limit)
        message(FATAL_ERROR
                "clap-wrapper: WCLAP_MEMORY_MAXIMUM must be below 4GiB (wasm32 address space)")
    endif()
    set(${output_variable}
        "{\n  \"minimumBytes\": ${MEM_MINIMUM_BYTES},\n  \"recommendedInitialBytes\": ${MEM_INITIAL_BYTES},\n  \"recommendedMaximumBytes\": ${MEM_MAXIMUM_BYTES},\n  \"shared\": true\n}\n"
        PARENT_SCOPE)
endfunction()

function(target_add_wclap_configuration)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            RESOURCE_DIRECTORY
            MEMORY_MINIMUM   # optional memory.json hint for browser hosts; see
            MEMORY_INITIAL   # clap_wrapper_wclap_memory_hint_json. Needs RESOURCE_DIRECTORY
            MEMORY_MAXIMUM   # because the file travels inside the .wclap bundle.
    )
    cmake_parse_arguments(TCLP "" "${oneValueArgs}" "" ${ARGN} )

    set(memory_hint_requested FALSE)
    foreach (key IN ITEMS MINIMUM INITIAL MAXIMUM)
        if (DEFINED TCLP_MEMORY_${key})
            set(memory_hint_requested TRUE)
        endif()
    endforeach()

    if (NOT ANY_WASM_TOOLCHAIN)
        message(FATAL_ERROR "Do not call this outside the Emscripten/WASI toolchain")
    endif()

    if (NOT DEFINED TCLP_TARGET)
        message(FATAL_ERROR "You must define TARGET in target_library_is_clap")
    endif()

    if (NOT DEFINED TCLP_OUTPUT_NAME)
        message(STATUS "Using target name as clap name in target_library_is_clap")
        set(TCLP_OUTPUT_NAME TCLP_TARGET)
    endif()

    # If a resource directory is defined, make a WCLAP bundle
    if(TCLP_RESOURCE_DIRECTORY AND NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
        set_target_properties(${TCLP_TARGET}
                PROPERTIES
                OUTPUT_NAME "${TCLP_OUTPUT_NAME}"
                SUFFIX ".wclap/module.wasm"
                PREFIX ""
        )
        # Make sure directory exists
        add_custom_command(TARGET ${TCLP_TARGET} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TCLP_TARGET}>"
        )

        add_custom_command(TARGET ${TCLP_TARGET} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${TCLP_RESOURCE_DIRECTORY}" "$<TARGET_FILE_DIR:${TCLP_TARGET}>"
        )
        if (memory_hint_requested)
            clap_wrapper_wclap_memory_hint_json(memory_hint_json
                    MINIMUM "${TCLP_MEMORY_MINIMUM}"
                    INITIAL "${TCLP_MEMORY_INITIAL}"
                    MAXIMUM "${TCLP_MEMORY_MAXIMUM}")
            set(memory_hint_file "${CMAKE_CURRENT_BINARY_DIR}/${TCLP_TARGET}-memory.json")
            file(WRITE "${memory_hint_file}" "${memory_hint_json}")
            add_custom_command(TARGET ${TCLP_TARGET} PRE_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${memory_hint_file}" "$<TARGET_FILE_DIR:${TCLP_TARGET}>/memory.json"
            )
        endif()
	# .tar.gz bundle of the directory
        add_custom_command(TARGET ${TCLP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E tar cz "$<TARGET_FILE_DIR:${TCLP_TARGET}>.tar.gz" "$<TARGET_FILE_DIR:${TCLP_TARGET}>"
        )
    else()
        if (memory_hint_requested)
            message(FATAL_ERROR
                    "clap-wrapper: a WCLAP memory hint needs a bundle; give RESOURCE_DIRECTORY "
                    "so memory.json can be packaged next to module.wasm")
        endif()
        set_target_properties(${TCLP_TARGET}
                PROPERTIES
                OUTPUT_NAME "${TCLP_OUTPUT_NAME}"
                SUFFIX ".wclap.wasm"
                PREFIX ""
        )
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${TCLP_TARGET} FLAVOR wclap)
    endif()
endfunction(target_add_wclap_configuration)
