#ifndef APU_PLATFORM_H
#define APU_PLATFORM_H

/*
 * ============================================================================
 * Build target selection
 * ============================================================================
 * OPENAL_TARGET_XBOX is defined when the library is built for the real Xbox
 * kernel (MmAllocateContiguousMemoryEx, HalReadSMBusValue, ...). Otherwise the
 * host fallbacks (malloc pool, no SMBus) are used.
 *
 * bin/nxdk-cc passes only -DNXDK; __NXDK__ and _XBOX are never defined by the
 * nxdk toolchain, so guarding on them alone silently selects the host paths on
 * real builds. All of them are accepted here so that every target-only code
 * path keys off this single macro.
 * ============================================================================
 */
#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#define OPENAL_TARGET_XBOX 1
#endif

#include <stdint.h>

/* CPU time-stamp counter (the console's CPU clock); 0 where there is none */
static inline uint64_t apu_platform_cycles(void) {
#if defined(__i386__) || defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

#endif /* APU_PLATFORM_H */
