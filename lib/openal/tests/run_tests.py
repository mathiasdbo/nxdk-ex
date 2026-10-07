#!/usr/bin/env python3
"""
Master Regression Test Runner for nxdk-ex OpenAL MCPX APU Subsystem
Compiles libopenal.a and executes all 17 host unit tests.
"""

import os
import sys
import glob
import shutil
import subprocess

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

def build_static_library(clang, llvm_ar, openal_dir):
    src_dir = os.path.join(openal_dir, "src")
    inc_dir = os.path.join(openal_dir, "include")
    lib_path = os.path.join(openal_dir, "libopenal.a")

    src_files = [
        "apu_mem.c",
        "apu_voice.c",
        "apu_spatial.c",
        "apu_eeprom.c",
        "apu_ep.c",
        "apu_voice_mgr.c",
        "al_buffer.c",
        "al_listener.c",
        "al_source.c",
        "alc_context.c"
    ]

    print("================================================================================")
    print(" Compiling OpenAL Static Library (libopenal.a)...")
    print("================================================================================")

    objs = []
    for src in src_files:
        c_path = os.path.join(src_dir, src)
        obj_path = os.path.join(src_dir, src.replace(".c", ".obj"))
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

    print(f"  [AR]  Successfully created {lib_path}\n")
    return lib_path

def run_all_tests(clang, openal_dir, lib_path):
    src_dir = os.path.join(openal_dir, "src")
    inc_dir = os.path.join(openal_dir, "include")
    tests_dir = os.path.join(openal_dir, "tests")

    test_files = sorted(glob.glob(os.path.join(tests_dir, "test_*.c")))
    temp_dir = os.environ.get("TEMP", ".")
    temp_exe = os.path.join(temp_dir, "nxdk_al_test_runner.exe")

    print("================================================================================")
    print(f" Running {len(test_files)} Unit Tests against libopenal.a...")
    print("================================================================================")

    passed_count = 0
    failed_count = 0

    for idx, tf in enumerate(test_files, 1):
        tname = os.path.basename(tf)
        compile_cmd = [
            clang,
            "-I", inc_dir,
            "-I", src_dir,
            "-Wall", "-Werror", "-pedantic", "-std=c99",
            tf, lib_path,
            "-o", temp_exe
        ]
        compile_res = subprocess.run(compile_cmd, capture_output=True, text=True)
        if compile_res.returncode != 0:
            print(f"[{idx:02d}/{len(test_files):02d}] [FAIL - COMPILE] {tname}")
            print(f"       Details: {compile_res.stderr.strip()[:200]}")
            failed_count += 1
            continue

        run_res = subprocess.run([temp_exe], capture_output=True, text=True)
        if run_res.returncode != 0:
            print(f"[{idx:02d}/{len(test_files):02d}] [FAIL - RUNTIME] {tname}")
            print(f"       Details: {(run_res.stderr or run_res.stdout).strip()[:200]}")
            failed_count += 1
            continue

        print(f"[{idx:02d}/{len(test_files):02d}] [PASS] {tname}")
        passed_count += 1

    if os.path.exists(temp_exe):
        try:
            os.remove(temp_exe)
        except OSError:
            pass

    print("================================================================================")
    print(f" SUMMARY: {passed_count}/{len(test_files)} PASSED | {failed_count} FAILED")
    print("================================================================================")

    if failed_count > 0:
        sys.exit(1)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    openal_dir = os.path.abspath(os.path.join(script_dir, ".."))

    clang, llvm_ar = find_llvm_tools()
    lib_path = build_static_library(clang, llvm_ar, openal_dir)
    run_all_tests(clang, openal_dir, lib_path)

if __name__ == "__main__":
    main()
