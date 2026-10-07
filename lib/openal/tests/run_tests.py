#!/usr/bin/env python3
"""
Master Regression Test Runner for nxdk-ex OpenAL MCPX APU Subsystem

Compiles lib/openal/src/*.c into a static library and then builds and runs
every lib/openal/tests/test_*.c host unit test against it. Tests are
discovered by globbing, so a new test_*.c file is picked up automatically
(tests/Makefile derives its test list from the same files).

All build products (objects, libopenal.a, test executables) are written to a
temporary directory that is removed on exit; nothing is left in the source
tree.
"""

import os
import sys
import glob
import shutil
import tempfile
import subprocess

# Upper bound for a single test run, so a hanging test fails instead of
# blocking the runner forever.
TEST_TIMEOUT_SECONDS = 180

def find_llvm_tools():
    clang = shutil.which("clang")
    llvm_ar = shutil.which("llvm-ar")

    if not clang:
        default_llvm_clang = r"C:\Program Files\LLVM\bin\clang.exe"
        if os.path.exists(default_llvm_clang):
            clang = default_llvm_clang

    if not llvm_ar:
        default_llvm_ar = r"C:\Program Files\LLVM\bin\llvm-ar.exe"
        if os.path.exists(default_llvm_ar):
            llvm_ar = default_llvm_ar

    if not clang or not llvm_ar:
        print("[ERROR] Clang or llvm-ar not found in PATH or standard installation directory.")
        sys.exit(1)

    return clang, llvm_ar

def math_link_flags():
    # The tests call sinf/cosf/powf/...; on Linux and macOS libm has to be
    # linked explicitly, whereas the Windows (MSVC ABI) clang provides it.
    if sys.platform.startswith("win"):
        return []
    return ["-lm"]

def build_static_library(clang, llvm_ar, openal_dir, build_dir):
    src_dir = os.path.join(openal_dir, "src")
    inc_dir = os.path.join(openal_dir, "include")
    lib_path = os.path.join(build_dir, "libopenal.a")

    src_files = sorted(glob.glob(os.path.join(src_dir, "*.c")))
    if not src_files:
        print(f"[ERROR] No library sources found in {src_dir}")
        sys.exit(1)

    print("================================================================================")
    print(" Compiling OpenAL Static Library (libopenal.a)...")
    print("================================================================================")

    objs = []
    for c_path in src_files:
        src = os.path.basename(c_path)
        obj_path = os.path.join(build_dir, src.replace(".c", ".obj"))
        cmd = [
            clang, "-c",
            "-I", inc_dir,
            "-I", src_dir,
            "-Wall", "-Werror", "-pedantic", "-std=c99",
            c_path, "-o", obj_path
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[FAIL] Error compiling {src}:\n{res.stderr}")
            sys.exit(1)
        objs.append(obj_path)
        print(f"  [CC]  {src} -> {os.path.basename(obj_path)}")

    ar_cmd = [llvm_ar, "rcs", lib_path] + objs
    res = subprocess.run(ar_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Error archiving libopenal.a:\n{res.stderr}")
        sys.exit(1)

    print(f"  [AR]  Successfully created {os.path.basename(lib_path)} ({len(objs)} objects)\n")
    return lib_path

def run_all_tests(clang, openal_dir, lib_path, build_dir):
    src_dir = os.path.join(openal_dir, "src")
    inc_dir = os.path.join(openal_dir, "include")
    tests_dir = os.path.join(openal_dir, "tests")

    test_files = sorted(glob.glob(os.path.join(tests_dir, "test_*.c")))
    if not test_files:
        print(f"[ERROR] No test_*.c files found in {tests_dir}")
        sys.exit(1)
    temp_exe = os.path.join(build_dir, "nxdk_al_test_runner.exe")

    print("================================================================================")
    print(f" Running {len(test_files)} Unit Tests against libopenal.a...")
    print("================================================================================")

    passed_count = 0
    failed_count = 0

    for idx, tf in enumerate(test_files, 1):
        tname = os.path.basename(tf)
        showcase_dir = os.path.abspath(os.path.join(openal_dir, "..", "..", "samples", "openal_showcase"))
        compile_cmd = [
            clang,
            "-I", inc_dir,
            "-I", src_dir,
            "-I", showcase_dir,
            "-Wall", "-Werror", "-pedantic", "-std=c99",
            tf
        ]
        if "test_wav_loader.c" in tname:
            compile_cmd.append(os.path.join(showcase_dir, "wav_loader.c"))
        elif "test_scene_engine.c" in tname:
            compile_cmd.extend([
                os.path.join(showcase_dir, "showcase_input.c"),
                os.path.join(showcase_dir, "showcase_ui.c")
            ])
        elif "test_interactive_modes.c" in tname or "test_showcase_qa.c" in tname:
            compile_cmd.extend([
                os.path.join(showcase_dir, "wav_loader.c"),
                os.path.join(showcase_dir, "showcase_input.c"),
                os.path.join(showcase_dir, "showcase_ui.c"),
                os.path.join(showcase_dir, "showcase_modes.c")
            ])
        compile_cmd.extend([lib_path] + math_link_flags() + ["-o", temp_exe])
        compile_res = subprocess.run(compile_cmd, capture_output=True, text=True)
        if compile_res.returncode != 0:
            print(f"[{idx:02d}/{len(test_files):02d}] [FAIL - COMPILE] {tname}")
            print(f"       Details: {compile_res.stderr.strip()[:200]}")
            failed_count += 1
            continue

        # cwd is tests/ to match "make test" (the Makefile runs every test from
        # there). No test currently opens or writes a file, so this is not
        # load-bearing today; it only keeps the two runners equivalent.
        try:
            run_res = subprocess.run([temp_exe], capture_output=True, text=True,
                                     cwd=tests_dir, timeout=TEST_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            print(f"[{idx:02d}/{len(test_files):02d}] [FAIL - TIMEOUT] {tname} (> {TEST_TIMEOUT_SECONDS}s)")
            failed_count += 1
            continue
        if run_res.returncode != 0:
            print(f"[{idx:02d}/{len(test_files):02d}] [FAIL - RUNTIME] {tname}")
            print(f"       Details: {(run_res.stderr or run_res.stdout).strip()[:200]}")
            failed_count += 1
            continue

        print(f"[{idx:02d}/{len(test_files):02d}] [PASS] {tname}")
        passed_count += 1

    print("================================================================================")
    print(f" SUMMARY: {passed_count}/{len(test_files)} PASSED | {failed_count} FAILED")
    print("================================================================================")

    if failed_count > 0:
        sys.exit(1)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    openal_dir = os.path.abspath(os.path.join(script_dir, ".."))

    clang, llvm_ar = find_llvm_tools()
    with tempfile.TemporaryDirectory(prefix="nxdk_al_tests_") as build_dir:
        lib_path = build_static_library(clang, llvm_ar, openal_dir, build_dir)
        run_all_tests(clang, openal_dir, lib_path, build_dir)

if __name__ == "__main__":
    main()
