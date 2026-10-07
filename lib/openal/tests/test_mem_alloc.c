/*
 * Contiguous-memory allocator regression test (apu_mem_alloc_phys / apu_mem_free).
 *
 * Covers the aligned-allocation heap corruption: the leading-split path used to
 * write a chunk header at an address derived from the aligned user pointer
 * without checking that the aligned block fits inside the free chunk, so for
 * alignments >= 64 and a small free chunk it overwrote the next chunk (and could
 * leave a zero-size self-linked chunk that hangs the free-list walk).
 *
 * Only the public apu_mem.h API is used. The run is fully deterministic (fixed
 * PRNG seed) and finishes in a few seconds. A watchdog turns an allocator hang
 * into a test failure instead of a stuck CI job.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "apu_mem.h"

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <unistd.h>
#define HAVE_WATCHDOG 1
#endif

/* Not assert(): must keep working if the suite is built with -DNDEBUG */
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#ifdef HAVE_WATCHDOG
static void watchdog(int sig) {
    static const char msg[] = "FAIL: watchdog expired (allocator hang?)\n";
    (void)sig;
    if (write(2, msg, sizeof(msg) - 1) < 0) {
        /* nothing useful to do */
    }
    _exit(2);
}
#endif

#define FUZZ_OPS        20000
#define MAX_LIVE        48
#define POOL_SIZE       APU_MEM_DEFAULT_POOL_SIZE

typedef struct {
    uint8_t *p;
    size_t   size;
    size_t   align;     /* requested alignment (0 = default) */
    uint32_t phys;
    uint32_t seed;
    int      used;
} slot_t;

static uint32_t s_rng = 0x1BADB002u;

static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint8_t pat(uint32_t seed, size_t i) {
    return (uint8_t)((seed ^ (seed >> 8) ^ (seed >> 16)) + i * 31u + (i >> 9));
}

static size_t eff_align(size_t a) {
    return a < 8 ? 8 : a;
}

static void check_zero(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            fprintf(stderr, "FAIL: byte %lu of fresh block is 0x%02X, not zero\n",
                    (unsigned long)i, (unsigned)p[i]);
            exit(1);
        }
    }
}

static void check_pattern(const slot_t *s) {
    size_t i;
    for (i = 0; i < s->size; i++) {
        if (s->p[i] != pat(s->seed, i)) {
            fprintf(stderr, "FAIL: canary broken at byte %lu of %lu-byte block (align %lu)\n",
                    (unsigned long)i, (unsigned long)s->size, (unsigned long)s->align);
            exit(1);
        }
    }
}

static int overlaps(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    return a < b + bn && b < a + an;
}

/* Allocate, validate alignment/zero-fill/phys, check overlap against live set, stamp a canary. */
static uint8_t *checked_alloc(slot_t *live, size_t nlive, size_t size, size_t align,
                              uint32_t *out_phys) {
    uint32_t phys = 0xDEADBEEFu;
    uint8_t *p = (uint8_t *)apu_mem_alloc_phys(size, align, &phys);
    size_t a = eff_align(align);
    size_t i;

    CHECK(p != NULL);
    CHECK(((uintptr_t)p & (a - 1)) == 0);
    CHECK((phys & (uint32_t)(a - 1)) == 0);
    CHECK(phys == apu_mem_get_physical_address(p));
    check_zero(p, size);
    for (i = 0; i < nlive; i++) {
        if (live[i].used) {
            CHECK(!overlaps(p, size, live[i].p, live[i].size));
        }
    }
    *out_phys = phys;
    return p;
}

static void checked_free(slot_t *s) {
    size_t i;
    check_pattern(s);
    /* Poison so a later allocation reusing this space must really zero it */
    for (i = 0; i < s->size; i++) {
        s->p[i] = 0xDD;
    }
    apu_mem_free(s->p);
    s->used = 0;
}

static void stamp(slot_t *s, uint32_t seed) {
    size_t i;
    s->seed = seed;
    for (i = 0; i < s->size; i++) {
        s->p[i] = pat(seed, i);
    }
}

/*
 * Directed scenario for the verified bug: a small free chunk in front of a live
 * block, then a 128/4096-byte aligned request whose aligned address lies beyond
 * the small chunk. With the old code the leading split wrote a header over the
 * live block's header. (Other layouts that trigger it are left to the fuzz.)
 */
