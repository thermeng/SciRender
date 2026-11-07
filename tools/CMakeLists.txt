# ---------------------------------------------------------------------------
# Developer CLI diagnostics.
#
# These are NOT part of the test suite: they print what a pipeline produces
# for a given dataset instead of asserting. Enable with
#   cmake -S . -B build -DSCIRENDER_BUILD_TOOLS=ON
# ---------------------------------------------------------------------------

add_executable(scirender_tool_cylinder2d cylinder2d_vorticity.cpp)
target_link_libraries(scirender_tool_cylinder2d PRIVATE CoreLib)

add_executable(scirender_tool_bench_streamline bench_streamline.cpp)
target_include_directories(scirender_tool_bench_streamline PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(scirender_tool_bench_streamline PRIVATE RenderCore CoreLib)
