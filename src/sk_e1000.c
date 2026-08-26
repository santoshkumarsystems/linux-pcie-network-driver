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
 *   - DMA address-mask negotiation
 *   - DMA-coherent descriptor memory allocation
 *   - legacy INTx interrupt registration
 *   - e1000 interrupt masking and cause handling
 *   - deterministic interrupt-path validation
 *   - shared hardware-independent interrupt decision logic
 *   - ordered error unwind and resource cleanup
 *
 * Future milestones:
 *
 *   - Intel RX descriptor definition
 *   - Intel TX descriptor definition
 *   - DMA-backed RX descriptor ring
 *   - DMA-backed TX descriptor ring
 *   - producer / consumer management
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

#include "sk_e1000_logic.h"
#include "sk_e1000_dma.h"


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
 * allocates the physical address range, and exposes that information
 * through the PCI resource APIs.
 *
 * The driver does NOT hard-code the observed QEMU BAR address.
 */

#define SK_E1000_BAR                    0


/*
 * --------------------------------------------------------------------------
 * DMA FOUNDATION
 * --------------------------------------------------------------------------
 *
 * Allocate an initial coherent memory region that will become the
 * foundation for hardware descriptor storage.
 *
 * This milestone intentionally does NOT program the address into RX/TX
 * hardware registers yet.
 *
 * Therefore:
 *
 *     coherent DMA memory exists                 YES
 *     valid device DMA address exists            YES
 *     NIC packet DMA through descriptors         NOT YET
 *
 * 4096 bytes gives the project a real persistent DMA-coherent region
 * while descriptor formats and separate RX/TX rings are introduced in
 * the next milestone.
 */

#define SK_E1000_DESCRIPTOR_MEM_SIZE    4096U


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
 */

