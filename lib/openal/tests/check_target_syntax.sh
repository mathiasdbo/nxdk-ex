#!/bin/sh
#
# check_target_syntax.sh - type-check lib/openal/src/*.c exactly as nxdk-cc would.
#
# Usage: sh check_target_syntax.sh [NXDK_DIR]
#
#   NXDK_DIR   nxdk checkout to use (default: the repository root this script
#              lives in, i.e. three directories above lib/openal/tests).
#   CLANG      environment variable, clang binary to use (default: clang).
#
# Exit status: 0 = clean, 1 = a diagnostic or a failed check, 2 = bad setup.
#
# Why this exists: the host tests (run_tests.py, the Makefile in this
# directory) build the library for the host, so every code path that is guarded
# for the real Xbox (MmAllocateContiguousMemoryEx, HalReadSMBusValue,
# ExQueryNonVolatileSetting, ...) is NEVER compiled by them. The guards used to
# test __NXDK__ / _XBOX, which bin/nxdk-cc never defines (it only passes
# -DNXDK), so the real code paths were dead on actual builds and nothing
# noticed. This script compiles the sources with the nxdk-cc flag set and the
# real nxdk kernel headers, and also checks that the target-only branches are
# really selected by those flags.
#
# Only -fsyntax-only is used: no nxdk toolchain (linker, pdclib) is required.
# If lib/pdclib/include is missing (the pdclib submodule is not checked out),
# minimal stub libc headers are generated in a temporary directory.

set -u

CLANG=${CLANG:-clang}

script_dir=$(cd "$(dirname "$0")" && pwd) || exit 2
NXDK_DIR=${1:-"$script_dir/../../.."}
NXDK_DIR=$(cd "$NXDK_DIR" 2>/dev/null && pwd) || {
    echo "check_target_syntax: cannot enter NXDK_DIR '${1:-}'" >&2
    exit 2
}
OPENAL_DIR=$NXDK_DIR/lib/openal

if [ ! -d "$OPENAL_DIR/src" ] || [ ! -f "$NXDK_DIR/lib/xboxkrnl/xboxkrnl.h" ]; then
    echo "check_target_syntax: '$NXDK_DIR' does not look like an nxdk checkout with lib/openal" >&2
    exit 2
fi
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "check_target_syntax: '$CLANG' not found (set CLANG=...)" >&2
    exit 2
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/openal-target-syntax.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
trap 'rm -rf "$tmp"; exit 1' HUP INT TERM

# --- libc headers -----------------------------------------------------------
# Same include set as bin/nxdk-cc; pdclib is only used when it is present.
if [ -d "$NXDK_DIR/lib/pdclib/include" ]; then
    libc_note="pdclib headers"
    set -- -isystem "$NXDK_DIR/lib/pdclib/include" \
           "-I$NXDK_DIR/lib/pdclib/platform/xbox/include"
else
    libc_note="generated stub libc headers"
    mkdir "$tmp/stubs" || exit 2
    cat > "$tmp/stubs/assert.h" <<'EOF'
#define assert(x) ((void)0)
EOF
    cat > "$tmp/stubs/string.h" <<'EOF'
#ifndef STUB_STRING_H
#define STUB_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
size_t strlen(const char *); int strcmp(const char *, const char *); int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *); char *strncpy(char *, const char *, size_t); char *strchr(const char *, int);
#endif
EOF
    cat > "$tmp/stubs/stdlib.h" <<'EOF'
#ifndef STUB_STDLIB_H
#define STUB_STDLIB_H
#include <stddef.h>
void *malloc(size_t); void *calloc(size_t, size_t); void *realloc(void *, size_t); void free(void *);
int abs(int); long labs(long); void abort(void); void exit(int); int atoi(const char *);
int rand(void); void srand(unsigned);
#define RAND_MAX 32767
#endif
EOF
    cat > "$tmp/stubs/math.h" <<'EOF'
#ifndef STUB_MATH_H
#define STUB_MATH_H
float sinf(float); float cosf(float); float sqrtf(float); float fabsf(float); float acosf(float); float atan2f(float, float);
float powf(float, float); float roundf(float); float floorf(float); float ceilf(float); float fmodf(float, float); float log10f(float); float log2f(float);
double sin(double); double cos(double); double sqrt(double); double fabs(double); double pow(double, double); double atan2(double, double);
#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define isinf(x) __builtin_isinf(x)
#define M_PI 3.14159265358979323846
#endif
EOF
    cat > "$tmp/stubs/stdio.h" <<'EOF'
