#include "apu_mem.h"
#include "apu_platform.h"
#include <string.h>
#include <stdlib.h>

#ifdef OPENAL_TARGET_XBOX
#include <xboxkrnl/xboxkrnl.h>
#ifndef NVAPU_MAXRAM
#define NVAPU_MAXRAM 0x03FFAFFF
#endif
#else
/* Host testing / static analysis fallback */
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#endif
#ifndef PAGE_WRITECOMBINE
#define PAGE_WRITECOMBINE 0x400
#endif
#ifndef NVAPU_MAXRAM
#define NVAPU_MAXRAM 0x03FFAFFF
#endif
#endif

#define APU_CHUNK_MAGIC 0x4150554Du /* 'APUM' */

/**
 * Internal metadata header for contiguous pool chunks.
 * Embedded immediately preceding memory allocations within the pool.
 */
typedef struct apu_chunk {
    uint32_t magic;                 /**< Validation tag (APU_CHUNK_MAGIC) */
    uint32_t is_free;               /**< 1 if chunk is free, 0 if allocated */
    size_t   total_size;            /**< Total chunk size in bytes (header + payload + padding) */
    size_t   payload_size;          /**< Size requested by caller */
    struct apu_chunk *next;         /**< Next chunk in pool */
    struct apu_chunk *prev;         /**< Previous chunk in pool */
} apu_chunk_t;

static void    *s_pool_virt = NULL;
static uint32_t s_pool_phys = 0;
static size_t   s_pool_size = 0;

#ifndef OPENAL_TARGET_XBOX
static void    *s_host_raw_pool = NULL;

/*
 * Host fallback for blocks that do not fit the pool. malloc() does not honour
 * alignments above its own, so over-allocate and keep the raw pointer just
 * below the aligned block for apu_mem_free(). alignment is a power of 2 >= 8.
 */
