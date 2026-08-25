/*
 * sk_e1000.c
 *
 * Linux PCIe network driver for the QEMU-emulated
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
 *   - legacy INTx interrupt registration
 *   - e1000 interrupt masking and cause handling
 *   - deterministic interrupt-path validation
 *   - shared hardware-independent interrupt decision logic
 *   - ordered error unwind and resource cleanup
 *
 * Future milestones:
 *
 *   - DMA addressing configuration
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
 * The driver intentionally discovers runtime PCI resources rather
 * than hard-coding physical BAR addresses or IRQ numbers.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/jiffies.h>

#include "sk_e1000_logic.h"


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

#define SK_E1000_VENDOR_ID          0x8086
#define SK_E1000_DEVICE_ID          0x100e


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
 * The driver does NOT hard-code the observed QEMU BAR address
 * (for example 0xfeb00000). That address is runtime platform state.
 */

#define SK_E1000_BAR                0


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

#define E1000_REG_CTRL              0x0000
#define E1000_REG_STATUS            0x0008


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
 *     We use this during bring-up to exercise the complete interrupt
 *     path through the QEMU e1000 hardware model.
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

#define E1000_REG_ICR               0x00c0
#define E1000_REG_ICS               0x00c8
#define E1000_REG_IMS               0x00d0
#define E1000_REG_IMC               0x00d8


/*
 * Maximum time probe() waits for the deterministic interrupt test.
 */

#define SK_E1000_IRQ_TIMEOUT_MS     1000


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
 *     Linux's representation of the PCI device.
 *
 *
 * bar0:
 *
 *     Kernel virtual address returned by pci_iomap().
 *
 *     This is NOT normal RAM.
 *
 *     It represents device MMIO and must be accessed with Linux
 *     MMIO accessors such as ioread32() and iowrite32().
 *
 *
 * irq_test_done:
 *
 *     Synchronization object used only for our deterministic
 *     interrupt bring-up test.
 *
 *     probe() triggers an interrupt through the hardware model.
 *     The ISR calls complete(), waking probe().
 *
 *
 * last_icr:
 *
 *     Stores the most recently observed Interrupt Cause Register
 *     value during the test.
 */

struct sk_e1000_device {
    struct pci_dev *pdev;
    void __iomem *bar0;

    struct completion irq_test_done;

    u32 last_icr;
};


/*
 * --------------------------------------------------------------------------
 * PCI DEVICE TABLE
 * --------------------------------------------------------------------------
 *
 * PCI_DEVICE() is a Linux kernel macro that fills the Vendor ID and
 * Device ID fields required for PCI matching.
 *
 * The final zero entry terminates the table.
 */

static const struct pci_device_id sk_e1000_pci_ids[] = {
    { PCI_DEVICE(SK_E1000_VENDOR_ID, SK_E1000_DEVICE_ID) },
    { 0, }
};


/*
 * Export the supported PCI IDs as module metadata.
 *
 * This allows the kernel/module tooling to associate this driver
 * with the supported hardware.
 */

MODULE_DEVICE_TABLE(pci, sk_e1000_pci_ids);


/*
 * --------------------------------------------------------------------------
 * INTERRUPT MASKING
 * --------------------------------------------------------------------------
 *
 * Stop this NIC from generating interrupt events.
 *
 * This is important during:
 *
 *     - initial bring-up
 *     - error cleanup
 *     - driver removal
 *
 * The interrupt source must be stopped before free_irq() removes the
 * handler, otherwise hardware could generate another interrupt while
 * the handler/resources are being torn down.
 */

