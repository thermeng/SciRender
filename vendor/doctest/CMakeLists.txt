# Vendored: doctest v2.4.12 (single-header C++ test framework, MIT)
# https://github.com/doctest/doctest
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(doctest::doctest ALIAS doctest)
