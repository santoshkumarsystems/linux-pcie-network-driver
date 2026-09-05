/*
 * sk_e1000.c
 *
 * Linux PCI network driver for the QEMU-emulated
 * Intel 82540EM Gigabit Ethernet Controller.
 *
 * Author: Santosh Kumar
 *
 * Implemented milestones:
 *
 *   - PCI Vendor / Device ID matching
 *   - PCI device enablement
 *   - BAR0 discovery and ownership
 *   - BAR0 MMIO mapping
 *   - CTRL and STATUS register access
 *   - PCI bus mastering
 *   - DMA mask negotiation
 *   - Intel legacy RX/TX descriptor definitions
 *   - compile-time descriptor-layout validation
 *   - separate DMA-coherent RX/TX descriptor-ring allocation
 *   - safe RX/TX descriptor-ring base/length MMIO programming
 *   - RX/TX packet engines kept disabled during ring bring-up
 *   - hardware-independent producer / consumer ring logic
 *   - legacy INTx interrupt registration
 *   - e1000 interrupt masking and cause handling
 *   - deterministic interrupt-path validation
 *   - shared hardware-independent interrupt decision logic
 *   - dependency-safe error unwind and resource cleanup
 *
 * Current data-path validation work:
 *
 *   - deterministic 60-byte Ethernet TX frame construction
 *   - streaming DMA mapping for one TX packet buffer
 *   - legacy TX descriptor submission through TDH/TDT
 *   - TX descriptor completion validation through DD write-back
 *   - opt-in deterministic external RX frame validation
 *   - streaming DMA mapping for one RX packet buffer
 *   - legacy RX descriptor ownership through RDH/RDT
 *   - RX DD/EOP completion and exact byte-for-byte validation
 *
 * Future milestones:
 *
 *   - connect reusable producer / consumer state to hardware rings
 *   - Linux net_device integration
 *   - asynchronous packet receive / transmit
 *   - driver statistics
 *
 * Hardware target:
 *
 *   Vendor ID : 0x8086
 *   Device ID : 0x100e
 *   Device    : Intel 82540EM Gigabit Ethernet Controller
 *
 * Development environment:
 *
 *   QEMU/KVM emulated PCI hardware
 *   Linux kernel module
 *
 * The driver discovers runtime PCI and DMA resources dynamically.
 * Physical BAR addresses, IRQ numbers, and DMA addresses are never
 * hard-coded.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <linux/byteorder/generic.h>

#include "sk_e1000_logic.h"
#include "sk_e1000_dma.h"
#include "sk_e1000_desc.h"
#include "sk_e1000_frame.h"


/*
 * --------------------------------------------------------------------------
 * PCI DEVICE IDENTIFICATION
 * --------------------------------------------------------------------------
 *
 * Linux reads Vendor ID and Device ID from PCI configuration space.
 *
 * The PCI core compares those values against sk_e1000_pci_ids[].
 * If they match and the device is available, Linux calls probe().
 */

#define SK_E1000_VENDOR_ID              0x8086
#define SK_E1000_DEVICE_ID              0x100e


/*
 * --------------------------------------------------------------------------
 * PCI BAR
 * --------------------------------------------------------------------------
 *
 * BAR = Base Address Register.
 *
 * The Intel 82540EM exposes its memory-mapped device register space
 * through BAR0.
 *
 * BAR0 itself is located in PCI configuration space. Linux reads it,
 * assigns the physical address range, and exposes that information
 * through the PCI resource APIs.
 *
 * The driver does NOT hard-code the observed QEMU BAR address.
 */

#define SK_E1000_BAR                    0


/*
 * --------------------------------------------------------------------------
 * CORE E1000 MMIO REGISTERS
 * --------------------------------------------------------------------------
 *
 * These are offsets relative to BAR0.
 *
 * CTRL:
 *     Device Control Register.
 *
 * STATUS:
 *     Device Status Register.
 *
 * RCTL:
 *     Receive Control Register.
 *
 *     RCTL.EN enables the receive packet engine. The Intel initialization
 *     sequence recommends leaving this bit clear until the receive
 *     descriptor ring and receive buffers are ready.
 *
 * TCTL:
 *     Transmit Control Register.
 *
 *     TCTL.EN enables the transmit packet engine.
 */

#define E1000_REG_CTRL                  0x0000
#define E1000_REG_STATUS                0x0008
#define E1000_REG_RCTL                  0x0100
#define E1000_REG_TCTL                  0x0400

#define E1000_RCTL_EN                   0x00000002U
#define E1000_RCTL_UPE                  0x00000008U
#define E1000_RCTL_SECRC                0x04000000U

#define E1000_TCTL_EN                   0x00000002U
#define E1000_TCTL_PSP                  0x00000008U
#define E1000_TCTL_CT_MASK              0x00000ff0U
#define E1000_TCTL_COLD_MASK            0x003ff000U
#define E1000_TCTL_RTLC                 0x01000000U

#define E1000_TCTL_CT_SHIFT             4U
#define E1000_TCTL_COLD_SHIFT           12U

#define E1000_COLLISION_THRESHOLD       15U
#define E1000_COLLISION_DISTANCE        63U


/*
 * --------------------------------------------------------------------------
 * RX DESCRIPTOR-RING REGISTERS
 * --------------------------------------------------------------------------
 *
 * RDBAL / RDBAH:
 *     Receive Descriptor Base Address Low / High.
 *
 *     Together they contain the device-visible 64-bit DMA address of
 *     the first receive descriptor.
 *
 *     Intel requires the base address to be 16-byte aligned. Hardware
 *     ignores the low four bits of RDBAL.
 *
 * RDLEN:
 *     Receive Descriptor Length in bytes.
 *
 *     Intel requires the ring length to be a multiple of 128 bytes.
 *
 * RDH:
 *     Receive Descriptor Head. Hardware advances this index while
 *     processing receive descriptors.
 *
 *     Intel documents software writes to RDH as an initialization
 *     operation performed after reset and before RCTL.EN is set.
 *
 * RDT:
 *     Receive Descriptor Tail. Software writes this register to make
 *     receive descriptors available to hardware.
 *
 * The base/length programming stage writes only RDBAL/RDBAH/RDLEN.
 * The deterministic RX validation path initializes RDH/RDT separately,
 * while RCTL.EN is clear, after the RX packet buffer and descriptor
 * ownership are established.
 */

#define E1000_REG_RDBAL                 0x2800
#define E1000_REG_RDBAH                 0x2804
#define E1000_REG_RDLEN                 0x2808
#define E1000_REG_RDH                   0x2810
#define E1000_REG_RDT                   0x2818


/*
 * --------------------------------------------------------------------------
 * TX DESCRIPTOR-RING REGISTERS
 * --------------------------------------------------------------------------
 *
 * TDBAL / TDBAH:
 *     Transmit Descriptor Base Address Low / High.
 *
 *     Together they contain the device-visible 64-bit DMA address of
 *     the first transmit descriptor.
 *
 *     Intel requires the base address to be 16-byte aligned. Hardware
 *     ignores the low four bits of TDBAL.
 *
 * TDLEN:
 *     Transmit Descriptor Length in bytes.
 *
 *     Intel requires the ring length to be a multiple of 128 bytes.
 *
 * TDH:
 *     Transmit Descriptor Head. Hardware advances this index while
 *     processing transmit descriptors.
 *
 *     Intel documents software writes to TDH as an initialization
 *     operation performed after reset and before TCTL.EN is set.
 *
 * TDT:
 *     Transmit Descriptor Tail. Software advances this register after
 *     preparing descriptors for transmission.
 *
 * The base/length programming stage writes only TDBAL/TDBAH/TDLEN.
 * The deterministic TX validation path initializes TDH/TDT separately,
 * while TCTL.EN is clear, after the TX packet buffer and descriptor
 * ownership are established.
 */

#define E1000_REG_TDBAL                 0x3800
#define E1000_REG_TDBAH                 0x3804
#define E1000_REG_TDLEN                 0x3808
#define E1000_REG_TDH                   0x3810
#define E1000_REG_TDT                   0x3818


/*
 * --------------------------------------------------------------------------
 * INTERRUPT REGISTERS
 * --------------------------------------------------------------------------
 *
 * ICR = Interrupt Cause Read
 *
 *     Reports pending interrupt causes.
 *
 *     Reading ICR also acknowledges/clears the reported causes for
 *     this controller model.
 *
 *
 * ICS = Interrupt Cause Set
 *
 *     Allows software to set interrupt-cause bits.
 *
 *     Used during bring-up to exercise the complete interrupt path
 *     through the QEMU e1000 hardware model.
 *
 *
 * IMS = Interrupt Mask Set
 *
 *     Writing a 1 enables the corresponding interrupt source.
 *
 *
 * IMC = Interrupt Mask Clear
 *
 *     Writing a 1 disables the corresponding interrupt source.
 */

#define E1000_REG_ICR                   0x00c0
#define E1000_REG_ICS                   0x00c8
#define E1000_REG_IMS                   0x00d0
#define E1000_REG_IMC                   0x00d8


/*
 * Maximum time probe() waits for the deterministic interrupt test.
 */

#define SK_E1000_IRQ_TIMEOUT_MS         1000
#define SK_E1000_TX_TIMEOUT_MS          1000
#define SK_E1000_RX_TIMEOUT_MS          15000