#define E1000_REG_CTRL                  0x0000
#define E1000_REG_STATUS                0x0008


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
 *     DMA addressing width successfully negotiated with the Linux DMA
 *     subsystem.
 *
 *     The driver first requests 64-bit DMA addressing and falls back
 *     to 32-bit addressing when necessary.
 *
 *
 * descriptor_mem:
 *
 *     Persistent DMA-coherent memory owned by this driver.
 *
 *     It contains two distinct address views:
 *
 *         cpu_addr
 *             used by the CPU
 *
 *         dma_addr
 *             used by the device
 *
 *     The DMA address will eventually be programmed into the e1000
 *     descriptor-ring registers.
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

    struct sk_e1000_dma_region descriptor_mem;

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
    iowrite32(0xffffffff,
              dev->bar0 + E1000_REG_IMC);


    /*
     * MMIO writes may be posted.
     *
     * Read STATUS so the previous write reaches the device before
     * execution proceeds.
     */
    (void)ioread32(dev->bar0 + E1000_REG_STATUS);


    /*
     * Clear already-pending interrupt causes.
     */
    (void)ioread32(dev->bar0 + E1000_REG_ICR);
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
    cause = ioread32(dev->bar0 + E1000_REG_ICR);


    /*
     * A zero cause means the shared interrupt did not originate from
     * this controller.
     */
    if (!sk_e1000_irq_is_pending(cause))
        return IRQ_NONE;


    dev->last_icr = cause;


    pr_info("sk_e1000: interrupt received ICR=0x%08x\n",
            cause);


    /*
     * Current bring-up validation specifically exercises LSC.
     *
     * LSC = Link Status Change.
     */
    if (sk_e1000_irq_has_lsc(cause)) {

        pr_info("sk_e1000: LSC interrupt handled\n");


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
        complete(&dev->irq_test_done);
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
 *     coherent DMA memory
 *          ->
 *     IRQ registration
 *
 * Failure handling releases owned resources in reverse order.
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

    pr_info("sk_e1000: matched PCI device %04x:%04x\n",
            pdev->vendor,
            pdev->device);


    /*
     * ------------------------------------------------------------------
     * ENABLE PCI MEMORY RESOURCES
     * ------------------------------------------------------------------
     */

    ret = pci_enable_device_mem(pdev);

    if (ret) {

        pr_err("sk_e1000: pci_enable_device_mem failed: %d\n",
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

        pr_err("sk_e1000: BAR0 is not an MMIO resource\n");

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
     * in the future RX/TX DMA datapath.
     */

    pci_set_master(pdev);


    pr_info("sk_e1000: PCI bus mastering enabled\n");


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

    pci_intx(pdev, 1);


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


    pr_info("sk_e1000: BAR0 physical start=0x%llx\n",
            (unsigned long long)bar_start);

    pr_info("sk_e1000: BAR0 size=%llu bytes\n",
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

        pr_err("sk_e1000: failed to claim BAR0: %d\n",
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

        pr_err("sk_e1000: failed to map BAR0\n");

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


    pr_info("sk_e1000: CTRL   = 0x%08x\n",
            ctrl);

    pr_info("sk_e1000: STATUS = 0x%08x\n",
            status);


    pr_info("sk_e1000: PCI/MMIO initialization PASSED\n");


    /*
     * ------------------------------------------------------------------
     * ALLOCATE DMA-COHERENT DESCRIPTOR MEMORY
     * ------------------------------------------------------------------
     *
     * The allocation persists for the lifetime of the driver.
     *
     * The CPU receives a normal kernel virtual address while the NIC
     * receives a separate DMA address generated by the Linux DMA API.
     *
     * The device address is NOT programmed into ring registers yet.
     * That occurs when real RX/TX descriptor rings are introduced.
     */

    ret =
        sk_e1000_dma_alloc(
            pdev,
            &dev->descriptor_mem,
            SK_E1000_DESCRIPTOR_MEM_SIZE);


    if (ret) {

        pr_err(
            "sk_e1000: coherent DMA allocation failed: %d\n",
            ret);

        goto err_unmap_bar;
    }


    pr_info(
        "sk_e1000: coherent DMA memory allocated size=%zu bytes\n",
        dev->descriptor_mem.size);


    /*
     * %pad is the Linux kernel formatter for dma_addr_t.
     *
     * We report the device-visible address as runtime evidence but
     * never assume or hard-code its numerical value.
     */

    pr_info(
        "sk_e1000: coherent DMA address=%pad\n",
        &dev->descriptor_mem.dma_addr);


    pr_info("sk_e1000: DMA foundation PASSED\n");


    /*
     * ------------------------------------------------------------------
     * PREPARE INTERRUPT HARDWARE
     * ------------------------------------------------------------------
     */

    sk_e1000_disable_interrupts(dev);


    init_completion(&dev->irq_test_done);

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

        pr_err("sk_e1000: request_irq(%u) failed: %d\n",
               pdev->irq,
               ret);

        goto err_free_dma;
    }


    pr_info("sk_e1000: IRQ handler registered on IRQ %u\n",
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

    pr_info("sk_e1000: triggering LSC interrupt test\n");


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
     * Future RX/TX packet processing will remain asynchronous and will
     * not wait synchronously for every DMA operation.
     */

    completed =
        wait_for_completion_timeout(
            &dev->irq_test_done,
            msecs_to_jiffies(
                SK_E1000_IRQ_TIMEOUT_MS));


    if (!completed) {

        pr_err("sk_e1000: interrupt test timed out\n");

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


    pr_info("sk_e1000: interrupt test PASSED\n");


    /*
     * The deterministic interrupt test is complete.
     *
     * Keep normal interrupts masked until RX/TX datapath
     * initialization is implemented.
     */

    sk_e1000_disable_interrupts(dev);


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
     * coherent DMA allocation
     *      ->
     * IRQ registration
     *      ->
     * hardware interrupt
     *      ->
     * ISR validation
     */

    pr_info(
        "sk_e1000: PCI/MMIO/DMA/IRQ initialization PASSED\n");


    return 0;


/*
 * --------------------------------------------------------------------------
 * ERROR UNWIND
 * --------------------------------------------------------------------------
 *
 * Resources are released in reverse acquisition order.
 */


err_free_irq:

    /*
     * Stop hardware interrupt generation before removing the ISR.
     */

    sk_e1000_disable_interrupts(dev);


    free_irq(
        pdev->irq,
        dev);


/*
 * The IRQ is now gone, so DMA memory can no longer become reachable
 * from future interrupt handling.
 */

err_free_dma:

    sk_e1000_dma_free(
        pdev,
        &dev->descriptor_mem);


err_unmap_bar:

    pci_iounmap(
        pdev,
        dev->bar0);


err_free_dev:

    kfree(dev);


err_release_region:

    pci_release_region(
        pdev,
        SK_E1000_BAR);


err_clear_master:

    pci_clear_master(pdev);


err_disable_device:

    pci_disable_device(pdev);


    return ret;
}


/*
 * --------------------------------------------------------------------------
 * DRIVER REMOVE
 * --------------------------------------------------------------------------
 *
 * Current teardown order:
 *
 *     mask NIC interrupts
 *          ->
 *     free Linux IRQ
 *          ->
 *     free coherent DMA memory
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
 * IRQ teardown occurs before DMA/MMIO teardown because the ISR may
 * eventually consume descriptor state and already accesses BAR0.
 */

static void sk_e1000_remove(struct pci_dev *pdev)
{
    struct sk_e1000_device *dev;


    dev = pci_get_drvdata(pdev);


    if (dev) {

        /*
         * Prevent new device interrupts before removing the handler.
         */

        sk_e1000_disable_interrupts(dev);


        /*
         * The ISR accesses BAR0 and future versions will access
         * descriptor state.
         *
         * Remove it before freeing either resource.
         */

        free_irq(
            pdev->irq,
            dev);


        /*
         * Release DMA-coherent memory while the PCI device is still
         * enabled and associated with the same DMA API context used
         * during allocation.
         */

        sk_e1000_dma_free(
            pdev,
            &dev->descriptor_mem);


        pr_info(
            "sk_e1000: coherent DMA memory released\n");


        /*
         * Remove the kernel virtual MMIO mapping.
         */

        if (dev->bar0) {

            pci_iounmap(
                pdev,
                dev->bar0);
        }


        /*
         * Release driver-private kernel memory.
         */

        kfree(dev);
    }


    /*
     * Disable the device's ability to initiate PCI transactions.
     */

    pci_clear_master(pdev);


    /*
     * Return BAR0 ownership to the PCI subsystem.
     */

    pci_release_region(
        pdev,
        SK_E1000_BAR);


    /*
     * Disable PCI memory resources enabled during probe().
     */

    pci_disable_device(pdev);


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