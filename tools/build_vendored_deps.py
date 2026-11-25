#!/usr/bin/env python3
"""Bootstrap the vendored CGNS import stack in one step.

  zlib -> HDF5 1.14.6 -> CGNS 4.5.2, all static, installed into
  build-vendored-lib/install. The app discovers it through CMAKE_PREFIX_PATH;
  presets without the prefix configure fine and CGNS import compiles out.

Requirements on PATH: git, cmake (C:/Qt/Tools/CMake_64/bin), ninja
(C:/Qt/Tools/Ninja), gcc/g++ (C:/Qt/Tools/mingw1310_64/bin).

Usage:  python tools/build_vendored_deps.py [--force] [--skip-cgns]
"""
import argparse
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(REPO, "vendor")
PREFIX = os.path.join(REPO, "build-vendored-lib", "install")
BUILD = os.path.join(REPO, "build-vendored-lib")

ZLIB_SOURCES = [
    "adler32.c", "compress.c", "crc32.c", "deflate.c", "gzclose.c", "gzlib.c",
    "gzread.c", "gzwrite.c", "infback.c", "inffast.c", "inflate.c", "inftrees.c",
    "trees.c", "uncompr.c", "zutil.c",
]


def run(cmd, cwd=None):
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        sys.exit(f"FAILED (exit {r.returncode}): {' '.join(cmd)}")


def cmake_configure(src, bld, defines):
    cmd = ["cmake", "-S", src, "-B", bld, "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++",
           f"-DCMAKE_INSTALL_PREFIX={PREFIX}"]
    cmd += [f"-D{k}={v}" for k, v in defines.items()]
    run(cmd)
    run(["cmake", "--build", bld])
    run(["cmake", "--install", bld])


def build_zlib():
    bld = os.path.join(BUILD, "zlib-build")
    os.makedirs(bld, exist_ok=True)
    src_list = " ".join(f"${{ZLIB_DIR}}/{s}" for s in ZLIB_SOURCES)
    with open(os.path.join(bld, "CMakeLists.txt"), "w") as f:
        f.write(f"""cmake_minimum_required(VERSION 3.18)
project(vendored_zlib C)
set(ZLIB_DIR "{VENDOR.replace(os.sep, '/')}/zlib")
add_library(z STATIC {src_list})
target_include_directories(z PUBLIC ${{ZLIB_DIR}})
set_target_properties(z PROPERTIES OUTPUT_NAME z)
install(TARGETS z ARCHIVE DESTINATION lib)
install(FILES ${{ZLIB_DIR}}/zlib.h ${{ZLIB_DIR}}/zconf.h DESTINATION include)
""")
    run(["cmake", "-S", bld, "-B", bld, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc",
         f"-DCMAKE_INSTALL_PREFIX={PREFIX}"])
    run(["cmake", "--build", bld])
    run(["cmake", "--install", bld])


def ensure_cgns_clones():
    """Fetch the pinned source clones if missing (large vendored trees are
    gitignored, fetched on demand)."""
    clones = [
        ("cgns", "https://github.com/CGNS/CGNS.git", "v4.5.2"),
        ("hdf5", "https://github.com/HDFGroup/hdf5.git", "hdf5-1.14.6"),
    ]
    for name, url, tag in clones:
        dst = os.path.join(VENDOR, name)
        if os.path.exists(os.path.join(dst, "CMakeLists.txt")):
            continue
        print(f"== cloning {name} {tag} ==", flush=True)
        run(["git", "clone", "--depth", "1", "--branch", tag, url, dst])


