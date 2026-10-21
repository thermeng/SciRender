# Runs a test case that is registered as a known failure.
#
# The CTest row passes while the defect is open (the test fails, or crashes) and
# fails as soon as the test passes - that is the signal to remove the entry
# from the known-failure registry.
#
# CTest's own WILL_FAIL property cannot be used for the whole registry: it
# does not invert a crash, and one of the known defects is an out-of-bounds
# read that takes the test process down.
#
# Required: TEST_EXECUTABLE, TEST_CASE, TEST_WORKING_DIR

execute_process(
    COMMAND "${TEST_EXECUTABLE}" "--test-case=${TEST_CASE}" "--no-colors"
    WORKING_DIRECTORY "${TEST_WORKING_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error)

message("${test_output}")
if(NOT test_error STREQUAL "")
    message("${test_error}")
endif()

if(result EQUAL 0)
    message(FATAL_ERROR
            "'${TEST_CASE}' passed but is registered as a known failure.\n"
            "The solver must have been fixed - remove the entry from the registry.")
endif()
