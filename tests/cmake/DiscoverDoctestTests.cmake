# ---------------------------------------------------------------------------
# CTest discovery for doctest binaries.
#
# Registers ONE CTest row per doctest test case, so `ctest` names the exact
# behaviour that failed instead of only reporting "the binary failed".
#
# The upstream helper (doctest/scripts/cmake/doctest.cmake) filters the
# listing output with the regex `[^\[doctest\] ]`. That filter is kept here
# only as a cross-check: we own the parsing so a mis-parse can never silently
# produce a wrong test matrix, and the number of discovered cases is verified
# against the number of registered cases reported by `--count`.
# ---------------------------------------------------------------------------

# Captured at include time: inside a function, CMAKE_CURRENT_LIST_DIR refers to
# the caller's directory, not to this file.
set(SCIRENDER_DISCOVER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/DiscoverDoctestTests.cmake"
    CACHE INTERNAL "Path of the doctest CTest discovery script")

function(scirender_discover_tests TARGET)
    set(_tests "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_tests.cmake")
    set(_include "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_include.cmake")

    add_custom_command(TARGET ${TARGET} POST_BUILD
        BYPRODUCTS "${_tests}"
        COMMAND ${CMAKE_COMMAND}
                -D "TEST_EXECUTABLE=$<TARGET_FILE:${TARGET}>"
                -D "TEST_OUTPUT=${_tests}"
                -D "TEST_WORKING_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${SCIRENDER_DISCOVER_SCRIPT}"
        VERBATIM)

    file(WRITE "${_include}"
         "if(EXISTS \"${_tests}\")\n"
         "  include(\"${_tests}\")\n"
         "else()\n"
         "  add_test(${TARGET}_NOT_BUILT ${TARGET}_NOT_BUILT)\n"
         "endif()\n")

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_include}")
endfunction()

# ---------------------------------------------------------------------------
# Script mode: query the binary and emit the add_test() calls.
# ---------------------------------------------------------------------------
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT EXISTS "${TEST_EXECUTABLE}")
        message(FATAL_ERROR "Test executable '${TEST_EXECUTABLE}' does not exist")
    endif()

    execute_process(
        COMMAND "${TEST_EXECUTABLE}" --list-test-cases --no-colors --no-version --no-intro
        OUTPUT_VARIABLE listing
        ERROR_VARIABLE listing_err
        RESULT_VARIABLE listing_result
        WORKING_DIRECTORY "${TEST_WORKING_DIR}")

    if(NOT listing_result EQUAL 0)
        message(FATAL_ERROR
                "Failed to list test cases of '${TEST_EXECUTABLE}' (exit ${listing_result})\n${listing_err}")
    endif()

    # doctest prints:
    #   [doctest] listing all test case names
    #   =====...
    #   <test case name>
    #   ...
    #   =====...
    #   [doctest] unskipped test cases passing the current filters: N
    string(REPLACE "\n" ";" listing "${listing}")

    set(cases "")
    foreach(line IN LISTS listing)
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        if(line MATCHES "^\\[doctest\\]")
            continue()
        endif()
        if(line MATCHES "^=+$")
            continue()
        endif()
        list(APPEND cases "${line}")
    endforeach()

    # Cross-check: the number of registered test cases must match the number
    # of discovered rows, otherwise fail the build instead of shipping a
    # silently incomplete test matrix.
    execute_process(
        COMMAND "${TEST_EXECUTABLE}" --count --no-colors --no-version --no-intro
        OUTPUT_VARIABLE counted
        ERROR_VARIABLE count_err
        RESULT_VARIABLE count_result
        WORKING_DIRECTORY "${TEST_WORKING_DIR}")

    if(NOT count_result EQUAL 0)
        message(FATAL_ERROR
                "Failed to count test cases of '${TEST_EXECUTABLE}' (exit ${count_result})\n${count_err}")
    endif()

    string(REGEX MATCHALL "[0-9]+" count_numbers "${counted}")
    list(LENGTH count_numbers count_number_count)
    if(count_number_count EQUAL 0)
        message(FATAL_ERROR "Could not read the test case count from:\n${counted}")
    endif()
    list(GET count_numbers -1 expected_count)

    list(LENGTH cases discovered_count)
    if(NOT discovered_count EQUAL expected_count)
        message(FATAL_ERROR
                "Test discovery mismatch for '${TEST_EXECUTABLE}':\n"
                "  discovered ${discovered_count} test case(s) but the binary reports ${expected_count}.\n"
                "Listing was:\n${listing}")
    endif()

    file(TO_CMAKE_PATH "${TEST_EXECUTABLE}" executable)
    file(TO_CMAKE_PATH "${TEST_WORKING_DIR}" working_dir)

    set(content "")
    foreach(case IN LISTS cases)
        # Label = the part of the test case name before the first dot
        # (core / fvm / render / slow) so `ctest -L core` selects a layer.
        string(REGEX REPLACE "\\..*$" "" label "${case}")
        # Escape commas: doctest treats them as filter separators.
        string(REPLACE "," "\\," escaped_case "${case}")
        string(APPEND content
               "add_test(\"${case}\" \"${executable}\" \"--test-case=${escaped_case}\" \"--no-colors\")\n")
        string(APPEND content
               "set_tests_properties(\"${case}\" PROPERTIES WORKING_DIRECTORY \"${working_dir}\" LABELS \"${label}\")\n")
    endforeach()

    file(WRITE "${TEST_OUTPUT}" "${content}")
endif()