def build_cgns(force):
    marker = os.path.join(PREFIX, "lib", "libcgns.a")
    if os.path.exists(marker) and not force:
        print(f"CGNS already installed at {PREFIX} (use --force to rebuild)")
        return

    ensure_cgns_clones()
    os.makedirs(PREFIX, exist_ok=True)
    print("== 1/3 zlib (from vendor/zlib) ==", flush=True)
    if os.path.exists(os.path.join(PREFIX, "lib", "libz.a")) and not force:
        print("zlib already installed, skipping")
    else:
        build_zlib()

    print("== 2/3 HDF5 1.14.6 (C, static, deflate via vendored zlib) ==", flush=True)
    if os.path.exists(os.path.join(PREFIX, "lib", "libhdf5.a")) and not force:
        print("HDF5 already installed, skipping")
    else:
        cmake_configure(os.path.join(VENDOR, "hdf5"), os.path.join(BUILD, "hdf5-build"), {
        "BUILD_SHARED_LIBS": "OFF",
        "HDF5_BUILD_STATIC_LIBS": "ON",
        "HDF5_BUILD_TOOLS": "OFF",
        "HDF5_BUILD_EXAMPLES": "OFF",
        "HDF5_BUILD_TESTS": "OFF",
        "HDF5_BUILD_FORTRAN": "OFF",
        "HDF5_BUILD_CPP_LIB": "OFF",
        "HDF5_BUILD_HL_LIB": "OFF",
        "HDF5_ENABLE_PARALLEL": "OFF",
        "HDF5_ENABLE_SZIP_SUPPORT": "OFF",
        "HDF5_ENABLE_Z_LIB_SUPPORT": "ON",
        "ZLIB_USE_EXTERNAL": "OFF",
        "ZLIB_USE_STATIC_LIBS": "ON",
        "ZLIB_ROOT": PREFIX,
        "CMAKE_C_FLAGS": "-Wa,-mbig-obj",
    })

    print("== 3/3 CGNS 4.5.2 (static, HDF5 backend) ==", flush=True)
    cgns_bld = os.path.join(BUILD, "cgns-build")
    os.makedirs(cgns_bld, exist_ok=True)
    # HDF5's exported target links ZLIB::ZLIB but its config package does not
    # find_dependency it; inject the find into the CGNS project scope.
    inject = os.path.join(cgns_bld, "find_zlib.cmake")
    with open(inject, "w") as f:
        f.write("find_package(ZLIB REQUIRED)\n")
    cmake_configure(os.path.join(VENDOR, "cgns"), cgns_bld, {
        "CGNS_BUILD_SHARED": "OFF",
        "CGNS_ENABLE_HDF5": "ON",
        "CGNS_ENABLE_FORTRAN": "OFF",
        "CGNS_ENABLE_TESTS": "OFF",
        "CGNS_BUILD_CGNSTOOLS": "OFF",
        "CGNS_ENABLE_LEGACY": "OFF",
        "CGNS_ENABLE_PARALLEL": "OFF",
        "hdf5_DIR": os.path.join(PREFIX, "cmake"),
        "CMAKE_PREFIX_PATH": PREFIX,
        "CMAKE_PROJECT_INCLUDE": inject,
        "ZLIB_ROOT": PREFIX,
        "CMAKE_C_FLAGS": "-Wa,-mbig-obj",
        # CGNS 4.5.2's CHECK_HDF5_FEATURE try_compile passes HDF5's imported
        # TARGET name (hdf5-static) where a path is required, which fails with
        # HDF5 1.14's config package. Pre-seed the check results instead; the
        # values are correct for the serial HDF5 1.14.6 built above
        # (multi-dataset I/O and file-space strategy exist; collective
        # metadata is MPI-only).
        "CHECK_HDF5_HAVE_MULTI_DATASETS": "1",
        "CHECK_HDF5_HAVE_COLL_METADATA": "0",
        "CHECK_HDF5_HAVE_FILE_SPACE_STRATEGY": "1",
    })
    print(f"CGNS installed at {PREFIX}")


def main():
    ap = argparse.ArgumentParser(
        description="Bootstrap the vendored CGNS import stack")
    ap.add_argument("--force", action="store_true",
                    help="rebuild the CGNS stack even if already installed")
    ap.add_argument("--skip-cgns", action="store_true",
                    help="skip the CGNS stack build")
    args = ap.parse_args()

    if not args.skip_cgns:
        build_cgns(args.force)

    print("\nDONE. Configure with any preset (debug / release / tests) to use")
    print("the prebuilt CGNS, or pass -DCMAKE_PREFIX_PATH=<repo>/build-vendored-lib/install.")


if __name__ == "__main__":
    main()