static void *host_direct_alloc(size_t size, size_t alignment) {
    size_t extra = alignment + sizeof(void *);
    uint8_t *raw;
    uintptr_t user;

    if (size > SIZE_MAX - extra) {
        return NULL;
    }
    raw = (uint8_t *)malloc(size + extra);
    if (!raw) {
        return NULL;
    }
    user = ((uintptr_t)raw + sizeof(void *) + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
    ((void **)user)[-1] = raw;
    return (void *)user;
}
#endif

int apu_mem_init(size_t pool_size) {
    apu_chunk_t *first;

    if (s_pool_virt != NULL) {
        if (pool_size == 0 || pool_size <= s_pool_size) {
            return 0;
        }
        apu_mem_shutdown();
    }

    if (pool_size == 0) {
        pool_size = APU_MEM_DEFAULT_POOL_SIZE;
    }

    /* Page-align pool size (4096 bytes) */
    pool_size = (pool_size + 4095u) & ~(size_t)4095u;

#ifdef OPENAL_TARGET_XBOX
    s_pool_virt = MmAllocateContiguousMemoryEx(
        pool_size,
        0,
        NVAPU_MAXRAM,
        4096,
        PAGE_READWRITE | PAGE_WRITECOMBINE
    );
    if (!s_pool_virt) {
        return -1;
    }
    s_pool_phys = (uint32_t)MmGetPhysicalAddress(s_pool_virt);
#else
    /* Host fallback: allocate with 4096 alignment for testing */
    s_host_raw_pool = malloc(pool_size + 4096);
    if (!s_host_raw_pool) {
        return -1;
    }
    s_pool_virt = (void *)(((uintptr_t)s_host_raw_pool + 4095u) & ~(uintptr_t)4095u);
    s_pool_phys = 0x01000000u;
#endif

    s_pool_size = pool_size;

    /* Initialize the single master free chunk */
    first = (apu_chunk_t *)s_pool_virt;
    first->magic = APU_CHUNK_MAGIC;
    first->is_free = 1;
    first->total_size = pool_size;
    first->payload_size = 0;
    first->next = NULL;
    first->prev = NULL;

    return 0;
}

void apu_mem_shutdown(void) {
    if (!s_pool_virt) {
        return;
    }

#ifdef OPENAL_TARGET_XBOX
    MmFreeContiguousMemory(s_pool_virt);
#else
    if (s_host_raw_pool) {
        free(s_host_raw_pool);
        s_host_raw_pool = NULL;
    }
#endif

    s_pool_virt = NULL;
    s_pool_phys = 0;
    s_pool_size = 0;
}

uint32_t apu_mem_get_physical_address(const void *ptr) {
    if (!ptr) {
        return 0;
    }

#ifdef OPENAL_TARGET_XBOX
    return (uint32_t)MmGetPhysicalAddress((void *)ptr);
#else
    if (s_pool_virt &&
        (const uint8_t *)ptr >= (const uint8_t *)s_pool_virt &&
        (const uint8_t *)ptr < (const uint8_t *)s_pool_virt + s_pool_size) {
        return s_pool_phys + (uint32_t)((const uint8_t *)ptr - (const uint8_t *)s_pool_virt);
    }
    return (uint32_t)(uintptr_t)ptr;
#endif
}

void *apu_mem_alloc_phys(size_t size, size_t alignment, uint32_t *out_phys) {
    apu_chunk_t *curr;
    size_t min_split_size;

    if (size == 0) {
        if (out_phys) {
            *out_phys = 0;
        }
        return NULL;
    }

    /* Default alignment to 8 bytes if unspecified */
    if (alignment < 8) {
        alignment = 8;
    }

    /* Verify alignment is a power of 2 */
    if ((alignment & (alignment - 1)) != 0) {
        if (out_phys) {
            *out_phys = 0;
        }
        return NULL;
    }

    /* Lazy-init pool if not initialized */
    if (!s_pool_virt) {
        if (apu_mem_init(0) != 0) {
            if (out_phys) {
                *out_phys = 0;
            }
            return NULL;
        }
    }

    min_split_size = sizeof(apu_chunk_t) + sizeof(apu_chunk_t *) + 8;
    curr = (apu_chunk_t *)s_pool_virt;

    while (curr) {
        if (curr->magic != APU_CHUNK_MAGIC) {
            /* Pool corruption detected */
            break;
        }

        if (curr->is_free) {
            uintptr_t header_end = (uintptr_t)curr + sizeof(apu_chunk_t);
            uintptr_t min_user = header_end + sizeof(apu_chunk_t *);
            uintptr_t cand_user = (min_user + (alignment - 1)) & ~(alignment - 1);
            uintptr_t chunk_end = (uintptr_t)curr + curr->total_size;

            /*
             * The aligned block must lie entirely inside this free chunk. Decide
             * that before writing any header: the leading split below only moves
             * the chunk header (cand_user is unchanged), so a block that does not
             * fit here must never touch the following chunk.
             */
            if (cand_user < min_user || cand_user > chunk_end || size > chunk_end - cand_user) {
                curr = curr->next;
                continue;
            }

            /* Split leading free chunk if large alignment offset introduces sufficient slack */
            uintptr_t cand_chunk_addr = (cand_user - sizeof(apu_chunk_t) - sizeof(apu_chunk_t *)) & ~((uintptr_t)7u);
            if (cand_chunk_addr >= (uintptr_t)curr + min_split_size) {
                apu_chunk_t *chunk_alloc = (apu_chunk_t *)cand_chunk_addr;
                size_t leading_size = cand_chunk_addr - (uintptr_t)curr;

                chunk_alloc->magic = APU_CHUNK_MAGIC;
                chunk_alloc->is_free = 1;
                chunk_alloc->total_size = curr->total_size - leading_size;
                chunk_alloc->payload_size = 0;
                chunk_alloc->prev = curr;
                chunk_alloc->next = curr->next;
                if (curr->next) {
                    curr->next->prev = chunk_alloc;
                }
                curr->next = chunk_alloc;
                curr->total_size = leading_size;

                curr = chunk_alloc;
                header_end = (uintptr_t)curr + sizeof(apu_chunk_t);
                min_user = header_end + sizeof(apu_chunk_t *);
                cand_user = (min_user + (alignment - 1)) & ~(alignment - 1);
            }

            {
                size_t user_offset = (size_t)(cand_user - (uintptr_t)curr);
                size_t needed = (user_offset + size + 7u) & ~(size_t)7u;

                if (curr->total_size >= needed) {
                    void *user_ptr;

                    /* Split trailing free chunk if sufficient space remains */
                    if (curr->total_size >= needed + min_split_size) {
                        apu_chunk_t *tail = (apu_chunk_t *)((uint8_t *)curr + needed);
                        tail->magic = APU_CHUNK_MAGIC;
                        tail->is_free = 1;
                        tail->total_size = curr->total_size - needed;
                        tail->payload_size = 0;
                        tail->prev = curr;
                        tail->next = curr->next;
                        if (curr->next) {
                            curr->next->prev = tail;
                        }
                        curr->next = tail;
                        curr->total_size = needed;
                    }

                    curr->is_free = 0;
                    curr->payload_size = size;

                    user_ptr = (void *)cand_user;
                    ((apu_chunk_t **)user_ptr)[-1] = curr;

                    memset(user_ptr, 0, size);

                    if (out_phys) {
                        *out_phys = apu_mem_get_physical_address(user_ptr);
                    }
                    return user_ptr;
                }
            }
        }
        curr = curr->next;
    }

    /* Fallback to direct contiguous kernel allocation if pool is exhausted or insufficient */
#ifdef OPENAL_TARGET_XBOX
    {
        void *direct = MmAllocateContiguousMemoryEx(
            size,
            0,
            NVAPU_MAXRAM,
            alignment,
            PAGE_READWRITE | PAGE_WRITECOMBINE
        );
        if (direct) {
            memset(direct, 0, size);
            if (out_phys) {
                *out_phys = (uint32_t)MmGetPhysicalAddress(direct);
            }
        } else {
            if (out_phys) {
                *out_phys = 0;
            }
        }
        return direct;
    }
#else
    {
        void *direct = host_direct_alloc(size, alignment);
        if (direct) {
            memset(direct, 0, size);
            if (out_phys) {
                *out_phys = (uint32_t)(uintptr_t)direct;
            }
        } else {
            if (out_phys) {
                *out_phys = 0;
            }
        }
        return direct;
    }
#endif
}

void *apu_mem_alloc_table(size_t size, uint32_t *out_phys) {
#ifdef OPENAL_TARGET_XBOX
    void *p = MmAllocateContiguousMemoryEx(size, 0, NVAPU_MAXRAM, 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    if (p) {
        memset(p, 0, size);
    }
    if (out_phys) {
        *out_phys = p ? (uint32_t)MmGetPhysicalAddress(p) : 0;
    }
    return p;
#else
    return apu_mem_alloc_phys(size, 0x4000, out_phys);
#endif
}

void apu_mem_free(void *ptr) {
    if (!ptr) {
        return;
    }

    if (s_pool_virt &&
        (uint8_t *)ptr >= (uint8_t *)s_pool_virt &&
        (uint8_t *)ptr < (uint8_t *)s_pool_virt + s_pool_size) {

        apu_chunk_t *chunk = ((apu_chunk_t **)ptr)[-1];
        if (chunk && chunk->magic == APU_CHUNK_MAGIC && !chunk->is_free) {
            chunk->is_free = 1;
            chunk->payload_size = 0;

            /* Coalesce with next chunk if free */
            if (chunk->next && chunk->next->is_free && chunk->next->magic == APU_CHUNK_MAGIC) {
                chunk->total_size += chunk->next->total_size;
                chunk->next = chunk->next->next;
                if (chunk->next) {
                    chunk->next->prev = chunk;
                }
            }

            /* Coalesce with previous chunk if free */
            if (chunk->prev && chunk->prev->is_free && chunk->prev->magic == APU_CHUNK_MAGIC) {
                chunk->prev->total_size += chunk->total_size;
                chunk->prev->next = chunk->next;
                if (chunk->next) {
                    chunk->next->prev = chunk->prev;
                }
            }
        }
    } else {
        /* Direct standalone allocation */
#ifdef OPENAL_TARGET_XBOX
        MmFreeContiguousMemory(ptr);
#else
        free(((void **)ptr)[-1]);
#endif
    }
}

size_t apu_prd_calculate_count(size_t buffer_size) {
    if (buffer_size == 0) {
        return 0;
    }
    return (buffer_size + NV_PAPU_PRD_MAX_CHUNK - 1) / NV_PAPU_PRD_MAX_CHUNK;
}

int apu_prd_build(uint32_t buffer_phys, size_t size, NVAPU_PRD_ENTRY *out_prds, size_t max_prds, size_t *out_count) {
    size_t required = apu_prd_calculate_count(size);
    size_t remaining;
    uint32_t curr_phys;
    size_t count;

    if (!out_prds || size == 0) {
        if (out_count) {
            *out_count = 0;
        }
        return -1;
    }

    if (max_prds < required) {
        if (out_count) {
            *out_count = 0;
        }
        return -2;
    }

    remaining = size;
    curr_phys = buffer_phys;
    count = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > NV_PAPU_PRD_MAX_CHUNK) ? NV_PAPU_PRD_MAX_CHUNK : remaining;

        out_prds[count].physical_address = curr_phys;
        out_prds[count].control = (uint32_t)(chunk & 0xFFFFu);

        curr_phys += (uint32_t)chunk;
        remaining -= chunk;

        if (remaining == 0) {
            out_prds[count].control |= NV_PAPU_PRD_EOT;
        }

        count++;
    }

    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int apu_prd_build_virt(const void *buffer, size_t size, NVAPU_PRD_ENTRY *out_prds, size_t max_prds, size_t *out_count) {
    uint32_t phys;
    if (!buffer) {
        if (out_count) {
            *out_count = 0;
        }
        return -1;
    }
    phys = apu_mem_get_physical_address(buffer);
    if (phys == 0) {
        if (out_count) {
            *out_count = 0;
        }
        return -1;
    }
    return apu_prd_build(phys, size, out_prds, max_prds, out_count);
}