#define SK_E1000_RX_TEST_BUFFER_SIZE    2048U


/*
 * --------------------------------------------------------------------------
 * OPTIONAL EXTERNAL RX VALIDATION
 * --------------------------------------------------------------------------
 *
 * The deterministic RX test requires an independently generated Ethernet
 * frame to arrive from the WSL/Linux host through the QEMU TAP backend.
 *
 * It is therefore opt-in rather than part of every probe. Keeping the
 * default false preserves normal regression tests and ordinary module load
 * behavior when no external injector is running.
 *
 * Example:
 *
 *     sudo insmod ./sk_e1000.ko run_rx_test=1
 *
 * The host then injects the matching frame with:
 *
 *     sudo python3 tests/integration/inject_rx_frame.py
 */
static bool run_rx_test;

module_param(
    run_rx_test,
    bool,
    0444);

MODULE_PARM_DESC(
    run_rx_test,
    "Run deterministic external RX DMA validation during probe");


/*
 * --------------------------------------------------------------------------
 * DRIVER PRIVATE STATE
 * --------------------------------------------------------------------------
 *
 * One instance of this structure exists for each NIC controlled by
 * this driver.
 *
 * pdev:
 *
 *     Linux representation of the PCI device.
 *
 *
 * bar0:
 *
 *     Kernel virtual address returned by pci_iomap().
 *
 *     This is device MMIO, not ordinary RAM.
 *
 *
 * dma_bits:
 *
 *     DMA addressing width successfully negotiated with the Linux
 *     DMA subsystem.
 *
 *     The driver first requests 64-bit DMA addressing and falls back
 *     to 32-bit addressing when necessary.
 *
 *
 * rx_desc_ring / tx_desc_ring:
 *
 *     Independent DMA-coherent memory regions containing the RX and
 *     TX descriptor arrays.
 *
 *     Each region contains:
 *
 *         cpu_addr
 *             CPU-visible kernel virtual address
 *
 *         dma_addr
 *             device-visible DMA address
 *
 *         size
 *             allocation size
 *
 *     Each ring currently contains SK_E1000_RING_COUNT legacy
 *     descriptors and occupies SK_E1000_RING_SIZE bytes.
 *
 *     RX/TX packet buffers are allocated only by the deterministic
 *     streaming-DMA validation paths.
 *
 *     During probe(), the DMA base addresses and ring lengths are
 *     programmed into the NIC RDBAL/RDBAH/RDLEN and
 *     TDBAL/TDBAH/TDLEN registers while both packet engines remain
 *     disabled.
 *
 *     RX/TX head/tail initialization is performed separately by the
 *     deterministic data-path validation routines only after descriptor
 *     and packet-buffer ownership have been established.
 *
 *
 * tx_test_cpu_addr / tx_test_dma_addr / tx_test_len / tx_test_mapped:
 *
 *     Ownership state for the deterministic TX validation packet.
 *
 *     The packet payload is normal kernel memory allocated with
 *     kmalloc(). It is exposed to the NIC through a streaming DMA
 *     mapping created with dma_map_single(..., DMA_TO_DEVICE).
 *
 *     This is intentionally different from the descriptor rings:
 *
 *         descriptor rings -> DMA-coherent memory
 *         packet payload   -> streaming DMA mapping
 *
 *     tx_test_mapped records whether dma_unmap_single() is required.
 *     A DMA address value of zero is not used as an ownership test
 *     because zero can be a valid DMA address on some platforms.
 *
 *
 * rx_test_cpu_addr / rx_test_dma_addr / rx_test_buffer_size /
 * rx_test_mapped:
 *
 *     Ownership state for the opt-in deterministic RX validation buffer.
 *
 *     The packet buffer is ordinary kmalloc() memory exposed to the NIC
 *     through dma_map_single(..., DMA_FROM_DEVICE). The descriptor ring
 *     remains coherent while packet data uses the streaming DMA API.
 *
 *     Software must not inspect packet bytes while the device owns the
 *     DMA_FROM_DEVICE mapping. The validation path first observes RX
 *     descriptor completion, stops the receive engine, unmaps the buffer,
 *     and only then performs the exact CPU byte comparison.
 *
 *
 * irq_test_done:
 *
 *     Synchronization object used only for deterministic interrupt
 *     bring-up validation.
 *
 *
 * last_icr:
 *
 *     Most recently observed Interrupt Cause Register value during
 *     validation.
 */

struct sk_e1000_device {
    struct pci_dev *pdev;

    void __iomem *bar0;

    unsigned int dma_bits;

    struct sk_e1000_dma_region rx_desc_ring;
    struct sk_e1000_dma_region tx_desc_ring;

    void *tx_test_cpu_addr;
    dma_addr_t tx_test_dma_addr;
    size_t tx_test_len;
    bool tx_test_mapped;

    void *rx_test_cpu_addr;
    dma_addr_t rx_test_dma_addr;
    size_t rx_test_buffer_size;
    bool rx_test_mapped;

    struct completion irq_test_done;

    u32 last_icr;
};


/*
 * --------------------------------------------------------------------------
 * PCI DEVICE TABLE
 * --------------------------------------------------------------------------
 */

static const struct pci_device_id sk_e1000_pci_ids[] = {
    { PCI_DEVICE(SK_E1000_VENDOR_ID, SK_E1000_DEVICE_ID) },
    { 0, }
};


MODULE_DEVICE_TABLE(pci, sk_e1000_pci_ids);


/*
 * --------------------------------------------------------------------------
 * INTERRUPT MASKING
 * --------------------------------------------------------------------------
 *
 * Stop this NIC from generating interrupt events.
 *
 * Device interrupt generation must be stopped before free_irq()
 * removes the Linux handler.
 */

