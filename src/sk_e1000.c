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
 * Future milestones:
 *
 *   - establish reset-safe RX/TX head/tail initialization
 *   - allocate and map RX packet buffers
 *   - map TX packet buffers
 *   - connect producer / consumer state to hardware rings
 *   - packet receive / transmit
 *   - Linux net_device integration
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

#include "sk_e1000_logic.h"
#include "sk_e1000_dma.h"
#include "sk_e1000_desc.h"


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
#define E1000_TCTL_EN                   0x00000002U


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
 * This milestone programs only RDBAL/RDBAH/RDLEN. RDH/RDT initialization
 * is intentionally deferred until the reset and receive-buffer ownership
 * sequence is established.
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
 * This milestone programs only TDBAL/TDBAH/TDLEN. TDH/TDT initialization
 * is intentionally deferred until the reset and transmit-buffer ownership
 * sequence is established.
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
 *     Packet buffers are NOT allocated yet.
 *
 *     During probe(), the DMA base addresses and ring lengths are
 *     programmed into the NIC RDBAL/RDBAH/RDLEN and
 *     TDBAL/TDBAH/TDLEN registers while both packet engines remain
 *     disabled.
 *
 *     Head/tail initialization is deliberately deferred until the
 *     reset and packet-buffer ownership sequence is implemented.
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
 * Intel documents RDH and TDH software writes as initialization operations
 * performed after reset and before enabling the corresponding packet engine.
 * Our current driver does not yet own a complete reset + packet-buffer
 * initialization sequence, so advancing to head/tail programming here would
 * make a stronger hardware claim than the implementation can safely support.
 *
 * What this milestone establishes:
 *
 *     RX DMA address  -> RDBAL / RDBAH
 *     RX ring bytes   -> RDLEN
 *
 *     TX DMA address  -> TDBAL / TDBAH
 *     TX ring bytes   -> TDLEN
 *
 * RCTL.EN and TCTL.EN remain clear throughout.
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
     * Only descriptor memory is allocated here.
     *
     * RX packet buffers are introduced in a later milestone.
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
     * TX packet mappings are not introduced yet.
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
     * Head/tail initialization is intentionally deferred.
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
     * TX was allocated after RX, so descriptor-ring memory is released
     * in reverse allocation order.
     */


err_free_tx_ring:

    /*
     * Descriptor-ring addressing may already have been programmed.
     * Keep both packet engines disabled before releasing DMA memory.
     *
     * The current milestone never enables either engine, so this is
     * defensive teardown that preserves the ownership invariant.
     */
    (void)sk_e1000_disable_packet_engines(
        dev);


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
 *     free Linux IRQ
 *          ->
 *     free TX descriptor-ring DMA memory
 *          ->
 *     free RX descriptor-ring DMA memory
 *          ->
 *     unmap BAR0
 *          ->
 *     free private state
 *          ->
 *     clear PCI bus mastering
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
         * RX/TX are not enabled by this milestone, but explicitly
         * enforce the invariant before descriptor DMA memory is freed.
         */
        (void)sk_e1000_disable_packet_engines(
            dev);


        /*
         * The ISR accesses BAR0 and future versions will access
         * descriptor state.
         *
         * Remove it before destroying either resource.
         */

        free_irq(
            pdev->irq,
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
     * Disable the device's ability to initiate PCI transactions.
     */

    pci_clear_master(
        pdev);


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