static void sk_e1000_disable_interrupts(struct sk_e1000_device *dev)
{
    /*
     * Writing all 1 bits to IMC clears all interrupt-enable bits.
     */
    iowrite32(0xffffffff,
              dev->bar0 + E1000_REG_IMC);


    /*
     * PCI/MMIO writes may be posted.
     *
     * Read STATUS so the previous write reaches the device before
     * execution proceeds into cleanup.
     */
    (void)ioread32(dev->bar0 + E1000_REG_STATUS);


    /*
     * Clear any already-pending interrupt causes.
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
 * Linux invokes this function when the PCI interrupt line associated
 * with the device is asserted.
 *
 * This QEMU configuration uses legacy INTx.
 *
 * Legacy INTx interrupts may be shared by multiple PCI devices.
 * Therefore our handler must determine whether the interrupt actually
 * belongs to this NIC.
 *
 * Hardware access remains here in the kernel driver:
 *
 *     ioread32()
 *         ->
 *     raw ICR cause value
 *
 * Hardware-independent interpretation of that cause value is delegated
 * to sk_e1000_logic.c. The same logic is exercised directly by the
 * user-space unit tests.
 */

static irqreturn_t sk_e1000_irq_handler(int irq, void *data)
{
    struct sk_e1000_device *dev = data;
    u32 cause;


    /*
     * The current handler does not need the numeric Linux IRQ value.
     *
     * The private device pointer identifies the controller whose
     * registers must be examined.
     */
    (void)irq;


    /*
     * Read ICR = Interrupt Cause Read.
     *
     * This identifies which e1000 interrupt source fired.
     *
     * Reading ICR also acknowledges the causes returned by the
     * controller.
     */
    cause = ioread32(dev->bar0 + E1000_REG_ICR);


    /*
     * Shared interrupt rule:
     *
     * A zero cause means this e1000 device has no pending interrupt.
     *
     * sk_e1000_irq_is_pending() is hardware-independent logic shared
     * with the unit-test environment.
     */
    if (!sk_e1000_irq_is_pending(cause))
        return IRQ_NONE;


    dev->last_icr = cause;


    pr_info("sk_e1000: interrupt received ICR=0x%08x\n",
            cause);


    /*
     * Our bring-up test specifically triggers LSC.
     *
     * LSC = Link Status Change.
     *
     * More than one interrupt cause can exist simultaneously, so the
     * shared helper checks the LSC bit rather than comparing the entire
     * ICR value.
     */
    if (sk_e1000_irq_has_lsc(cause)) {

        pr_info("sk_e1000: LSC interrupt handled\n");


        /*
         * Signal probe() that the hardware interrupt successfully
         * travelled through:
         *
         *     CPU MMIO write
         *          ->
         *     e1000 interrupt logic
         *          ->
         *     PCI INTx
         *          ->
         *     Linux interrupt subsystem
         *          ->
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
 * Linux calls probe() after:
 *
 *     1. PCI enumeration discovers the device.
 *     2. Vendor/Device ID matches sk_e1000_pci_ids[].
 *     3. The device is available for this driver to bind.
 *
 *
 * Resource acquisition order:
 *
 *     PCI device enable
 *          ->
 *     PCI bus mastering
 *          ->
 *     BAR0 ownership
 *          ->
 *     driver private memory
 *          ->
 *     BAR0 MMIO mapping
 *          ->
 *     IRQ registration
 *
 *
 * Failure cleanup releases these resources in reverse order.
 */

static int sk_e1000_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id)
{
    struct sk_e1000_device *dev;

    resource_size_t bar_start;
    resource_size_t bar_len;

    unsigned long completed;

    u32 ctrl;
    u32 status;

    int ret;


    /*
     * The PCI core already used this table entry to perform the
     * match. The current implementation does not need additional
     * information from it.
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
     *
     * pci_enable_device_mem() is a Linux PCI API.
     *
     * It enables the PCI memory resources required for BAR-based
     * MMIO communication.
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
     *
     * BAR0 must represent memory space because the e1000 register
     * interface is accessed through MMIO.
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
     * Bus mastering allows the NIC to initiate PCI transactions
     * rather than only respond to CPU transactions.
     *
     * This is essential for DMA:
     *
     *     RX:
     *         NIC -> system RAM
     *
     *     TX:
     *         system RAM -> NIC
     *
     * No DMA buffers are configured in this milestone yet.
     *
     * We enable the PCI capability now because it is part of the
     * hardware initialization required by the upcoming DMA datapath.
     */

    pci_set_master(pdev);


    pr_info("sk_e1000: PCI bus mastering enabled\n");


    /*
     * ------------------------------------------------------------------
     * ENABLE LEGACY PCI INTx
     * ------------------------------------------------------------------
     *
     * The current QEMU device configuration uses legacy INTx.
     *
     * MSI/MSI-X support can be considered as a later extension.
     */

    pci_intx(pdev, 1);


    /*
     * ------------------------------------------------------------------
     * DISCOVER BAR0
     * ------------------------------------------------------------------
     *
     * Linux provides the physical resource values assigned during
     * PCI enumeration.
     *
     * Never hard-code the runtime BAR address.
     */

    bar_start =
        pci_resource_start(pdev,
                           SK_E1000_BAR);

    bar_len =
        pci_resource_len(pdev,
                         SK_E1000_BAR);


    pr_info("sk_e1000: BAR0 physical start=0x%llx\n",
            (unsigned long long)bar_start);

    pr_info("sk_e1000: BAR0 size=%llu bytes\n",
            (unsigned long long)bar_len);


    /*
     * ------------------------------------------------------------------
     * CLAIM BAR0
     * ------------------------------------------------------------------
     *
     * pci_request_region() establishes exclusive driver ownership
     * of the BAR resource.
     */

    ret = pci_request_region(pdev,
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
     *
     * kzalloc() allocates kernel memory and initializes it to zero.
     */

    dev = kzalloc(sizeof(*dev),
                  GFP_KERNEL);

    if (!dev) {

        ret = -ENOMEM;

        goto err_release_region;
    }


    dev->pdev = pdev;


    /*
     * ------------------------------------------------------------------
     * MAP BAR0
     * ------------------------------------------------------------------
     *
     * pci_iomap() maps the PCI BAR into kernel virtual address
     * space.
     *
     * dev->bar0 is a device-memory mapping, not a normal RAM
     * pointer.
     */

    dev->bar0 = pci_iomap(pdev,
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
     *
     * CTRL:
     *
     *     BAR0 + 0x0000
     *
     * STATUS:
     *
     *     BAR0 + 0x0008
     *
     * ioread32() must be used for MMIO rather than directly
     * dereferencing the address as normal memory.
     */

    ctrl =
        ioread32(dev->bar0 +
                 E1000_REG_CTRL);

    status =
        ioread32(dev->bar0 +
                 E1000_REG_STATUS);


    pr_info("sk_e1000: CTRL   = 0x%08x\n",
            ctrl);

    pr_info("sk_e1000: STATUS = 0x%08x\n",
            status);


    pr_info("sk_e1000: PCI/MMIO initialization PASSED\n");


    /*
     * ------------------------------------------------------------------
     * PREPARE INTERRUPT HARDWARE
     * ------------------------------------------------------------------
     *
     * Start with every interrupt source masked and remove stale
     * causes before registering our handler.
     */

    sk_e1000_disable_interrupts(dev);


    /*
     * Initialize completion synchronization used by the
     * deterministic interrupt test.
     */

    init_completion(&dev->irq_test_done);

    dev->last_icr = 0;


    /*
     * ------------------------------------------------------------------
     * REGISTER INTERRUPT HANDLER
     * ------------------------------------------------------------------
     *
     * request_irq() registers our ISR with the Linux interrupt
     * subsystem.
     *
     * IRQF_SHARED is required for legacy shared INTx interrupts.
     *
     * dev is:
     *
     *     - passed to the ISR
     *     - used as the unique dev_id for free_irq()
     */

    ret = request_irq(pdev->irq,
                      sk_e1000_irq_handler,
                      IRQF_SHARED,
                      "sk_e1000",
                      dev);

    if (ret) {

        pr_err("sk_e1000: request_irq(%u) failed: %d\n",
               pdev->irq,
               ret);

        goto err_unmap_bar;
    }


    pr_info("sk_e1000: IRQ handler registered on IRQ %u\n",
            pdev->irq);


    /*
     * ------------------------------------------------------------------
     * ENABLE THE TEST INTERRUPT
     * ------------------------------------------------------------------
     *
     * Enable only the LSC cause.
     */

    iowrite32(SK_E1000_INT_LSC,
              dev->bar0 +
              E1000_REG_IMS);


    /*
     * Flush the posted MMIO write.
     */

    (void)ioread32(dev->bar0 +
                   E1000_REG_STATUS);


    /*
     * ------------------------------------------------------------------
     * TRIGGER INTERRUPT THROUGH HARDWARE MODEL
     * ------------------------------------------------------------------
     *
     * Writing the LSC bit into ICS does NOT directly call our ISR.
     *
     * Instead:
     *
     *     CPU executes MMIO write
     *              |
     *              v
     *     QEMU e1000 receives register write
     *              |
     *              v
     *     e1000 sets interrupt cause
     *              |
     *              v
     *     PCI INTx asserted
     *              |
     *              v
     *     Linux interrupt subsystem
     *              |
     *              v
     *     sk_e1000_irq_handler()
     *
     * This provides deterministic validation of the real interrupt
     * delivery path exposed by the emulated NIC.
     */

    pr_info("sk_e1000: triggering LSC interrupt test\n");


    iowrite32(SK_E1000_INT_LSC,
              dev->bar0 +
              E1000_REG_ICS);


    /*
     * Flush the posted ICS write.
     */

    (void)ioread32(dev->bar0 +
                   E1000_REG_STATUS);


    /*
     * ------------------------------------------------------------------
     * WAIT FOR ISR
     * ------------------------------------------------------------------
     *
     * Interrupt handling is asynchronous.
     *
     * The CPU cannot assume the interrupt completed immediately
     * after the MMIO write.
     *
     * probe() waits for the ISR to signal irq_test_done.
     *
     * This completion is a bring-up validation mechanism. It is not
     * intended to serialize the future RX/TX DMA datapath.
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
     * Validate that the interrupt observed by our ISR contained
     * the expected LSC test cause.
     *
     * Use the same shared decision function exercised by our unit
     * tests rather than duplicating the bit-test logic here.
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
     * The deterministic bring-up test is finished.
     *
     * Mask interrupts again until normal RX/TX interrupt handling
     * is introduced.
     */

    sk_e1000_disable_interrupts(dev);


    /*
     * Associate our private state with the pci_dev only after the
     * initialization sequence has succeeded.
     *
     * remove() later retrieves it using pci_get_drvdata().
     */

    pci_set_drvdata(pdev,
                    dev);


    /*
     * Successful milestone:
     *
     * PCI enumeration
     *      ->
     * PCI ID match
     *      ->
     * PCI device enable
     *      ->
     * bus mastering
     *      ->
     * BAR0 discovery
     *      ->
     * BAR0 ownership
     *      ->
     * MMIO mapping
     *      ->
     * CTRL/STATUS reads
     *      ->
     * IRQ registration
     *      ->
     * hardware interrupt
     *      ->
     * shared cause interpretation
     *      ->
     * ISR execution
     */

    pr_info(
        "sk_e1000: PCI/MMIO/IRQ initialization PASSED\n");


    return 0;


/*
 * --------------------------------------------------------------------------
 * ERROR UNWIND
 * --------------------------------------------------------------------------
 *
 * Every label releases only resources acquired before reaching that
 * point.
 *
 * Cleanup proceeds in reverse acquisition order.
 */


err_free_irq:

    /*
     * Stop hardware interrupt generation before removing the ISR.
     */
    sk_e1000_disable_interrupts(dev);


    /*
     * The ISR accesses BAR0, so free_irq() must happen before BAR0
     * is unmapped.
     */
    free_irq(pdev->irq,
             dev);


err_unmap_bar:

    pci_iounmap(pdev,
                dev->bar0);


err_free_dev:

    kfree(dev);


err_release_region:

    pci_release_region(pdev,
                       SK_E1000_BAR);


err_clear_master:

    /*
     * Remove PCI bus-master capability because initialization
     * failed and this driver is releasing the hardware.
     */
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
 * Called when:
 *
 *     - the module is unloaded, or
 *     - the driver is explicitly unbound from the PCI device.
 *
 *
 * Resource cleanup order:
 *
 *     disable NIC interrupts
 *          ->
 *     free Linux IRQ
 *          ->
 *     unmap BAR0
 *          ->
 *     free private memory
 *          ->
 *     clear PCI bus mastering
 *          ->
 *     release BAR0
 *          ->
 *     disable PCI device
 */

static void sk_e1000_remove(struct pci_dev *pdev)
{
    struct sk_e1000_device *dev;


    dev = pci_get_drvdata(pdev);


    if (dev) {

        /*
         * Stop the device from generating new interrupts before
         * the Linux handler is removed.
         */
        sk_e1000_disable_interrupts(dev);


        /*
         * The ISR accesses BAR0.
         *
         * Therefore:
         *
         *     free_irq()
         *
         * must happen before:
         *
         *     pci_iounmap()
         */
        free_irq(pdev->irq,
                 dev);


        /*
         * Remove the kernel virtual MMIO mapping.
         */
        if (dev->bar0)
            pci_iounmap(pdev,
                        dev->bar0);


        /*
         * Release driver-private kernel memory.
         */
        kfree(dev);
    }


    /*
     * Disable the NIC's ability to initiate PCI transactions.
     */
    pci_clear_master(pdev);


    /*
     * Give BAR0 ownership back to the PCI subsystem.
     */
    pci_release_region(pdev,
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
 *
 * name:
 *
 *     Kernel-visible driver name.
 *
 * id_table:
 *
 *     PCI hardware supported by this module.
 *
 * probe:
 *
 *     Called when compatible hardware is bound.
 *
 * remove:
 *
 *     Called when the driver releases the device.
 */

static struct pci_driver sk_e1000_driver = {
    .name       = "sk_e1000",
    .id_table   = sk_e1000_pci_ids,
    .probe      = sk_e1000_probe,
    .remove     = sk_e1000_remove,
};


/*
 * module_pci_driver() is a Linux convenience macro.
 *
 * At module load it effectively registers sk_e1000_driver with the
 * PCI core.
 *
 * At module unload it unregisters the PCI driver.
 */

module_pci_driver(sk_e1000_driver);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Santosh Kumar");
MODULE_DESCRIPTION(
    "Linux PCIe network driver for QEMU Intel 82540EM");