static void test_small_free_chunk_then_aligned(size_t align) {
    slot_t live[3];
    uint32_t phys;
    size_t pool = 64u * 1024u;

    memset(live, 0, sizeof(live));
    apu_mem_shutdown();
    CHECK(apu_mem_init(pool) == 0);

    live[0].size = 64;
    live[0].p = checked_alloc(live, 0, live[0].size, 8, &phys);
    live[0].used = 1;
    stamp(&live[0], 11);

    live[1].size = 4000;
    live[1].p = checked_alloc(live, 1, live[1].size, 8, &phys);
    live[1].used = 1;
    stamp(&live[1], 22);

    checked_free(&live[0]);                 /* small free chunk, followed by live block 1 */

    live[2].size = 16;
    live[2].align = align;
    live[2].p = checked_alloc(live, 3, live[2].size, align, &phys);
    live[2].used = 1;
    stamp(&live[2], 33);

    check_pattern(&live[1]);                /* neighbouring live block untouched */
    checked_free(&live[1]);
    checked_free(&live[2]);

    /* Pool must have coalesced back into one chunk: nearly the whole pool is allocatable */
    {
        uint8_t *big = (uint8_t *)apu_mem_alloc_phys(pool - 8192u, 4096, &phys);
        CHECK(big != NULL);
        CHECK(((uintptr_t)big & 4095u) == 0);
        apu_mem_free(big);
    }
    apu_mem_shutdown();
}

