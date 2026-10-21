# ---------------------------------------------------------------------------
# Developer CLI diagnostics.
#
# These are NOT part of the test suite: they print what a pipeline produces
# for a given dataset instead of asserting. Enable with
#   cmake -S . -B build -DSCIRENDER_BUILD_TOOLS=ON
# ---------------------------------------------------------------------------

add_executable(scirender_tool_cylinder2d cylinder2d_vorticity.cpp)
target_link_libraries(scirender_tool_cylinder2d PRIVATE CoreLib)
