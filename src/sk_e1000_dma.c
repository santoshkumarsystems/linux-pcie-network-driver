/*
 * sk_e1000_dma.c
 *
 * DMA resource-management implementation for the sk_e1000 Linux
 * network driver.
 *
 * DMA = Direct Memory Access.
 *
 * This module is responsible for:
 *
 *   - negotiating the device DMA addressing width
 *   - allocating DMA-coherent memory
 *   - tracking CPU and device address views separately
 *   - releasing DMA resources safely
 *
 * Actual RX/TX descriptor programming and packet DMA are intentionally
 * outside this module and will be introduced in later milestones.
 *
 * Author: Santosh Kumar
 */

#include <linux/errno.h>
#include <linux/string.h>

#include "sk_e1000_dma.h"


/*
 * --------------------------------------------------------------------------
 * DMA ADDRESSING CONFIGURATION
 * --------------------------------------------------------------------------
 *
 * A DMA-capable device cannot simply assume that every system-memory
 * address is reachable.
 *
 * Linux must be told how many address bits the device can generate.
 *
 * dma_set_mask_and_coherent() configures both:
 *
 *     streaming DMA mappings
 *
 * and:
 *
 *     coherent DMA allocations
 *
 * using the same address mask.
 *
 *
 * Negotiation policy:
 *
 *     try 64-bit DMA
 *          |
 *          +---- success ----> use 64-bit
 *          |
 *          v
 *     try 32-bit DMA
 *          |
 *          +---- success ----> use 32-bit
 *          |
 *          v
 *        failure
 *
 *
 * The selected width is returned to the caller so the main driver can
 * report the actual capability negotiated with Linux.
 */

int sk_e1000_dma_configure(struct pci_dev *pdev,
                           unsigned int *selected_bits)
{
    int ret;


    /*
     * The PCI device and output pointer are required.
     */
    if (!pdev || !selected_bits)
        return -EINVAL;


    /*
     * Do not expose an old value if both negotiations fail.
     */
    *selected_bits = 0;


    /*
     * ------------------------------------------------------------------
     * TRY 64-BIT DMA
     * ------------------------------------------------------------------
     *
     * DMA_BIT_MASK(64) describes an address mask capable of reaching
     * the full 64-bit DMA address space supported by the DMA API.
     */

    ret =
        dma_set_mask_and_coherent(
            &pdev->dev,
            DMA_BIT_MASK(SK_E1000_DMA_BITS_64));


    if (ret == 0) {

        *selected_bits =
            SK_E1000_DMA_BITS_64;

        return 0;
    }


    /*
     * ------------------------------------------------------------------
     * FALL BACK TO 32-BIT DMA
     * ------------------------------------------------------------------
     *
     * A platform may be unable to provide the requested 64-bit DMA
     * capability even when the hardware format itself can represent
     * wider addresses.
     *
     * Falling back allows the driver to operate when all DMA resources
     * can be placed within the lower 4 GiB addressable range.
     */

    ret =
        dma_set_mask_and_coherent(
            &pdev->dev,
            DMA_BIT_MASK(SK_E1000_DMA_BITS_32));


    if (ret == 0) {

        *selected_bits =
            SK_E1000_DMA_BITS_32;

        return 0;
    }


    /*
     * Neither supported DMA addressing mode could be configured.
     *
     * The caller must abort device initialization because later DMA
     * allocations or packet mappings would not be safe.
     */

    return ret;
}


/*
 * --------------------------------------------------------------------------
 * DMA-COHERENT ALLOCATION
 * --------------------------------------------------------------------------
 *
 * dma_alloc_coherent() gives the driver two views of the same backing
 * memory:
 *
 *     CPU virtual address
 *
 *         region->cpu_addr
 *
 * and:
 *
 *     device DMA address
 *
 *         region->dma_addr
 *
 *
 * The addresses serve different consumers:
 *
 *     CPU                         NIC
 *      |                           |
 *      v                           v
 * cpu_addr                     dma_addr
 *      \                           /
 *       \                         /
 *        +------ same RAM -------+
 *
 *
 * The CPU address must never be programmed directly into the NIC.
 *
 * Hardware registers and descriptors receive the DMA address returned
 * by the Linux DMA subsystem.
 */

int sk_e1000_dma_alloc(struct pci_dev *pdev,
                       struct sk_e1000_dma_region *region,
                       size_t size)
{
    void *cpu_addr;
    dma_addr_t dma_addr;


    /*
     * Validate required inputs.
     *
     * A zero-length DMA allocation has no useful meaning for this
     * driver's resource model.
     */

    if (!pdev || !region || size == 0)
        return -EINVAL;


    /*
     * Reject accidental reuse of a region that already owns an
     * allocation.
     *
     * DMA addresses can legitimately be zero on some systems, so
     * cpu_addr and size are also checked rather than relying only on
     * dma_addr as the ownership indicator.
     */

    if (region->cpu_addr ||
        region->dma_addr != (dma_addr_t)0 ||
        region->size != 0) {

        return -EBUSY;
    }


    /*
     * ------------------------------------------------------------------
     * ALLOCATE COHERENT DMA MEMORY
     * ------------------------------------------------------------------
     *
     * GFP_KERNEL is appropriate because allocation occurs during the
     * normal probe path where sleeping is permitted.
     *
     * dma_alloc_coherent() returns:
     *
     *     cpu_addr
     *         CPU-accessible kernel virtual address
     *
     *     dma_addr
     *         address to provide to the device
     */

    cpu_addr =
        dma_alloc_coherent(
            &pdev->dev,
            size,
            &dma_addr,
            GFP_KERNEL);


    if (!cpu_addr)
        return -ENOMEM;


    /*
     * Initialize the shared region to a known state.
     *
     * Descriptor rings will eventually be built in coherent memory.
     * Starting with deterministic contents prevents stale data from
     * being interpreted as descriptor ownership/status information.
     */

    memset(cpu_addr,
           0,
           size);


    /*
     * Publish ownership only after allocation and initialization have
     * completed successfully.
     */

    region->cpu_addr = cpu_addr;
    region->dma_addr = dma_addr;
    region->size = size;


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * DMA-COHERENT RELEASE
 * --------------------------------------------------------------------------
 *
 * dma_free_coherent() must receive the same:
 *
 *     device
 *     allocation size
 *     CPU address
 *     DMA address
 *
 * associated with the original allocation.
 *
 * For that reason the region structure keeps all allocation metadata
 * together for the entire ownership lifetime.
 */

void sk_e1000_dma_free(struct pci_dev *pdev,
                       struct sk_e1000_dma_region *region)
{
    /*
     * Defensive handling for an empty cleanup path.
     */
    if (!pdev || !region)
        return;


    /*
     * An empty region owns no DMA allocation.
     */
    if (!region->cpu_addr)
        return;


    /*
     * Release the coherent allocation through the Linux DMA API.
     */

    dma_free_coherent(
        &pdev->dev,
        region->size,
        region->cpu_addr,
        region->dma_addr);


    /*
     * ------------------------------------------------------------------
     * CLEAR SOFTWARE OWNERSHIP
     * ------------------------------------------------------------------
     *
     * Once dma_free_coherent() returns, neither address may be used
     * again.
     *
     * Clearing all bookkeeping helps prevent:
     *
     *     - stale CPU pointer reuse
     *     - stale DMA address reuse
     *     - accidental double free
     */

    region->cpu_addr = NULL;
    region->dma_addr = (dma_addr_t)0;
    region->size = 0;
}