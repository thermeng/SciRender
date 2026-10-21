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
set(SCIRENDER_EXPECT_FAILURE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/expect_failure.cmake"
    CACHE INTERNAL "Path of the known-failure test wrapper")

# scirender_discover_tests(TARGET <tgt> [REGISTRIES <file>...])
#
# REGISTRIES lists "known failure" files: one test-case name per line. A listed
# test case is registered with WILL_FAIL TRUE, so it passes *by failing* while
# the defect is open. The moment the solver is fixed the row turns red, which
# is the signal to remove the entry. A listed name that matches no discovered
# test case fails the build, so the registry cannot silently rot.
function(scirender_discover_tests TARGET)
    cmake_parse_arguments(ARG "" "" "REGISTRIES" "" ${ARGN})

    set(_tests "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_tests.cmake")
    set(_include "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_include.cmake")

    # A dedicated target (not a POST_BUILD step) so the CTest matrix is
    # regenerated on EVERY build: the registry of known failures can change
    # without the test binary changing.
    add_custom_target(${TARGET}_ctest_discovery ALL
        COMMAND ${CMAKE_COMMAND}
                -D "TEST_EXECUTABLE=$<TARGET_FILE:${TARGET}>"
                -D "TEST_OUTPUT=${_tests}"
                -D "TEST_WORKING_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -D "XFAIL_REGISTRIES=${ARG_REGISTRIES}"
                -P "${SCIRENDER_DISCOVER_SCRIPT}"
        DEPENDS ${TARGET}
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

    # ------------------------------------------------------------------------
    # Known-failure registry
    #
    # Each registry file lists test-case names (one per line, '#' comments and
    # blank lines allowed) that are known to fail because the solver deviates
    # from the contract the test states. Those rows are registered with
    # WILL_FAIL TRUE: they pass while the defect is open and turn red as soon
    # as the solver is fixed.
    #
    # A name prefixed with "crash:" describes a defect that takes the test
    # process down. The prefix is stripped here; the wrapper treats a crash
    # like any other failure.
    # ------------------------------------------------------------------------
    set(xfail_names "")
    foreach(registry IN LISTS XFAIL_REGISTRIES)
        if(registry STREQUAL "")
            continue()
        endif()
        if(NOT EXISTS "${registry}")
            message(FATAL_ERROR "Known-failure registry '${registry}' does not exist")
        endif()
        file(STRINGS "${registry}" registry_lines)
        foreach(line IN LISTS registry_lines)
            string(STRIP "${line}" line)
            if(line STREQUAL "")
                continue()
            endif()
            if(line MATCHES "^#")
                continue()
            endif()
            if(line MATCHES "^crash:")
                string(REGEX REPLACE "^crash:" "" line "${line}")
            endif()
            list(APPEND xfail_names "${line}")
        endforeach()
    endforeach()

    # Every registered name must exist, otherwise the registry has gone stale.
    foreach(name IN LISTS xfail_names)
        list(FIND cases "${name}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                    "Known-failure registry lists '${name}', but no such test case exists in "
                    "'${TEST_EXECUTABLE}'.\nRemove the stale entry (or restore the test).")
        endif()
    endforeach()

    set(content "")
    foreach(case IN LISTS cases)
        # Label = the part of the test case name before the first dot
        # (core / fvm / render / slow / cfd) so `ctest -L core` selects a layer.
        string(REGEX REPLACE "\\..*$" "" label "${case}")
        # Escape commas: doctest treats them as filter separators.
        string(REPLACE "," "\\," escaped_case "${case}")

        list(FIND xfail_names "${case}" is_xfail)
        set(extra_props "")
        if(NOT is_xfail EQUAL -1)
            set(extra_props ";known-broken")
        endif()

        if(is_xfail EQUAL -1)
            string(APPEND content
                   "add_test(\"${case}\" \"${executable}\" \"--test-case=${escaped_case}\" \"--no-colors\")\n")
        else()
            # A known failure runs through a wrapper that inverts the outcome:
            # the row passes while the defect is open (a crash counts as a
            # failure too) and turns red as soon as the solver is fixed.
            string(APPEND content
                   "add_test(\"${case}\" \"${CMAKE_COMMAND}\" \"-DTEST_EXECUTABLE=${executable}\" \"-DTEST_CASE=${escaped_case}\" \"-DTEST_WORKING_DIR=${working_dir}\" \"-P\" \"${SCIRENDER_EXPECT_FAILURE_SCRIPT}\")\n")
        endif()
        string(APPEND content
               "set_tests_properties(\"${case}\" PROPERTIES WORKING_DIRECTORY \"${working_dir}\" LABELS \"${label}${extra_props}\")\n")
    endforeach()

    file(WRITE "${TEST_OUTPUT}" "${content}")
endif()