#ifndef STUB_STDIO_H
#define STUB_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct _FILE FILE; extern FILE *stdout, *stderr;
int printf(const char *, ...); int fprintf(FILE *, const char *, ...); int snprintf(char *, size_t, const char *, ...);
FILE *fopen(const char *, const char *); int fclose(FILE *); size_t fread(void *, size_t, size_t, FILE *); size_t fwrite(const void *, size_t, size_t, FILE *); int fseek(FILE *, long, int); long ftell(FILE *); int puts(const char *);
#define SEEK_SET 0
#define SEEK_END 2
#endif
EOF
    set -- -isystem "$tmp/stubs"
fi

# --- nxdk-cc flag set (copied from bin/nxdk-cc, minus the linker option) -----
# plus the include dirs lib/openal/Makefile adds to NXDK_CFLAGS.
set -- -target i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin \
    "-I$NXDK_DIR/lib" \
    "-I$NXDK_DIR/lib/xboxrt/libc_extensions" \
    "$@" \
    "-I$NXDK_DIR/lib/winapi" \
    "-I$NXDK_DIR/lib/xboxrt/vcruntime" \
    -Wno-builtin-macro-redefined \
    -DNXDK \
    -D__STDC__=1 \
    -U__STDC_NO_THREADS__ \
    "-I$OPENAL_DIR/include" \
    "-I$OPENAL_DIR/src"

# Warnings are failures. -Wall -Wextra may be overridden, e.g. WARN_FLAGS=-w.
WARN_FLAGS=${WARN_FLAGS:--Wall -Wextra -Werror}

rc=0
echo "check_target_syntax: $("$CLANG" --version | sed -n 1p), $libc_note"

# --- 1. Type-check every source with the real kernel headers ------------------
for f in "$OPENAL_DIR"/src/*.c; do
    # shellcheck disable=SC2086 # WARN_FLAGS is intentionally word-split
    if ! "$CLANG" "$@" $WARN_FLAGS -fsyntax-only "$f" >"$tmp/diag" 2>&1 ||
       grep -Eq 'warning:|error:' "$tmp/diag"; then
        echo "== $(basename "$f")"
        sed -n 1,12p "$tmp/diag"
        rc=1
    fi
done

# --- 2. The target-only branches must really be selected by these flags ------
# Preprocess and look for code that exists only on the Xbox branch (a call into
# the kernel) and for code that exists only on the host branch.
# usage: expect_branch SRC XBOX_ONLY_REGEX HOST_ONLY_REGEX CLANG_FLAGS...
expect_branch() {
    src=$1; present=$2; absent=$3
    shift 3
    if ! "$CLANG" "$@" -E -P "$OPENAL_DIR/src/$src" >"$tmp/pp" 2>"$tmp/diag"; then
        echo "== $src: preprocessing failed"
        sed -n 1,12p "$tmp/diag"
        rc=1
        return
    fi
    if ! grep -q "$present" "$tmp/pp"; then
        echo "== $src: Xbox code path was NOT compiled with -DNXDK (nothing matches '$present')"
        rc=1
    fi
    if [ -n "$absent" ] && grep -q "$absent" "$tmp/pp"; then
        echo "== $src: host fallback was compiled with -DNXDK (found '$absent')"
        rc=1
    fi
}
expect_branch apu_mem.c    '= *MmAllocateContiguousMemoryEx *(' 's_host_raw_pool' "$@"
expect_branch apu_eeprom.c '= *HalReadSMBusValue *('            ''                "$@"
# the raw SMC byte must be decoded before use; returning it raw is the original enum-mismatch bug
expect_branch apu_eeprom.c 'return *apu_av_pack_from_smc *(' ''          "$@"
expect_branch apu_eeprom.c '= *ExQueryNonVolatileSetting *('    ''                "$@"

# --- 3. apu_eeprom.h and the kernel header must coexist in either order -------
# (XC_AUDIO is defined by both; a differing spelling is a macro-redefined
# warning once the Xbox branch is compiled.)
printf '#include "apu_eeprom.h"\n#include <xboxkrnl/xboxkrnl.h>\nint openal_probe_a;\n' >"$tmp/probe_a.c"
printf '#include <xboxkrnl/xboxkrnl.h>\n#include "apu_eeprom.h"\nint openal_probe_b;\n' >"$tmp/probe_b.c"
for p in probe_a probe_b; do
    # shellcheck disable=SC2086
    if ! "$CLANG" "$@" $WARN_FLAGS -fsyntax-only "$tmp/$p.c" >"$tmp/diag" 2>&1 ||
       grep -Eq 'warning:|error:' "$tmp/diag"; then
        echo "== include-order probe ($p): apu_eeprom.h vs xboxkrnl.h"
        sed -n 1,12p "$tmp/diag"
        rc=1
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "target syntax check: clean"
else
    echo "target syntax check: FAILED"
fi
exit "$rc"
