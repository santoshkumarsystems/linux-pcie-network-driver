/*
 * sk_e1000_dma.h
 *
 * DMA resource management interface for the sk_e1000 Linux
 * network driver.
 *
 * DMA = Direct Memory Access.
 *
 * This interface owns the hardware-independent bookkeeping required
 * for DMA-capable memory while the Linux DMA API performs the actual
 * device/platform mapping operations.
 *
 * The driver never assumes that a CPU virtual address is directly
 * usable by the device. CPU and device addresses are tracked
 * separately.
 *
 * Author: Santosh Kumar
 */

#ifndef SK_E1000_DMA_H
#define SK_E1000_DMA_H


#include <linux/types.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>


/*
 * --------------------------------------------------------------------------
 * DMA ADDRESSING
 * --------------------------------------------------------------------------
 *
 * The driver first requests a 64-bit DMA mask.
 *
 * If the platform cannot satisfy that requirement, it falls back to a
 * 32-bit DMA mask.
 *
 * The selected width is retained in driver state so initialization and
 * runtime evidence can report the actual DMA capability negotiated with
 * the Linux DMA subsystem.
 */

#define SK_E1000_DMA_BITS_64        64U
#define SK_E1000_DMA_BITS_32        32U


/*
 * --------------------------------------------------------------------------
 * DMA REGION
 * --------------------------------------------------------------------------
 *
 * A coherent DMA allocation has two different address views:
 *
 * cpu_addr:
 *
 *     Kernel virtual address used by the CPU.
 *
 * dma_addr:
 *
 *     DMA address supplied to the device.
 *
 *     This value must be used when programming hardware descriptor or
 *     buffer-address registers.
 *
 * size:
 *
 *     Number of bytes owned by this allocation.
 *
 *
 * CPU view:
 *
 *     cpu_addr
 *         |
 *         v
 *     system RAM
 *
 *
 * Device view:
 *
 *     dma_addr
 *         |
 *         v
 *     DMA mapping
 *         |
 *         v
 *     same backing memory
 *
 *
 * The two addresses must never be assumed to have the same numerical
 * value.
 */

struct sk_e1000_dma_region {
    void *cpu_addr;
    dma_addr_t dma_addr;
    size_t size;
};


/*
 * --------------------------------------------------------------------------
 * DMA CONFIGURATION
 * --------------------------------------------------------------------------
 *
 * Configure the device DMA mask and coherent DMA mask.
 *
 * The implementation attempts:
 *
 *     64-bit addressing
 *
 * and falls back to:
 *
 *     32-bit addressing
 *
 * when necessary.
 *
 * selected_bits receives the successfully negotiated DMA width.
 *
 * Return:
 *
 *     0
 *         DMA addressing configured successfully.
 *
 *     negative errno
 *         Neither supported DMA mask could be configured.
 */

int sk_e1000_dma_configure(struct pci_dev *pdev,
                           unsigned int *selected_bits);


/*
 * --------------------------------------------------------------------------
 * COHERENT DMA ALLOCATION
 * --------------------------------------------------------------------------
 *
 * Allocate one DMA-coherent memory region.
 *
 * Coherent memory is appropriate for hardware descriptor structures
 * because both the CPU and device can observe the shared memory without
 * explicit streaming-map synchronization calls.
 *
 * The returned CPU virtual address and DMA address are stored together
 * in region so ownership remains explicit.
 *
 * region must remain valid until sk_e1000_dma_free() releases the
 * allocation.
 *
 * Return:
 *
 *     0
 *         Allocation succeeded.
 *
 *     negative errno
 *         Allocation failed or arguments were invalid.
 */

int sk_e1000_dma_alloc(struct pci_dev *pdev,
                       struct sk_e1000_dma_region *region,
                       size_t size);


/*
 * --------------------------------------------------------------------------
 * COHERENT DMA RELEASE
 * --------------------------------------------------------------------------
 *
 * Release a region previously allocated by sk_e1000_dma_alloc().
 *
 * The helper also clears the software bookkeeping after release so stale
 * CPU or DMA addresses cannot accidentally be reused.
 *
 * Calling this helper with an empty region is safe.
 */

void sk_e1000_dma_free(struct pci_dev *pdev,
                       struct sk_e1000_dma_region *region);


#endif /* SK_E1000_DMA_H */