int main(void) {
    static const size_t aligns[] = { 0, 8, 64, 128, 4096 };
    static slot_t live[MAX_LIVE];
    unsigned long n_alloc[5] = { 0, 0, 0, 0, 0 };
    unsigned long n_pool = 0, n_direct = 0, n_free = 0, max_live = 0;
    uintptr_t pool_lo, pool_hi;
    size_t nlive = 0;
    size_t i;
    int op;

#ifdef HAVE_WATCHDOG
    signal(SIGALRM, watchdog);
    alarm(60);
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== APU Contiguous Memory Allocator Regression Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Argument validation
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing allocator argument validation...\n");
    {
        uint32_t phys = 0x12345678u;
        apu_mem_shutdown();
        CHECK(apu_mem_alloc_phys(0, 8, &phys) == NULL);
        CHECK(phys == 0);
        phys = 0x12345678u;
        CHECK(apu_mem_alloc_phys(64, 12, &phys) == NULL);     /* not a power of 2 */
        CHECK(phys == 0);
        phys = 0x12345678u;
        CHECK(apu_mem_alloc_phys(64, 24, &phys) == NULL);
        CHECK(phys == 0);
        CHECK(apu_mem_alloc_phys(64, 4097, NULL) == NULL);    /* out_phys is optional */
        /* Sizes that overflow internal arithmetic are refused, never wrapped */
        phys = 0x12345678u;
        CHECK(apu_mem_alloc_phys((size_t)-1, 0, &phys) == NULL);
        CHECK(phys == 0);
        CHECK(apu_mem_alloc_phys((size_t)-1 - 4096u, 8192, NULL) == NULL);
        apu_mem_free(NULL);                                   /* must be a no-op */
    }
    printf("    -> Bad sizes and alignments rejected [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: Directed repro (small free chunk followed by a live block)
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing aligned allocation next to a small free chunk...\n");
    test_small_free_chunk_then_aligned(128);
    test_small_free_chunk_then_aligned(4096);
    printf("    -> Live neighbours survive aligned allocations (128/4096) [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: Deterministic fuzz
     * ------------------------------------------------------------------------
     */
    printf("[3] Fuzzing allocator (%d ops, alignments 0/8/64/128/4096)...\n", FUZZ_OPS);
    apu_mem_shutdown();
    CHECK(apu_mem_init(0) == 0);

    /*
     * Derive the pool's virtual window from a probe block: the first block of a
     * fresh pool sits a few dozen bytes past the 4096-aligned pool base, and
     * virtual and physical offsets into the contiguous pool are identical.
     */
    {
        uint32_t phys = 0;
        void *probe = apu_mem_alloc_phys(8, 0, &phys);
        CHECK(probe != NULL);
        pool_lo = (uintptr_t)probe - (uintptr_t)(phys & 4095u);
        pool_hi = pool_lo + POOL_SIZE;
        CHECK((pool_lo & 4095u) == 0);
        apu_mem_free(probe);
    }

    memset(live, 0, sizeof(live));
    for (op = 0; op < FUZZ_OPS; op++) {
        int do_alloc = (nlive == 0) || (nlive < MAX_LIVE && (rnd() % 100u) < 55u);

        if (do_alloc) {
            size_t size, align, k;
            uint32_t r = rnd() % 1000u;
            uint32_t phys;
            slot_t *s = NULL;

            if (r < 500u)       size = 1u + rnd() % 256u;
            else if (r < 800u)  size = 257u + rnd() % 7936u;
            else if (r < 985u)  size = 8193u + rnd() % 61808u;       /* up to ~70000 */
            else if (r < 992u)  size = 1024u * 1024u + rnd() % (900u * 1024u); /* may or may not fit */
            else                size = POOL_SIZE + 1u + rnd() % (1024u * 1024u);   /* never fits the pool */
            k = rnd() % 5u;
            align = aligns[k];

            for (i = 0; i < MAX_LIVE; i++) {
                if (!live[i].used) {
                    s = &live[i];
                    break;
                }
            }
            CHECK(s != NULL);

            s->size = size;
            s->align = align;
            s->p = checked_alloc(live, MAX_LIVE, size, align, &phys);
            s->phys = phys;
            s->used = 1;
            stamp(s, (uint32_t)op * 2654435761u + 1u);
            nlive++;
            n_alloc[k]++;
            if ((uintptr_t)s->p >= pool_lo && (uintptr_t)s->p + size <= pool_hi) {
                n_pool++;
            } else {
                n_direct++;
            }
            if (nlive > max_live) {
                max_live = nlive;
            }
        } else {
            size_t pick = rnd() % MAX_LIVE;
            size_t j;
            for (j = 0; j < MAX_LIVE; j++) {
                slot_t *s = &live[(pick + j) % MAX_LIVE];
                if (s->used) {
                    checked_free(s);
                    nlive--;
                    n_free++;
                    break;
                }
            }
        }
    }

    /* Verify every survivor, then release everything */
    for (i = 0; i < MAX_LIVE; i++) {
        if (live[i].used) {
            checked_free(&live[i]);
            nlive--;
            n_free++;
        }
    }
    CHECK(nlive == 0);
    printf("    -> %lu allocs (align 0/8/64/128/4096: %lu/%lu/%lu/%lu/%lu), %lu from pool, %lu direct, %lu frees, max live %lu [PASS]\n",
           n_pool + n_direct, n_alloc[0], n_alloc[1], n_alloc[2], n_alloc[3], n_alloc[4],
           n_pool, n_direct, n_free, max_live);
    CHECK(n_pool > 0 && n_direct > 0);

    /*
     * ------------------------------------------------------------------------
     * Test 4: Coalescing after the fuzz
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing pool coalescing after freeing everything...\n");
    {
        uint32_t phys = 0;
        size_t big_size = 1900u * 1024u;
        uint8_t *big = (uint8_t *)apu_mem_alloc_phys(big_size, 4096, &phys);

        CHECK(big != NULL);
        CHECK(((uintptr_t)big & 4095u) == 0);
        CHECK((phys & 4095u) == 0);
        /* Must come out of the pool itself, not the direct-allocation fallback */
        CHECK((uintptr_t)big >= pool_lo && (uintptr_t)big + big_size <= pool_hi);
        check_zero(big, big_size);
        memset(big, 0xA5, big_size);
        apu_mem_free(big);

        /* ...and again: freeing it must have restored the single free chunk */
        big = (uint8_t *)apu_mem_alloc_phys(big_size, 4096, &phys);
        CHECK(big != NULL);
        CHECK((uintptr_t)big >= pool_lo && (uintptr_t)big + big_size <= pool_hi);
        apu_mem_free(big);
    }
    printf("    -> 1.9 MiB 4096-aligned block allocatable from the pool again [PASS]\n");

    apu_mem_shutdown();

    printf("=== All Allocator Regression Tests Passed Successfully! ===\n");
    return 0;
}
