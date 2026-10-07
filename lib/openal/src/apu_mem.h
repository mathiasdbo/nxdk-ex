#ifndef APU_MEM_H
#define APU_MEM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox MCPX APU Physical Memory & PRD Scatter-Gather Management
 * ============================================================================
 * Hardware Contracts:
 * - Physical Region Descriptor (PRD) Entry: 8 bytes, packed.
 * - PRD Tables: 8-byte aligned in non-paged physically contiguous memory.
 * - Voice Context Array: 8192 bytes (64 * 128B), 4096-byte (4KB) page aligned.
 * - Kernel Allocator: MmAllocateContiguousMemoryEx with
 *   LowestAcceptableAddress = 0, HighestAcceptableAddress = 0x03FFAFFF (MAXRAM),
 *   caching = PAGE_READWRITE | PAGE_WRITECOMBINE.
 * ============================================================================
 */

/**
 * Maximum buffer chunk size that can be described by a single PRD entry.
 *
 * The 16-bit size field (control bits [0:15]) could hold 65535, but an odd
 * maximum would start every chunk after the first at an odd address and split
 * 16-bit samples. 0xF000 (61440) is a multiple of 4096 (the hardware buffer
 * path addresses memory in 4 KiB SGE pages, see lib/openal/docs/XEMU_VERIFICATION.md) and
 * of every PCM frame size (1, 2, 4), so every chunk except the last is a whole
 * number of pages and all chunk starts stay frame aligned.
 */
#define NV_PAPU_PRD_MAX_CHUNK           0xF000u

/**
 * End-of-Table (EOT) flag set on bit 31 of PRD control DWORD.
 * Informs the APU bus-master DMA engine of table termination / loop point.
 */
#define NV_PAPU_PRD_EOT                 (1u << 31)

/* Aliases for convenience */
#define APU_PRD_MAX_CHUNK               NV_PAPU_PRD_MAX_CHUNK
#define APU_PRD_EOT                     NV_PAPU_PRD_EOT

/**
 * Default contiguous memory pool allocation size (2 MB).
 */
#define APU_MEM_DEFAULT_POOL_SIZE       (2u * 1024u * 1024u)

/**
 * Physical Region Descriptor (PRD) table entry for APU scatter-gather DMA engine.
 * Total size: exactly 8 bytes.
 */
typedef struct {
    uint32_t physical_address;          /**< 32-bit physical RAM address of PCM data chunk */
    uint32_t control;                   /**< [0:15] Size in bytes (<= NV_PAPU_PRD_MAX_CHUNK), [31] EOT flag */
} __attribute__((packed)) NVAPU_PRD_ENTRY;

/**
 * Initialize the contiguous physical memory pool allocator.
 *
 * Allocates non-paged physically contiguous memory with 4096-byte page alignment
 * and PAGE_READWRITE | PAGE_WRITECOMBINE caching flags.
 *
 * @param pool_size Size of pool in bytes. If 0, APU_MEM_DEFAULT_POOL_SIZE is used.
 * @return 0 on success, negative error code on failure.
 */
int apu_mem_init(size_t pool_size);

/**
 * Allocate physically contiguous memory with specified alignment.
 *
 * Satisfies alignment requirements such as 4096-byte page alignment for voice
 * context slabs or 8-byte alignment for PRD tables.
 *
 * @param size Size in bytes to allocate.
 * @param alignment Required power-of-2 byte alignment (e.g. 8, 64, 4096).
 *                  If 0, default 8-byte alignment is applied.
 * @param out_phys Pointer to store the 32-bit physical DMA address (may be NULL).
 * @return Virtual address pointer, or NULL on allocation failure.
 */
void *apu_mem_alloc_phys(size_t size, size_t alignment, uint32_t *out_phys);

/**
 * Free physically contiguous memory previously allocated with apu_mem_alloc_phys.
 *
 * @param ptr Virtual address returned by apu_mem_alloc_phys. If NULL, does nothing.
 */
void apu_mem_free(void *ptr);

#define apu_mem_free_phys(ptr) apu_mem_free(ptr)

/**
 * Release all resources and destroy the contiguous memory pool.
 */
void apu_mem_shutdown(void);

/**
 * Calculate the number of PRD entries required for a buffer of given size.
 *
 * Buffers exceeding NV_PAPU_PRD_MAX_CHUNK (61440 bytes) require multiple PRD entries.
 *
 * @param buffer_size Buffer size in bytes.
 * @return Number of NVAPU_PRD_ENTRY elements needed.
 */
size_t apu_prd_calculate_count(size_t buffer_size);

/**
 * Build a Physical Region Descriptor (PRD) table from a physical buffer address.
 *
 * Buffers larger than NV_PAPU_PRD_MAX_CHUNK (61440 bytes) are split across
 * consecutive PRDs advancing the physical address accordingly. All chunks but
 * the last are NV_PAPU_PRD_MAX_CHUNK bytes. The final entry has the
 * NV_PAPU_PRD_EOT (bit 31) flag set.
 *
 * @param buffer_phys Base physical address of contiguous audio buffer.
 * @param size Total buffer size in bytes.
 * @param out_prds Destination array of NVAPU_PRD_ENTRY. Must have capacity >= max_prds.
 * @param max_prds Maximum entries out_prds can hold.
 * @param out_count Pointer to receive count of PRD entries written (may be NULL).
 * @return 0 on success, negative error code on failure (-1: bad args, -2: table overflow).
 */
int apu_prd_build(uint32_t buffer_phys, size_t size, NVAPU_PRD_ENTRY *out_prds, size_t max_prds, size_t *out_count);

/**
 * Build a PRD table from a virtual buffer pointer (must be contiguous memory).
 *
 * Translates virtual address to physical address via kernel MmGetPhysicalAddress
 * or internal pool mapping, then delegates to apu_prd_build.
 *
 * @param buffer Virtual address of contiguous audio buffer.
 * @param size Total buffer size in bytes.
 * @param out_prds Destination array of NVAPU_PRD_ENTRY.
 * @param max_prds Maximum entries out_prds can hold.
 * @param out_count Pointer to receive count of PRD entries written (may be NULL).
 * @return 0 on success, negative error code on failure.
 */
int apu_prd_build_virt(const void *buffer, size_t size, NVAPU_PRD_ENTRY *out_prds, size_t max_prds, size_t *out_count);

/**
 * Resolve the physical DMA address of a virtual memory pointer.
 *
 * @param ptr Virtual pointer within contiguous memory or pool.
 * @return 32-bit physical RAM address, or 0 if unresolvable.
 */
uint32_t apu_mem_get_physical_address(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* APU_MEM_H */