static void sk_e1000_disable_interrupts(struct sk_e1000_device *dev)
{
    /*
     * Writing all 1 bits to IMC clears all interrupt-enable bits.
     */
    iowrite32(
        0xffffffff,
        dev->bar0 + E1000_REG_IMC);


    /*
     * MMIO writes may be posted.
     *
     * Read STATUS so the previous write reaches the device before
     * execution proceeds.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    /*
     * Clear already-pending interrupt causes.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_ICR);
}



/*
 * --------------------------------------------------------------------------
 * RX/TX PACKET-ENGINE DISABLE
 * --------------------------------------------------------------------------
 *
 * Descriptor-ring registers must not be reconfigured while the associated
 * packet engines can still consume descriptors.
 *
 * Clear RCTL.EN and TCTL.EN, flush the posted MMIO writes through STATUS,
 * and wait briefly for outstanding device activity to quiesce.
 *
 * The upstream Linux e1000 driver uses the same high-level safety pattern
 * when bringing the device down: disable RX/TX, flush, then wait before
 * continuing teardown/reconfiguration.
 */
static int
sk_e1000_disable_packet_engines(struct sk_e1000_device *dev)
{
    u32 rctl;
    u32 tctl;


    rctl =
        ioread32(
            dev->bar0 + E1000_REG_RCTL);

    tctl =
        ioread32(
            dev->bar0 + E1000_REG_TCTL);


    iowrite32(
        rctl & ~E1000_RCTL_EN,
        dev->bar0 + E1000_REG_RCTL);

    iowrite32(
        tctl & ~E1000_TCTL_EN,
        dev->bar0 + E1000_REG_TCTL);


    /*
     * Flush both posted writes before waiting for the engines to stop.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    /*
     * Match the conservative quiesce interval used by the upstream
     * Linux e1000 driver after disabling receive and transmit.
     */
    msleep(10);


    /*
     * Verify that the enable bits remain clear before descriptor-ring
     * addressing is changed or DMA memory is later released.
     */
    rctl =
        ioread32(
            dev->bar0 + E1000_REG_RCTL);

    tctl =
        ioread32(
            dev->bar0 + E1000_REG_TCTL);


    if ((rctl & E1000_RCTL_EN) ||
        (tctl & E1000_TCTL_EN)) {

        pr_err(
            "sk_e1000: failed to disable RX/TX packet engines "
            "RCTL=0x%08x TCTL=0x%08x\n",
            rctl,
            tctl);

        return -EIO;
    }


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * DESCRIPTOR-RING BASE/LENGTH PROGRAMMING
 * --------------------------------------------------------------------------
 *
 * Program the Intel 82540EM with the DMA base address and byte length of
 * each coherent descriptor ring.
 *
 * This function intentionally does NOT write RDH/RDT/TDH/TDT.
 *
 * Head/tail programming belongs to queue ownership, not ring-address
 * discovery. The deterministic TX and RX validation paths initialize
 * TDH/TDT and RDH/RDT separately, while the corresponding packet engine is
 * disabled, only after descriptor memory and the mapped packet buffer are
 * ready.
 *
 * What this function establishes:
 *
 *     RX DMA address  -> RDBAL / RDBAH
 *     RX ring bytes   -> RDLEN
 *
 *     TX DMA address  -> TDBAL / TDBAH
 *     TX ring bytes   -> TDLEN
 *
 * RCTL.EN and TCTL.EN remain clear throughout this function.
 */
static int
sk_e1000_program_descriptor_ring_addressing(
    struct sk_e1000_device *dev)
{
    u64 rx_dma;
    u64 tx_dma;

    u64 rx_programmed;
    u64 tx_programmed;

    u32 rx_low;
    u32 rx_high;
    u32 tx_low;
    u32 tx_high;

    u32 rx_len;
    u32 tx_len;

    int ret;


    rx_dma =
        (u64)dev->rx_desc_ring.dma_addr;

    tx_dma =
        (u64)dev->tx_desc_ring.dma_addr;


    /*
     * Intel requires descriptor-ring base addresses to be aligned on
     * 16-byte boundaries.
     *
     * dma_alloc_coherent() normally returns substantially stronger
     * alignment, but the driver validates the hardware contract rather
     * than depending on that implementation detail.
     */
    if ((rx_dma & (SK_E1000_RING_BASE_ALIGNMENT - 1U)) != 0U) {

        pr_err(
            "sk_e1000: RX descriptor-ring DMA address is not "
            "%u-byte aligned\n",
            SK_E1000_RING_BASE_ALIGNMENT);

        return -EINVAL;
    }


    if ((tx_dma & (SK_E1000_RING_BASE_ALIGNMENT - 1U)) != 0U) {

        pr_err(
            "sk_e1000: TX descriptor-ring DMA address is not "
            "%u-byte aligned\n",
            SK_E1000_RING_BASE_ALIGNMENT);

        return -EINVAL;
    }


    /*
     * The allocation size is part of the hardware contract as well.
     * The compile-time descriptor checks already guarantee that
     * SK_E1000_RING_SIZE is a valid multiple of Intel's 128-byte ring
     * length granularity.
     */
    if (dev->rx_desc_ring.size != SK_E1000_RING_SIZE ||
        dev->tx_desc_ring.size != SK_E1000_RING_SIZE) {

        pr_err(
            "sk_e1000: descriptor-ring allocation size mismatch "
            "RX=%zu TX=%zu expected=%u\n",
            dev->rx_desc_ring.size,
            dev->tx_desc_ring.size,
            SK_E1000_RING_SIZE);

        return -EINVAL;
    }


    /*
     * Establish the safety invariant before changing descriptor-ring
     * addressing: neither packet engine may be active.
     */
    ret =
        sk_e1000_disable_packet_engines(
            dev);

    if (ret)
        return ret;


    /*
     * Program RX descriptor-ring base address.
     *
     * RDBAL receives address bits 31:0.
     * RDBAH receives address bits 63:32.
     */
    iowrite32(
        (u32)rx_dma,
        dev->bar0 + E1000_REG_RDBAL);

    iowrite32(
        (u32)(rx_dma >> 32),
        dev->bar0 + E1000_REG_RDBAH);

    iowrite32(
        SK_E1000_RING_SIZE,
        dev->bar0 + E1000_REG_RDLEN);


    /*
     * Program TX descriptor-ring base address and byte length.
     */
    iowrite32(
        (u32)tx_dma,
        dev->bar0 + E1000_REG_TDBAL);

    iowrite32(
        (u32)(tx_dma >> 32),
        dev->bar0 + E1000_REG_TDBAH);

    iowrite32(
        SK_E1000_RING_SIZE,
        dev->bar0 + E1000_REG_TDLEN);


    /*
     * Flush posted descriptor-register writes before readback
     * validation.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    /*
     * Read back the exact values accepted by the hardware model.
     *
     * Intel ignores the low four base-address bits; valid addresses
     * are already 16-byte aligned, so an exact reconstructed-address
     * comparison is appropriate.
     */
    rx_low =
        ioread32(
            dev->bar0 + E1000_REG_RDBAL);

    rx_high =
        ioread32(
            dev->bar0 + E1000_REG_RDBAH);

    rx_len =
        ioread32(
            dev->bar0 + E1000_REG_RDLEN);

    tx_low =
        ioread32(
            dev->bar0 + E1000_REG_TDBAL);

    tx_high =
        ioread32(
            dev->bar0 + E1000_REG_TDBAH);

    tx_len =
        ioread32(
            dev->bar0 + E1000_REG_TDLEN);


    rx_programmed =
        ((u64)rx_high << 32) |
        (u64)rx_low;

    tx_programmed =
        ((u64)tx_high << 32) |
        (u64)tx_low;


    if (rx_programmed != rx_dma ||
        rx_len != SK_E1000_RING_SIZE) {

        pr_err(
            "sk_e1000: RX descriptor-ring register readback mismatch "
            "base=0x%016llx expected=0x%016llx "
            "len=%u expected=%u\n",
            (unsigned long long)rx_programmed,
            (unsigned long long)rx_dma,
            rx_len,
            SK_E1000_RING_SIZE);

        return -EIO;
    }


    if (tx_programmed != tx_dma ||
        tx_len != SK_E1000_RING_SIZE) {

        pr_err(
            "sk_e1000: TX descriptor-ring register readback mismatch "
            "base=0x%016llx expected=0x%016llx "
            "len=%u expected=%u\n",
            (unsigned long long)tx_programmed,
            (unsigned long long)tx_dma,
            tx_len,
            SK_E1000_RING_SIZE);

        return -EIO;
    }


    /*
     * Confirm that programming the ring-address registers did not
     * accidentally enable either packet engine.
     */
    if ((ioread32(dev->bar0 + E1000_REG_RCTL) & E1000_RCTL_EN) ||
        (ioread32(dev->bar0 + E1000_REG_TCTL) & E1000_TCTL_EN)) {

        pr_err(
            "sk_e1000: RX/TX packet engine unexpectedly enabled "
            "during descriptor-ring programming\n");

        return -EIO;
    }


    pr_info(
        "sk_e1000: RX descriptor-ring registers programmed "
        "base=%pad length=%zu bytes\n",
        &dev->rx_desc_ring.dma_addr,
        dev->rx_desc_ring.size);

    pr_info(
        "sk_e1000: TX descriptor-ring registers programmed "
        "base=%pad length=%zu bytes\n",
        &dev->tx_desc_ring.dma_addr,
        dev->tx_desc_ring.size);

    pr_info(
        "sk_e1000: descriptor-ring base/length programming PASSED "
        "with RX/TX engines disabled\n");


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC TX PACKET-BUFFER RELEASE
 * --------------------------------------------------------------------------
 *
 * Packet payload memory uses the Linux streaming DMA API.
 *
 * The correct release order is:
 *
 *     stop hardware access
 *          ->
 *     dma_unmap_single()
 *          ->
 *     kfree()
 *
 * The caller is responsible for ensuring the transmit engine cannot still
 * consume the mapping before this helper is called.
 */
static void
sk_e1000_release_tx_test_buffer(
    struct sk_e1000_device *dev)
{
    if (!dev)
        return;


    if (dev->tx_test_mapped) {

        dma_unmap_single(
            &dev->pdev->dev,
            dev->tx_test_dma_addr,
            dev->tx_test_len,
            DMA_TO_DEVICE);

        dev->tx_test_mapped = false;
    }


    dev->tx_test_dma_addr =
        (dma_addr_t)0;

    dev->tx_test_len = 0;


    kfree(
        dev->tx_test_cpu_addr);

    dev->tx_test_cpu_addr = NULL;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC TX PACKET-BUFFER PREPARATION
 * --------------------------------------------------------------------------
 *
 * Build one exact 60-byte Ethernet frame in ordinary kernel memory and
 * create a streaming DMA mapping for device reads.
 *
 * The CPU must not modify the mapped packet bytes while the mapping is
 * owned by the device.
 */
static int
sk_e1000_prepare_tx_test_buffer(
    struct sk_e1000_device *dev)
{
    dma_addr_t dma_addr;
    size_t frame_len;


    if (!dev || !dev->pdev)
        return -EINVAL;


    /*
     * Reject accidental reuse of partially or fully owned state.
     */
    if (dev->tx_test_cpu_addr ||
        dev->tx_test_mapped ||
        dev->tx_test_len != 0) {

        return -EBUSY;
    }


    dev->tx_test_cpu_addr =
        kmalloc(
            SK_E1000_TX_TEST_FRAME_LEN,
            GFP_KERNEL);


    if (!dev->tx_test_cpu_addr)
        return -ENOMEM;


    frame_len =
        sk_e1000_build_tx_test_frame(
            dev->tx_test_cpu_addr,
            SK_E1000_TX_TEST_FRAME_LEN);


    if (frame_len != SK_E1000_TX_TEST_FRAME_LEN) {

        pr_err(
            "sk_e1000: deterministic TX frame construction failed "
            "length=%zu expected=%u\n",
            frame_len,
            SK_E1000_TX_TEST_FRAME_LEN);

        kfree(
            dev->tx_test_cpu_addr);

        dev->tx_test_cpu_addr = NULL;

        return -EINVAL;
    }


    /*
     * DMA_TO_DEVICE means the NIC will read these bytes from system RAM.
     *
     * dma_map_single() publishes the CPU-written packet contents to the
     * device according to the platform DMA rules and returns the address
     * that must be placed into the hardware descriptor.
     */
    dma_addr =
        dma_map_single(
            &dev->pdev->dev,
            dev->tx_test_cpu_addr,
            frame_len,
            DMA_TO_DEVICE);


    if (dma_mapping_error(
            &dev->pdev->dev,
            dma_addr)) {

        pr_err(
            "sk_e1000: deterministic TX packet DMA mapping failed\n");

        kfree(
            dev->tx_test_cpu_addr);

        dev->tx_test_cpu_addr = NULL;

        return -EIO;
    }


    dev->tx_test_dma_addr = dma_addr;
    dev->tx_test_len = frame_len;
    dev->tx_test_mapped = true;


    pr_info(
        "sk_e1000: deterministic TX packet mapped "
        "length=%zu DMA=%pad\n",
        dev->tx_test_len,
        &dev->tx_test_dma_addr);


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC ONE-DESCRIPTOR TX VALIDATION
 * --------------------------------------------------------------------------
 *
 * This is intentionally a bring-up validation path rather than a production
 * net_device transmit routine. It is scoped to this project's QEMU-emulated
 * Intel 82540EM target and does not replace the fuller reset/PHY/TIPG
 * initialization required for a production physical-NIC driver.
 *
 * Hardware/software flow:
 *
 *     CPU builds 60-byte frame
 *          ->
 *     dma_map_single(..., DMA_TO_DEVICE)
 *          ->
 *     TX descriptor[0]
 *          ->
 *     TDH = 0, TDT = 0 while TCTL.EN is clear
 *          ->
 *     configure and enable transmit unit
 *          ->
 *     memory ordering barrier
 *          ->
 *     TDT = 1
 *          ->
 *     NIC DMA-reads descriptor + packet
 *          ->
 *     NIC writes descriptor DD status
 *
 * The RS command bit requests status write-back. The function polls DD only
 * for deterministic bring-up evidence; a production driver would normally
 * reclaim completed descriptors asynchronously.
 */
static int
sk_e1000_run_deterministic_tx_test(
    struct sk_e1000_device *dev)
{
    struct sk_e1000_tx_desc *tx_desc;

    unsigned long deadline;

    u32 tctl;
    u32 tctl_readback;
    u32 tdh;
    u32 tdt;
    u32 required_tctl;

    u8 status;

    int ret;
    int stop_ret;


    if (!dev ||
        !dev->bar0 ||
        !dev->tx_desc_ring.cpu_addr ||
        dev->tx_desc_ring.size < sizeof(*tx_desc)) {

        return -EINVAL;
    }


    /*
     * Queue ownership changes only while both packet engines are stopped.
     */
    ret =
        sk_e1000_disable_packet_engines(
            dev);

    if (ret)
        return ret;


    ret =
        sk_e1000_prepare_tx_test_buffer(
            dev);

    if (ret)
        return ret;


    tx_desc =
        (struct sk_e1000_tx_desc *)
            dev->tx_desc_ring.cpu_addr;


    /*
     * Descriptor zero is the only descriptor submitted by this validation.
     *
     * buffer_addr:
     *     device-visible packet DMA address
     *
     * length:
     *     60 bytes, before Ethernet FCS
     *
     * EOP:
     *     this descriptor ends the packet
     *
     * IFCS:
     *     request Ethernet FCS insertion according to the legacy Intel
     *     descriptor contract
     *
     * RS:
     *     request descriptor status write-back so DD can be verified
     */
    memset(
        tx_desc,
        0,
        sizeof(*tx_desc));

    tx_desc->buffer_addr =
        cpu_to_le64(
            (u64)dev->tx_test_dma_addr);

    tx_desc->length =
        cpu_to_le16(
            (u16)dev->tx_test_len);

    tx_desc->cso = 0;

    tx_desc->cmd =
        SK_E1000_TXD_CMD_EOP |
        SK_E1000_TXD_CMD_IFCS |
        SK_E1000_TXD_CMD_RS;

    tx_desc->status = 0;
    tx_desc->css = 0;

    tx_desc->special =
        cpu_to_le16(0);


    /*
     * Initialize the single TX queue while transmit is disabled.
     *
     * TDT points one descriptor beyond the final descriptor hardware may
     * consume. Therefore the empty ring begins with TDH == TDT == 0.
     */
    iowrite32(
        0,
        dev->bar0 + E1000_REG_TDH);

    iowrite32(
        0,
        dev->bar0 + E1000_REG_TDT);


    /*
     * Flush the posted head/tail initialization writes before validating
     * them or enabling the transmit engine.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    tdh =
        ioread32(
            dev->bar0 + E1000_REG_TDH);

    tdt =
        ioread32(
            dev->bar0 + E1000_REG_TDT);


    if (tdh != 0 || tdt != 0) {

        pr_err(
            "sk_e1000: TX head/tail initialization failed "
            "TDH=%u TDT=%u\n",
            tdh,
            tdt);

        ret = -EIO;

        goto out_stop_and_release;
    }


    /*
     * Configure the transmit control fields used by Intel's e1000 family:
     *
     * EN:
     *     enable transmitter
     *
     * PSP:
     *     pad short packets
     *
     * CT:
     *     collision threshold = 15
     *
     * COLD:
     *     collision distance = 63
     *
     * RTLC:
     *     retransmit on late collision
     *
     * Preserve unrelated TCTL bits while replacing CT/COLD with the
     * intended values.
     */
    tctl =
        ioread32(
            dev->bar0 + E1000_REG_TCTL);

    tctl &=
        ~(E1000_TCTL_CT_MASK |
          E1000_TCTL_COLD_MASK);

    required_tctl =
        E1000_TCTL_EN |
        E1000_TCTL_PSP |
        E1000_TCTL_RTLC |
        (E1000_COLLISION_THRESHOLD <<
         E1000_TCTL_CT_SHIFT) |
        (E1000_COLLISION_DISTANCE <<
         E1000_TCTL_COLD_SHIFT);

    tctl |= required_tctl;


    iowrite32(
        tctl,
        dev->bar0 + E1000_REG_TCTL);


    /*
     * Flush the posted control write before ringing the TX tail doorbell.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    tctl_readback =
        ioread32(
            dev->bar0 + E1000_REG_TCTL);


    if ((tctl_readback &
         (E1000_TCTL_EN |
          E1000_TCTL_PSP |
          E1000_TCTL_CT_MASK |
          E1000_TCTL_COLD_MASK |
          E1000_TCTL_RTLC)) !=
        required_tctl) {

        pr_err(
            "sk_e1000: TX control programming mismatch "
            "TCTL=0x%08x expected-mask=0x%08x\n",
            tctl_readback,
            required_tctl);

        ret = -EIO;

        goto out_stop_and_release;
    }


    /*
     * Descriptor memory is DMA-coherent, but software still needs an
     * ordering point before transferring descriptor ownership to hardware.
     *
     * dma_wmb():
     *     orders coherent DMA-memory writes for the device.
     *
     * wmb():
     *     prevents the MMIO tail doorbell from becoming visible before
     *     the descriptor writes that precede it.
     */
    dma_wmb();
    wmb();


    /*
     * Submit descriptor zero.
     *
     * With TDH == 0, advancing TDT from 0 to 1 makes descriptor zero
     * available to hardware.
     */
    iowrite32(
        1,
        dev->bar0 + E1000_REG_TDT);


    /*
     * Flush the posted tail write so the device observes the submission.
     */
    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    /*
     * Deterministic bring-up validation waits for hardware write-back.
     *
     * READ_ONCE prevents the compiler from reusing a stale descriptor
     * status value while the device updates coherent memory asynchronously.
     */
    deadline =
        jiffies +
        msecs_to_jiffies(
            SK_E1000_TX_TIMEOUT_MS);


    do {

        status =
            READ_ONCE(
                tx_desc->status);

        if (status & SK_E1000_TXD_STAT_DD)
            break;


        usleep_range(
            1000,
            2000);

    } while (time_before(
                 jiffies,
                 deadline));


    if (!(status & SK_E1000_TXD_STAT_DD)) {

        pr_err(
            "sk_e1000: deterministic TX timed out waiting for DD "
            "status=0x%02x\n",
            status);

        ret = -ETIMEDOUT;

        goto out_stop_and_release;
    }


    /*
     * After observing DD, order any later CPU reads after the device's
     * completion write-back.
     */
    dma_rmb();


    tdh =
        ioread32(
            dev->bar0 + E1000_REG_TDH);

    tdt =
        ioread32(
            dev->bar0 + E1000_REG_TDT);


    if (tdh != 1 || tdt != 1) {

        pr_err(
            "sk_e1000: deterministic TX head/tail completion mismatch "
            "TDH=%u TDT=%u\n",
            tdh,
            tdt);

        ret = -EIO;

        goto out_stop_and_release;
    }


    pr_info(
        "sk_e1000: deterministic TX descriptor completed "
        "status=0x%02x TDH=%u TDT=%u\n",
        status,
        tdh,
        tdt);


    ret = 0;


out_stop_and_release:

    /*
     * Do not unmap/free a DMA_TO_DEVICE packet buffer until hardware can no
     * longer consume it.
     *
     * If MMIO engine quiesce cannot be verified, disable PCI bus mastering
     * before releasing the streaming mapping. The validation still fails,
     * but the fallback prevents the device from initiating further DMA
     * transactions into memory that is about to be unmapped/freed.
     */
    stop_ret =
        sk_e1000_disable_packet_engines(
            dev);


    if (stop_ret) {

        pci_clear_master(
            dev->pdev);


        if (ret == 0)
            ret = stop_ret;
    }


    if (ret == 0) {

        pr_info(
            "sk_e1000: deterministic TX packet DMA validation PASSED "
            "length=%zu bytes\n",
            dev->tx_test_len);
    }


    sk_e1000_release_tx_test_buffer(
        dev);


    return ret;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC RX PACKET-BUFFER RELEASE
 * --------------------------------------------------------------------------
 *
 * RX packet data uses a streaming DMA mapping with DMA_FROM_DEVICE.
 *
 * The receive engine must no longer be able to write this buffer before the
 * mapping is released. The caller establishes that invariant before invoking
 * this helper from active RX paths. Error/remove paths also stop both packet
 * engines first.
 */
static void
sk_e1000_release_rx_test_buffer(
    struct sk_e1000_device *dev)
{
    if (!dev)
        return;


    if (dev->rx_test_mapped) {

        dma_unmap_single(
            &dev->pdev->dev,
            dev->rx_test_dma_addr,
            dev->rx_test_buffer_size,
            DMA_FROM_DEVICE);

        dev->rx_test_mapped = false;
    }


    dev->rx_test_dma_addr =
        (dma_addr_t)0;

    dev->rx_test_buffer_size = 0;


    kfree(
        dev->rx_test_cpu_addr);

    dev->rx_test_cpu_addr = NULL;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC RX PACKET-BUFFER PREPARATION
 * --------------------------------------------------------------------------
 *
 * Allocate one 2048-byte receive buffer and map it for device writes.
 *
 * 2048 bytes matches the legacy Intel receive-buffer size selected by the
 * validation RCTL programming below. The externally injected frame is only
 * 60 bytes, so it fits entirely in one descriptor/buffer.
 */
static int
sk_e1000_prepare_rx_test_buffer(
    struct sk_e1000_device *dev)
{
    dma_addr_t dma_addr;


    if (!dev || !dev->pdev)
        return -EINVAL;


    if (dev->rx_test_cpu_addr ||
        dev->rx_test_mapped ||
        dev->rx_test_buffer_size != 0) {

        return -EBUSY;
    }


    dev->rx_test_cpu_addr =
        kmalloc(
            SK_E1000_RX_TEST_BUFFER_SIZE,
            GFP_KERNEL);


    if (!dev->rx_test_cpu_addr)
        return -ENOMEM;


    /*
     * Initialize the CPU-owned buffer before mapping it to the device.
     * Software does not touch these bytes again until DMA ownership has
     * returned to the CPU.
     */
    memset(
        dev->rx_test_cpu_addr,
        0xcc,
        SK_E1000_RX_TEST_BUFFER_SIZE);


    dma_addr =
        dma_map_single(
            &dev->pdev->dev,
            dev->rx_test_cpu_addr,
            SK_E1000_RX_TEST_BUFFER_SIZE,
            DMA_FROM_DEVICE);


    if (dma_mapping_error(
            &dev->pdev->dev,
            dma_addr)) {

        pr_err(
            "sk_e1000: deterministic RX DMA mapping failed\n");

        kfree(
            dev->rx_test_cpu_addr);

        dev->rx_test_cpu_addr = NULL;

        return -EIO;
    }


    dev->rx_test_dma_addr = dma_addr;
    dev->rx_test_buffer_size = SK_E1000_RX_TEST_BUFFER_SIZE;
    dev->rx_test_mapped = true;


    pr_info(
        "sk_e1000: deterministic RX buffer mapped "
        "size=%zu DMA=%pad\n",
        dev->rx_test_buffer_size,
        &dev->rx_test_dma_addr);


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC EXTERNAL RX FRAME VALIDATION
 * --------------------------------------------------------------------------
 *
 * Hardware-independent byte matching lives in sk_e1000_frame.c so the exact
 * RX frame contract can be exercised by user-space Unity tests.
 *
 * The host injector remains independently implemented in Python. This kernel
 * wrapper maps the shared validation result to Linux errno values and keeps
 * the detailed runtime diagnostics used by the integration evidence.
 */
static int
sk_e1000_validate_rx_test_frame(
    const u8 *frame,
    size_t length)
{
    enum sk_e1000_rx_test_frame_status frame_status;

    size_t mismatch_offset = 0U;

    unsigned char expected_byte = 0U;


    frame_status =
        sk_e1000_check_rx_test_frame(
            frame,
            length,
            &mismatch_offset,
            &expected_byte);


    switch (frame_status) {

    case SK_E1000_RX_TEST_FRAME_VALID:

        return 0;


    case SK_E1000_RX_TEST_FRAME_NULL:

        return -EINVAL;


    case SK_E1000_RX_TEST_FRAME_LENGTH_MISMATCH:

        pr_err(
            "sk_e1000: deterministic RX frame length mismatch "
            "actual=%zu expected=%u\n",
            length,
            SK_E1000_RX_TEST_FRAME_LEN);

        return -EIO;


    case SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH:

        /*
         * The shared checker reports a content mismatch only after proving
         * length == SK_E1000_RX_TEST_FRAME_LEN, so mismatch_offset is within
         * the caller-owned frame.
         */
        pr_err(
            "sk_e1000: deterministic RX byte mismatch "
            "offset=%zu actual=0x%02x expected=0x%02x\n",
            mismatch_offset,
            frame[mismatch_offset],
            expected_byte);

        return -EIO;


    default:

        /*
         * Defensive guard in case the shared status enum is extended later
         * without updating this kernel-specific mapping.
         */
        pr_err(
            "sk_e1000: deterministic RX validator returned "
            "unexpected status=%d\n",
            (int)frame_status);

        return -EIO;
    }
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC ONE-DESCRIPTOR RX VALIDATION
 * --------------------------------------------------------------------------
 *
 * This is an opt-in bring-up validation path for the project's QEMU e1000
 * target. It is deliberately synchronous so runtime evidence is simple and
 * reproducible; a production net_device driver would receive asynchronously.
 *
 * Hardware/software flow:
 *
 *     host Python builds external 60-byte frame
 *          ->
 *     TAP / QEMU network backend
 *          ->
 *     QEMU Intel 82540EM receive engine
 *          ->
 *     RX descriptor[0]
 *          ->
 *     DMA_FROM_DEVICE packet buffer
 *          ->
 *     DD + EOP descriptor write-back
 *          ->
 *     CPU regains DMA ownership
 *          ->
 *     exact byte-for-byte comparison
 *
 * RDT points one descriptor beyond the final descriptor hardware may
 * process. With RDH=0 and RDT=1, only descriptor zero is available.
 */
static int
sk_e1000_run_deterministic_rx_test(
    struct sk_e1000_device *dev)
{
    struct sk_e1000_rx_desc *rx_desc;

    unsigned long deadline;

    u32 rctl;
    u32 rctl_readback;
    u32 rdh;
    u32 rdt;

    u16 rx_length;

    u8 status;
    u8 errors;

    int ret;
    int stop_ret;


    if (!dev ||
        !dev->bar0 ||
        !dev->rx_desc_ring.cpu_addr ||
        dev->rx_desc_ring.size < sizeof(*rx_desc)) {

        return -EINVAL;
    }


    ret =
        sk_e1000_disable_packet_engines(
            dev);

    if (ret)
        return ret;


    ret =
        sk_e1000_prepare_rx_test_buffer(
            dev);

    if (ret)
        return ret;


    rx_desc =
        (struct sk_e1000_rx_desc *)
            dev->rx_desc_ring.cpu_addr;


    /*
     * Descriptor zero is the only buffer exposed to hardware.
     *
     * The receive buffer uses a streaming DMA_FROM_DEVICE mapping while the
     * descriptor itself resides in the coherent ring allocation.
     */
    memset(
        rx_desc,
        0,
        sizeof(*rx_desc));

    rx_desc->buffer_addr =
        cpu_to_le64(
            (u64)dev->rx_test_dma_addr);


    /*
     * Publish the descriptor before transferring queue ownership.
     */
    dma_wmb();
    wmb();


    /*
     * Intel defines RDT as one descriptor beyond the last valid descriptor.
     * Therefore RDH=0/RDT=1 exposes exactly descriptor zero.
     *
     * These writes occur while RCTL.EN is clear.
     */
    iowrite32(
        0,
        dev->bar0 + E1000_REG_RDH);

    iowrite32(
        1,
        dev->bar0 + E1000_REG_RDT);


    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    rdh =
        ioread32(
            dev->bar0 + E1000_REG_RDH);

    rdt =
        ioread32(
            dev->bar0 + E1000_REG_RDT);


    if (rdh != 0 || rdt != 1) {

        pr_err(
            "sk_e1000: RX head/tail initialization failed "
            "RDH=%u RDT=%u\n",
            rdh,
            rdt);

        ret = -EIO;

        goto out_stop_and_release;
    }


    /*
     * Deterministic validation RCTL policy:
     *
     * EN:
     *     enable receiver
     *
     * UPE:
     *     accept unicast traffic without depending on receive-address
     *     register programming that this educational milestone does not yet
     *     own. The received destination MAC is still checked byte-for-byte.
     *
     * SECRC:
     *     strip the Ethernet FCS before DMA so the descriptor length and CPU
     *     buffer contain the exact 60 bytes generated by the host injector.
     *
     * All receive-buffer-size bits remain zero, selecting the legacy 2048
     * byte buffer size used by SK_E1000_RX_TEST_BUFFER_SIZE.
     */
    rctl =
        E1000_RCTL_EN |
        E1000_RCTL_UPE |
        E1000_RCTL_SECRC;


    iowrite32(
        rctl,
        dev->bar0 + E1000_REG_RCTL);


    (void)ioread32(
        dev->bar0 + E1000_REG_STATUS);


    rctl_readback =
        ioread32(
            dev->bar0 + E1000_REG_RCTL);


    if ((rctl_readback &
         (E1000_RCTL_EN |
          E1000_RCTL_UPE |
          E1000_RCTL_SECRC)) != rctl) {

        pr_err(
            "sk_e1000: RX control programming mismatch "
            "RCTL=0x%08x expected=0x%08x\n",
            rctl_readback,
            rctl);

        ret = -EIO;

        goto out_stop_and_release;
    }


    pr_info(
        "sk_e1000: deterministic RX armed "
        "RDH=0 RDT=1 buffer=%zu bytes; "
        "waiting up to %u ms for external frame\n",
        dev->rx_test_buffer_size,
        SK_E1000_RX_TIMEOUT_MS);


    deadline =
        jiffies +
        msecs_to_jiffies(
            SK_E1000_RX_TIMEOUT_MS);


    status = 0;


    do {

        status =
            READ_ONCE(
                rx_desc->status);

        if (status & SK_E1000_RXD_STAT_DD)
            break;


        usleep_range(
            1000,
            2000);

    } while (time_before(
                 jiffies,
                 deadline));


    if (!(status & SK_E1000_RXD_STAT_DD)) {

        pr_err(
            "sk_e1000: deterministic RX timed out waiting for DD "
            "status=0x%02x\n",
            status);

        ret = -ETIMEDOUT;

        goto out_stop_and_release;
    }


    /*
     * DD is the device ownership hand-back for descriptor state. Order all
     * later descriptor reads after the observed completion byte.
     */
    dma_rmb();


    status =
        READ_ONCE(
            rx_desc->status);

    rx_length =
        le16_to_cpu(
            rx_desc->length);

    errors =
        READ_ONCE(
            rx_desc->errors);


    if ((status &
         (SK_E1000_RXD_STAT_DD |
          SK_E1000_RXD_STAT_EOP)) !=
        (SK_E1000_RXD_STAT_DD |
         SK_E1000_RXD_STAT_EOP)) {

        pr_err(
            "sk_e1000: deterministic RX descriptor missing DD/EOP "
            "status=0x%02x\n",
            status);

        ret = -EIO;

        goto out_stop_and_release;
    }


    if (errors != 0) {

        pr_err(
            "sk_e1000: deterministic RX descriptor reported errors "
            "errors=0x%02x\n",
            errors);

        ret = -EIO;

        goto out_stop_and_release;
    }


    if (rx_length != SK_E1000_RX_TEST_FRAME_LEN) {

        pr_err(
            "sk_e1000: deterministic RX descriptor length mismatch "
            "length=%u expected=%u\n",
            rx_length,
            SK_E1000_RX_TEST_FRAME_LEN);

        ret = -EIO;

        goto out_stop_and_release;
    }


    rdh =
        ioread32(
            dev->bar0 + E1000_REG_RDH);

    rdt =
        ioread32(
            dev->bar0 + E1000_REG_RDT);


    if (rdh != 1 || rdt != 1) {

        pr_err(
            "sk_e1000: deterministic RX head/tail completion mismatch "
            "RDH=%u RDT=%u\n",
            rdh,
            rdt);

        ret = -EIO;

        goto out_stop_and_release;
    }


    /*
     * Stop the receiver before returning ownership of the streaming packet
     * mapping to the CPU.
     */
    stop_ret =
        sk_e1000_disable_packet_engines(
            dev);


    if (stop_ret) {

        /*
         * The validation must fail, but the streaming DMA mapping still
         * needs a safe ownership boundary before release.
         */
        pci_clear_master(
            dev->pdev);

        ret = stop_ret;

        goto out_release;
    }


    /*
     * dma_unmap_single(DMA_FROM_DEVICE) completes the streaming DMA ownership
     * transfer back to the CPU. Only after this point may software inspect
     * the bytes written by the NIC.
     */
    dma_unmap_single(
        &dev->pdev->dev,
        dev->rx_test_dma_addr,
        dev->rx_test_buffer_size,
        DMA_FROM_DEVICE);

    dev->rx_test_mapped = false;
    dev->rx_test_dma_addr = (dma_addr_t)0;


    ret =
        sk_e1000_validate_rx_test_frame(
            dev->rx_test_cpu_addr,
            rx_length);


    if (ret)
        goto out_release;


    pr_info(
        "sk_e1000: deterministic RX descriptor completed "
        "status=0x%02x length=%u RDH=%u RDT=%u\n",
        status,
        rx_length,
        rdh,
        rdt);

    pr_info(
        "sk_e1000: deterministic RX packet DMA validation PASSED "
        "length=%u bytes\n",
        rx_length);


    ret = 0;


out_release:

    sk_e1000_release_rx_test_buffer(
        dev);

    return ret;


out_stop_and_release:

    stop_ret =
        sk_e1000_disable_packet_engines(
            dev);


    if (stop_ret) {

        /*
         * Fall back to disabling PCI bus mastering before releasing the RX
         * streaming mapping if packet-engine quiesce could not be verified.
         */
        pci_clear_master(
            dev->pdev);


        if (ret == 0)
            ret = stop_ret;
    }


    sk_e1000_release_rx_test_buffer(
        dev);


    return ret;
}


/*
 * --------------------------------------------------------------------------
 * INTERRUPT SERVICE ROUTINE
 * --------------------------------------------------------------------------
 *
 * ISR = Interrupt Service Routine.
 *
 * Legacy INTx interrupts may be shared by multiple PCI devices.
 * The handler therefore reads ICR before claiming the interrupt.
 *
 * Hardware access stays in this file. Hardware-independent cause
 * interpretation is delegated to sk_e1000_logic.c and is independently
 * exercised through Unity unit tests.
 */

static irqreturn_t sk_e1000_irq_handler(int irq, void *data)
{
    struct sk_e1000_device *dev = data;
    u32 cause;

    (void)irq;


    /*
     * Read and acknowledge the Interrupt Cause Register.
     */
    cause =
        ioread32(
            dev->bar0 + E1000_REG_ICR);


    /*
     * A zero cause means the shared interrupt did not originate from
     * this controller.
     */
    if (!sk_e1000_irq_is_pending(cause))
        return IRQ_NONE;


    dev->last_icr = cause;


    pr_info(
        "sk_e1000: interrupt received ICR=0x%08x\n",
        cause);


    /*
     * Current bring-up validation specifically exercises LSC.
     *
     * LSC = Link Status Change.
     */
    if (sk_e1000_irq_has_lsc(cause)) {

        pr_info(
            "sk_e1000: LSC interrupt handled\n");


        /*
         * Wake probe() after the interrupt successfully travels:
         *
         *     device
         *        ->
         *     PCI INTx
         *        ->
         *     Linux interrupt subsystem
         *        ->
         *     this ISR
         */
        complete(
            &dev->irq_test_done);
    }


    return IRQ_HANDLED;
}


/*
 * --------------------------------------------------------------------------
 * PCI PROBE
 * --------------------------------------------------------------------------
 *
 * Resource acquisition currently proceeds as:
 *
 *     PCI device enable
 *          ->
 *     PCI bus mastering
 *          ->
 *     DMA mask negotiation
 *          ->
 *     BAR0 ownership
 *          ->
 *     driver-private memory
 *          ->
 *     BAR0 MMIO mapping
 *          ->
 *     RX descriptor-ring DMA allocation
 *          ->
 *     TX descriptor-ring DMA allocation
 *          ->
 *     descriptor-ring base/length programming
 *          ->
 *     IRQ registration
 *
 * Failure handling releases only successfully acquired resources,
 * generally in reverse acquisition order.
 */

static int sk_e1000_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id)
{
    struct sk_e1000_device *dev;

    resource_size_t bar_start;
    resource_size_t bar_len;

    unsigned long completed;

    unsigned int dma_bits;

    u32 ctrl;
    u32 status;

    int ret;


    /*
     * The PCI core already used this entry to match the hardware.
     */
    (void)id;


    /*
     * ------------------------------------------------------------------
     * PCI MATCH
     * ------------------------------------------------------------------
     */

    pr_info(
        "sk_e1000: matched PCI device %04x:%04x\n",
        pdev->vendor,
        pdev->device);


    /*
     * ------------------------------------------------------------------
     * ENABLE PCI MEMORY RESOURCES
     * ------------------------------------------------------------------
     */

    ret =
        pci_enable_device_mem(
            pdev);


    if (ret) {

        pr_err(
            "sk_e1000: pci_enable_device_mem failed: %d\n",
            ret);

        return ret;
    }


    /*
     * ------------------------------------------------------------------
     * VERIFY BAR0 TYPE
     * ------------------------------------------------------------------
     */

    if (!(pci_resource_flags(pdev, SK_E1000_BAR) &
          IORESOURCE_MEM)) {

        pr_err(
            "sk_e1000: BAR0 is not an MMIO resource\n");

        ret = -ENODEV;

        goto err_disable_device;
    }


    /*
     * ------------------------------------------------------------------
     * ENABLE PCI BUS MASTERING
     * ------------------------------------------------------------------
     *
     * Bus mastering allows the NIC to initiate transactions toward
     * system memory.
     *
     * This capability is required before the device can participate
     * in the RX/TX DMA datapath.
     */

    pci_set_master(
        pdev);


    pr_info(
        "sk_e1000: PCI bus mastering enabled\n");


    /*
     * ------------------------------------------------------------------
     * CONFIGURE DMA ADDRESSING
     * ------------------------------------------------------------------
     *
     * The Linux DMA API determines which device-visible address range
     * can safely be used.
     *
     * sk_e1000_dma_configure() attempts:
     *
     *     64-bit
     *        ->
     *     32-bit fallback
     *
     * Initialization must stop if neither mode can be configured.
     */

    ret =
        sk_e1000_dma_configure(
            pdev,
            &dma_bits);


    if (ret) {

        pr_err(
            "sk_e1000: DMA addressing configuration failed: %d\n",
            ret);

        goto err_clear_master;
    }


    pr_info(
        "sk_e1000: DMA addressing configured: %u-bit\n",
        dma_bits);


    /*
     * ------------------------------------------------------------------
     * ENABLE LEGACY PCI INTx
     * ------------------------------------------------------------------
     */

    pci_intx(
        pdev,
        1);


    /*
     * ------------------------------------------------------------------
     * DISCOVER BAR0
     * ------------------------------------------------------------------
     *
     * Runtime BAR values come from Linux PCI resource discovery.
     */

    bar_start =
        pci_resource_start(
            pdev,
            SK_E1000_BAR);

    bar_len =
        pci_resource_len(
            pdev,
            SK_E1000_BAR);


    pr_info(
        "sk_e1000: BAR0 physical start=0x%llx\n",
        (unsigned long long)bar_start);

    pr_info(
        "sk_e1000: BAR0 size=%llu bytes\n",
        (unsigned long long)bar_len);


    /*
     * ------------------------------------------------------------------
     * CLAIM BAR0
     * ------------------------------------------------------------------
     */

    ret =
        pci_request_region(
            pdev,
            SK_E1000_BAR,
            "sk_e1000");


    if (ret) {

        pr_err(
            "sk_e1000: failed to claim BAR0: %d\n",
            ret);

        goto err_clear_master;
    }


    /*
     * ------------------------------------------------------------------
     * ALLOCATE DRIVER PRIVATE STATE
     * ------------------------------------------------------------------
     */

    dev =
        kzalloc(
            sizeof(*dev),
            GFP_KERNEL);


    if (!dev) {

        ret = -ENOMEM;

        goto err_release_region;
    }


    dev->pdev = pdev;
    dev->dma_bits = dma_bits;


    /*
     * ------------------------------------------------------------------
     * MAP BAR0
     * ------------------------------------------------------------------
     *
     * pci_iomap() produces a kernel virtual mapping for device MMIO.
     */

    dev->bar0 =
        pci_iomap(
            pdev,
            SK_E1000_BAR,
            0);


    if (!dev->bar0) {

        pr_err(
            "sk_e1000: failed to map BAR0\n");

        ret = -ENOMEM;

        goto err_free_dev;
    }


    /*
     * ------------------------------------------------------------------
     * REAL MMIO REGISTER ACCESS
     * ------------------------------------------------------------------
     */

    ctrl =
        ioread32(
            dev->bar0 +
            E1000_REG_CTRL);

    status =
        ioread32(
            dev->bar0 +
            E1000_REG_STATUS);


    pr_info(
        "sk_e1000: CTRL   = 0x%08x\n",
        ctrl);

    pr_info(
        "sk_e1000: STATUS = 0x%08x\n",
        status);


    pr_info(
        "sk_e1000: PCI/MMIO initialization PASSED\n");


    /*
     * ------------------------------------------------------------------
     * ALLOCATE RX DESCRIPTOR RING
     * ------------------------------------------------------------------
     *
     * The RX ring is DMA-coherent because both CPU and NIC will access
     * descriptor contents asynchronously.
     *
     * SK_E1000_RING_SIZE currently represents:
     *
     *     64 descriptors * 16 bytes = 1024 bytes
     *
     * Only descriptor memory is allocated at this stage.
     *
     * The opt-in deterministic RX validation path allocates and maps its
     * packet buffer later, after descriptor-ring and interrupt bring-up.
     *
     * The descriptor-ring DMA base address and length are programmed
     * after both RX and TX ring allocations succeed.
     */

    ret =
        sk_e1000_dma_alloc(
            pdev,
            &dev->rx_desc_ring,
            SK_E1000_RING_SIZE);


    if (ret) {

        pr_err(
            "sk_e1000: RX descriptor-ring DMA allocation failed: %d\n",
            ret);

        goto err_unmap_bar;
    }


    pr_info(
        "sk_e1000: RX descriptor ring allocated count=%u size=%zu bytes\n",
        SK_E1000_RING_COUNT,
        dev->rx_desc_ring.size);


    /*
     * %pad is the Linux kernel formatter for dma_addr_t.
     *
     * The address is runtime evidence only. It must never be
     * hard-coded or treated as a CPU virtual address.
     */

    pr_info(
        "sk_e1000: RX descriptor ring DMA address=%pad\n",
        &dev->rx_desc_ring.dma_addr);


    /*
     * ------------------------------------------------------------------
     * ALLOCATE TX DESCRIPTOR RING
     * ------------------------------------------------------------------
     *
     * RX and TX are independent hardware queues and therefore own
     * separate coherent descriptor-ring allocations.
     *
     * The deterministic TX packet buffer is mapped later, after ring
     * addressing and interrupt-path bring-up have been validated.
     */

    ret =
        sk_e1000_dma_alloc(
            pdev,
            &dev->tx_desc_ring,
            SK_E1000_RING_SIZE);


    if (ret) {

        pr_err(
            "sk_e1000: TX descriptor-ring DMA allocation failed: %d\n",
            ret);

        goto err_free_rx_ring;
    }


    pr_info(
        "sk_e1000: TX descriptor ring allocated count=%u size=%zu bytes\n",
        SK_E1000_RING_COUNT,
        dev->tx_desc_ring.size);


    pr_info(
        "sk_e1000: TX descriptor ring DMA address=%pad\n",
        &dev->tx_desc_ring.dma_addr);


    pr_info(
        "sk_e1000: RX/TX descriptor-ring DMA allocation PASSED\n");


    /*
     * ------------------------------------------------------------------
     * PROGRAM DESCRIPTOR-RING BASE ADDRESSES AND LENGTHS
     * ------------------------------------------------------------------
     *
     * The NIC learns where the RX and TX descriptor arrays live in
     * DMA address space, but RX/TX packet engines remain disabled.
     *
     * Head/tail initialization is deferred to the deterministic TX/RX
     * validation routines, after packet-buffer ownership is established.
     */

    ret =
        sk_e1000_program_descriptor_ring_addressing(
            dev);


    if (ret) {

        pr_err(
            "sk_e1000: descriptor-ring register programming "
            "failed: %d\n",
            ret);

        goto err_free_tx_ring;
    }


    /*
     * ------------------------------------------------------------------
     * PREPARE INTERRUPT HARDWARE
     * ------------------------------------------------------------------
     */

    sk_e1000_disable_interrupts(
        dev);


    init_completion(
        &dev->irq_test_done);


    dev->last_icr = 0;


    /*
     * ------------------------------------------------------------------
     * REGISTER INTERRUPT HANDLER
     * ------------------------------------------------------------------
     */

    ret =
        request_irq(
            pdev->irq,
            sk_e1000_irq_handler,
            IRQF_SHARED,
            "sk_e1000",
            dev);


    if (ret) {

        pr_err(
            "sk_e1000: request_irq(%u) failed: %d\n",
            pdev->irq,
            ret);

        goto err_free_tx_ring;
    }


    pr_info(
        "sk_e1000: IRQ handler registered on IRQ %u\n",
        pdev->irq);


    /*
     * ------------------------------------------------------------------
     * ENABLE DETERMINISTIC TEST INTERRUPT
     * ------------------------------------------------------------------
     */

    iowrite32(
        SK_E1000_INT_LSC,
        dev->bar0 +
        E1000_REG_IMS);


    /*
     * Flush the posted interrupt-mask write.
     */

    (void)ioread32(
        dev->bar0 +
        E1000_REG_STATUS);


    /*
     * ------------------------------------------------------------------
     * TRIGGER INTERRUPT THROUGH HARDWARE MODEL
     * ------------------------------------------------------------------
     *
     * The ISR is not called directly.
     *
     *     CPU MMIO write to ICS
     *              |
     *              v
     *     e1000 hardware model
     *              |
     *              v
     *     PCI INTx assertion
     *              |
     *              v
     *     Linux interrupt subsystem
     *              |
     *              v
     *     sk_e1000_irq_handler()
     */

    pr_info(
        "sk_e1000: triggering LSC interrupt test\n");


    iowrite32(
        SK_E1000_INT_LSC,
        dev->bar0 +
        E1000_REG_ICS);


    /*
     * Flush the posted ICS write.
     */

    (void)ioread32(
        dev->bar0 +
        E1000_REG_STATUS);


    /*
     * ------------------------------------------------------------------
     * WAIT FOR ISR
     * ------------------------------------------------------------------
     *
     * This completion exists only for deterministic bring-up
     * validation.
     *
     * Future RX/TX packet processing remains asynchronous and will
     * not synchronously wait for each DMA operation.
     */

    completed =
        wait_for_completion_timeout(
            &dev->irq_test_done,
            msecs_to_jiffies(
                SK_E1000_IRQ_TIMEOUT_MS));


    if (!completed) {

        pr_err(
            "sk_e1000: interrupt test timed out\n");

        ret = -ETIMEDOUT;

        goto err_free_irq;
    }


    /*
     * Verify the expected interrupt cause.
     */

    if (!sk_e1000_irq_has_lsc(dev->last_icr)) {

        pr_err(
            "sk_e1000: unexpected interrupt cause 0x%08x\n",
            dev->last_icr);

        ret = -EIO;

        goto err_free_irq;
    }


    pr_info(
        "sk_e1000: interrupt test PASSED\n");


    /*
     * The deterministic interrupt test is complete.
     *
     * Keep normal interrupts masked until the RX/TX datapath is
     * initialized.
     */

    sk_e1000_disable_interrupts(
        dev);


    /*
     * ------------------------------------------------------------------
     * RUN DETERMINISTIC REAL TX DMA VALIDATION
     * ------------------------------------------------------------------
     *
     * This step moves real Ethernet bytes through the complete transmit
     * path:
     *
     *     CPU RAM
     *        ->
     *     streaming DMA mapping
     *        ->
     *     legacy TX descriptor
     *        ->
     *     TDH/TDT queue ownership
     *        ->
     *     QEMU e1000 DMA read
     *        ->
     *     descriptor DD write-back
     *
     * External PCAP verification is intentionally performed by the
     * integration environment, not by trusting this driver's log alone.
     */
    ret =
        sk_e1000_run_deterministic_tx_test(
            dev);


    if (ret) {

        pr_err(
            "sk_e1000: deterministic TX DMA validation failed: %d\n",
            ret);

        goto err_free_irq;
    }


    /*
     * ------------------------------------------------------------------
     * OPTIONAL DETERMINISTIC EXTERNAL RX DMA VALIDATION
     * ------------------------------------------------------------------
     *
     * This path is opt-in because it requires a host-side Ethernet frame
     * to be injected while probe() is waiting. Normal regression/module
     * loads therefore remain independent of external timing.
     */
    if (run_rx_test) {

        ret =
            sk_e1000_run_deterministic_rx_test(
                dev);


        if (ret) {

            pr_err(
                "sk_e1000: deterministic RX DMA validation failed: %d\n",
                ret);

            goto err_free_irq;
        }
    }


    /*
     * Publish driver-private state only after complete initialization.
     */

    pci_set_drvdata(
        pdev,
        dev);


    /*
     * Current successful initialization path:
     *
     * PCI match
     *      ->
     * PCI enable
     *      ->
     * bus mastering
     *      ->
     * DMA mask negotiation
     *      ->
     * BAR0 discovery / ownership
     *      ->
     * MMIO mapping
     *      ->
     * RX descriptor-ring DMA allocation
     *      ->
     * TX descriptor-ring DMA allocation
     *      ->
     * descriptor-ring base/length programming
     *      ->
     * IRQ registration
     *      ->
     * hardware interrupt
     *      ->
     * ISR validation
     *      ->
     * deterministic one-frame TX DMA validation
     *      ->
     * optional externally injected one-frame RX DMA validation
     */

    /*
     * Keep the established baseline log stable because the existing
     * regression suite keys off this exact message. Deterministic TX and
     * optional RX validation each have independent PASS logs above.
     */
    pr_info(
        "sk_e1000: PCI/MMIO/DMA/RING/IRQ initialization PASSED\n");


    return 0;


/*
 * --------------------------------------------------------------------------
 * ERROR UNWIND
 * --------------------------------------------------------------------------
 *
 * Each label represents an ownership boundary.
 *
 * Resources are released only if the corresponding acquisition step
 * succeeded.
 */


err_free_irq:

    /*
     * Stop device interrupt generation before removing the ISR.
     */

    sk_e1000_disable_interrupts(
        dev);


    free_irq(
        pdev->irq,
        dev);


    /*
     * IRQ teardown is complete.
     *
     * Fall through to the DMA teardown boundary below.
     */


err_free_tx_ring:

    /*
     * Descriptor-ring addressing may already have been programmed and a
     * deterministic packet test may have enabled a packet engine.
     *
     * First request a normal RX/TX engine stop. Because probe is already
     * failing, also clear PCI bus mastering before releasing any streaming
     * packet mapping or coherent descriptor-ring memory. This second
     * boundary prevents further device-initiated DMA even if MMIO engine
     * quiesce could not be verified.
     */
    (void)sk_e1000_disable_packet_engines(
        dev);

    pci_clear_master(
        pdev);


    sk_e1000_release_rx_test_buffer(
        dev);

    sk_e1000_release_tx_test_buffer(
        dev);


    /*
     * TX was allocated after RX, so descriptor-ring memory is released
     * in reverse allocation order.
     */
    sk_e1000_dma_free(
        pdev,
        &dev->tx_desc_ring);


err_free_rx_ring:

    sk_e1000_dma_free(
        pdev,
        &dev->rx_desc_ring);


err_unmap_bar:

    pci_iounmap(
        pdev,
        dev->bar0);


err_free_dev:

    kfree(
        dev);


err_release_region:

    pci_release_region(
        pdev,
        SK_E1000_BAR);


err_clear_master:

    pci_clear_master(
        pdev);


err_disable_device:

    pci_disable_device(
        pdev);


    return ret;
}


/*
 * --------------------------------------------------------------------------
 * DRIVER REMOVE
 * --------------------------------------------------------------------------
 *
 * Current dependency-safe teardown order:
 *
 *     mask NIC interrupts
 *          ->
 *     disable RX/TX packet engines
 *          ->
 *     clear PCI bus mastering
 *          ->
 *     free Linux IRQ
 *          ->
 *     unmap/free deterministic RX packet buffer if still owned
 *          ->
 *     unmap/free deterministic TX packet buffer if still owned
 *          ->
 *     free TX descriptor-ring DMA memory
 *          ->
 *     free RX descriptor-ring DMA memory
 *          ->
 *     unmap BAR0
 *          ->
 *     free private state
 *          ->
 *     release BAR0
 *          ->
 *     disable PCI device
 *
 * IRQ teardown occurs before DMA/MMIO teardown because the ISR already
 * accesses BAR0 and future packet-processing versions will also consume
 * descriptor state.
 */

static void sk_e1000_remove(struct pci_dev *pdev)
{
    struct sk_e1000_device *dev;


    dev =
        pci_get_drvdata(
            pdev);


    if (dev) {

        /*
         * Prevent new device interrupts before removing the handler.
         */

        sk_e1000_disable_interrupts(
            dev);


        /*
         * Deterministic packet validation enables RX/TX only for controlled
         * transactions. Request the normal stopped-engine invariant again
         * during remove().
         */
        (void)sk_e1000_disable_packet_engines(
            dev);


        /*
         * remove() is committed to teardown, so clear PCI bus mastering
         * before releasing any DMA-backed memory. This remains safe even if
         * packet-engine quiesce could not be verified through MMIO.
         */
        pci_clear_master(
            pdev);


        /*
         * The ISR accesses BAR0 and future versions will access descriptor
         * state. Remove it before destroying either resource.
         */

        free_irq(
            pdev->irq,
            dev);


        /*
         * Defensive cleanup for any deterministic RX/TX streaming mapping
         * still owned by this device.
         */
        sk_e1000_release_rx_test_buffer(
            dev);

        sk_e1000_release_tx_test_buffer(
            dev);


        /*
         * Release TX first because it was allocated after RX.
         *
         * DMA memory is released while the PCI device remains enabled
         * and associated with the same DMA API context used during
         * allocation.
         */

        sk_e1000_dma_free(
            pdev,
            &dev->tx_desc_ring);


        pr_info(
            "sk_e1000: TX descriptor ring DMA memory released\n");


        sk_e1000_dma_free(
            pdev,
            &dev->rx_desc_ring);


        pr_info(
            "sk_e1000: RX descriptor ring DMA memory released\n");


        /*
         * Remove the kernel virtual MMIO mapping.
         */

        if (dev->bar0) {

            pci_iounmap(
                pdev,
                dev->bar0);
        }


        /*
         * Prevent stale access through PCI driver data before releasing
         * the private structure.
         */

        pci_set_drvdata(
            pdev,
            NULL);


        /*
         * Release driver-private kernel memory.
         */

        kfree(
            dev);
    }


    /*
     * Return BAR0 ownership to the PCI subsystem.
     */

    pci_release_region(
        pdev,
        SK_E1000_BAR);


    /*
     * Disable PCI memory resources enabled during probe().
     */

    pci_disable_device(
        pdev);


    pr_info(
        "sk_e1000: device removed and resources released\n");
}


/*
 * --------------------------------------------------------------------------
 * LINUX PCI DRIVER REGISTRATION
 * --------------------------------------------------------------------------
 */

static struct pci_driver sk_e1000_driver = {
    .name       = "sk_e1000",
    .id_table   = sk_e1000_pci_ids,
    .probe      = sk_e1000_probe,
    .remove     = sk_e1000_remove,
};


/*
 * module_pci_driver() registers this driver with the Linux PCI core
 * during module initialization and unregisters it during module exit.
 */

module_pci_driver(sk_e1000_driver);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Santosh Kumar");
MODULE_DESCRIPTION(
    "Linux PCI network driver for QEMU Intel 82540EM");
