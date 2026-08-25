/*
 * sk_e1000.c
 *
 * Linux PCIe network driver for the QEMU-emulated
 * Intel 82540EM Gigabit Ethernet Controller.
 *
 * Author: Santosh Kumar
 *
 * Current milestone:
 *   - Match the Intel 82540EM PCI device
 *   - Enable the PCI device
 *   - Discover and claim BAR0
 *   - Map BAR0 as MMIO
 *   - Read real device CTRL and STATUS registers
 *   - Release all resources cleanly on driver removal
 *
 * Future milestones will extend this driver with:
 *   - PCI bus mastering
 *   - interrupts
 *   - DMA
 *   - RX/TX descriptor rings
 *   - Linux net_device integration
 *
 * Hardware target:
 *   Vendor ID : 0x8086
 *   Device ID : 0x100e
 *   Device    : Intel 82540EM Gigabit Ethernet Controller
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>


/*
 * Intel PCI identification.
 *
 * These values are matched by the Linux PCI subsystem against
 * the PCI configuration space of devices present in the system.
 */
#define SK_E1000_VENDOR_ID      0x8086
#define SK_E1000_DEVICE_ID      0x100e


/*
 * The Intel 82540EM exposes its memory-mapped hardware registers
 * through PCI BAR0.
 *
 * BAR = Base Address Register.
 *
 * Linux discovers the physical address assigned to this BAR.
 * The driver must never hard-code the runtime physical BAR address.
 */
#define SK_E1000_BAR            0


/*
 * Intel 82540EM MMIO register offsets.
 *
 * These offsets are relative to the beginning of BAR0.
 *
 * CTRL:
 *   Device Control Register.
 *
 * STATUS:
 *   Device Status Register.
 */
#define E1000_REG_CTRL          0x0000
#define E1000_REG_STATUS        0x0008


/*
 * Private per-device driver state.
 *
 * One instance is allocated when probe() successfully begins
 * managing a matching PCI device.
 *
 * pdev:
 *   Linux PCI representation of the hardware device.
 *
 * bar0:
 *   Kernel virtual address corresponding to the device's
 *   memory-mapped BAR0 register region.
 */
struct sk_e1000_device {
    struct pci_dev *pdev;
    void __iomem *bar0;
};


/*
 * PCI device table.
 *
 * PCI_DEVICE() is a Linux kernel macro that creates the matching
 * fields for the specified Vendor ID and Device ID.
 *
 * The terminating empty entry is required.
 */
static const struct pci_device_id sk_e1000_pci_ids[] = {
    { PCI_DEVICE(SK_E1000_VENDOR_ID, SK_E1000_DEVICE_ID) },
    { 0, }
};


/*
 * Export the supported PCI IDs so user-space tools and the kernel
 * module subsystem can associate this module with the hardware.
 */
MODULE_DEVICE_TABLE(pci, sk_e1000_pci_ids);


/*
 * sk_e1000_probe()
 *
 * Called by the Linux PCI subsystem when:
 *
 *   1. A PCI device exists.
 *   2. Its Vendor/Device ID matches sk_e1000_pci_ids[].
 *   3. The device is available for this driver to bind.
 *
 * This function establishes ownership of the device resources
 * required by the current milestone.
 */
static int sk_e1000_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id)
{
    struct sk_e1000_device *dev;

    resource_size_t bar_start;
    resource_size_t bar_len;

    u32 ctrl;
    u32 status;

    int ret;


    /*
     * The PCI core already performed the ID match before calling
     * probe(). Log the actual runtime identity for verification.
     */
    pr_info("sk_e1000: matched PCI device %04x:%04x\n",
            pdev->vendor,
            pdev->device);


    /*
     * We currently do not need information directly from the
     * matching table entry beyond the fact that the match occurred.
     */
    (void)id;


    /*
     * Enable the PCI device's memory resources.
     *
     * pci_enable_device_mem() is a Linux PCI API.
     *
     * We use the memory-specific version because this milestone
     * communicates with the controller through BAR0 MMIO.
     */
    ret = pci_enable_device_mem(pdev);
    if (ret) {
        pr_err("sk_e1000: pci_enable_device_mem failed: %d\n",
               ret);
        return ret;
    }


    /*
     * Verify that BAR0 actually represents a memory resource.
     *
     * IORESOURCE_MEM is a Linux resource flag identifying
     * memory-mapped PCI address space.
     */
    if (!(pci_resource_flags(pdev, SK_E1000_BAR) &
          IORESOURCE_MEM)) {

        pr_err("sk_e1000: BAR0 is not an MMIO resource\n");

        ret = -ENODEV;
        goto err_disable_device;
    }


    /*
     * Ask the PCI subsystem for the runtime physical address
     * and length assigned to BAR0.
     *
     * Example from our QEMU environment:
     *
     *   BAR0 start = 0xfeb00000
     *   BAR0 size  = 128 KiB
     *
     * Those addresses are runtime information and are deliberately
     * not hard-coded into the driver.
     */
    bar_start = pci_resource_start(pdev, SK_E1000_BAR);
    bar_len   = pci_resource_len(pdev, SK_E1000_BAR);

    pr_info("sk_e1000: BAR0 physical start=0x%llx\n",
            (unsigned long long)bar_start);

    pr_info("sk_e1000: BAR0 size=%llu bytes\n",
            (unsigned long long)bar_len);


    /*
     * Claim exclusive ownership of BAR0.
     *
     * pci_request_region() prevents another driver from
     * simultaneously claiming the same PCI BAR.
     */
    ret = pci_request_region(pdev,
                             SK_E1000_BAR,
                             "sk_e1000");

    if (ret) {
        pr_err("sk_e1000: failed to claim BAR0: %d\n",
               ret);

        goto err_disable_device;
    }


    /*
     * Allocate private state for this device.
     *
     * kzalloc() is a Linux kernel memory-allocation API.
     * It allocates memory and initializes it to zero.
     */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);

    if (!dev) {
        ret = -ENOMEM;
        goto err_release_region;
    }


    dev->pdev = pdev;


    /*
     * Map BAR0 into kernel virtual address space.
     *
     * The PCI BAR represents device MMIO, not normal system RAM.
     *
     * After pci_iomap() succeeds:
     *
     *   dev->bar0
     *
     * becomes the base kernel virtual address used with MMIO
     * accessors such as ioread32() and iowrite32().
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
     * Associate our private structure with the Linux pci_dev.
     *
     * remove() can later recover it using pci_get_drvdata().
     */
    pci_set_drvdata(pdev, dev);


    /*
     * Read actual Intel 82540EM registers.
     *
     * CTRL is located at BAR0 + 0x0000.
     *
     * STATUS is located at BAR0 + 0x0008.
     *
     * ioread32() is a Linux MMIO accessor. It must be used instead
     * of dereferencing the mapped device address as a normal C
     * pointer.
     */
    ctrl = ioread32(dev->bar0 + E1000_REG_CTRL);

    status = ioread32(dev->bar0 + E1000_REG_STATUS);


    pr_info("sk_e1000: CTRL   = 0x%08x\n",
            ctrl);

    pr_info("sk_e1000: STATUS = 0x%08x\n",
            status);


    /*
     * Reaching this point proves the first complete hardware path:
     *
     * PCI enumeration
     *      ->
     * PCI ID match
     *      ->
     * probe()
     *      ->
     * BAR discovery
     *      ->
     * BAR ownership
     *      ->
     * MMIO mapping
     *      ->
     * real NIC register access
     */
    pr_info("sk_e1000: PCI/MMIO initialization PASSED\n");


    return 0;


/*
 * Error unwind
 *
 * Each label releases only resources that were successfully
 * acquired before reaching that stage.
 *
 * Resources are released in reverse acquisition order.
 */

err_free_dev:

    kfree(dev);


err_release_region:

    pci_release_region(pdev,
                       SK_E1000_BAR);


err_disable_device:

    pci_disable_device(pdev);

    return ret;
}


/*
 * sk_e1000_remove()
 *
 * Called by the Linux PCI subsystem when this driver is detached
 * from the device or the module is unloaded.
 *
 * Resource cleanup occurs in reverse order from initialization:
 *
 *   MMIO mapping
 *       ->
 *   private memory
 *       ->
 *   BAR ownership
 *       ->
 *   PCI device enablement
 */
static void sk_e1000_remove(struct pci_dev *pdev)
{
    struct sk_e1000_device *dev;


    dev = pci_get_drvdata(pdev);


    if (dev) {

        /*
         * Remove the kernel virtual mapping for BAR0.
         */
        if (dev->bar0)
            pci_iounmap(pdev,
                        dev->bar0);


        /*
         * Release the driver's private state.
         */
        kfree(dev);
    }


    /*
     * Give BAR0 ownership back to the PCI subsystem.
     */
    pci_release_region(pdev,
                       SK_E1000_BAR);


    /*
     * Disable the PCI memory resources enabled during probe().
     */
    pci_disable_device(pdev);


    pr_info("sk_e1000: device removed and resources released\n");
}


/*
 * Linux PCI driver registration structure.
 *
 * name:
 *   Driver name visible to the kernel.
 *
 * id_table:
 *   Supported PCI Vendor/Device IDs.
 *
 * probe:
 *   Called when compatible hardware is bound.
 *
 * remove:
 *   Called when the driver releases the hardware.
 */
static struct pci_driver sk_e1000_driver = {
    .name       = "sk_e1000",
    .id_table   = sk_e1000_pci_ids,
    .probe      = sk_e1000_probe,
    .remove     = sk_e1000_remove,
};


/*
 * Linux convenience macro that provides module initialization
 * and cleanup code for a PCI driver.
 *
 * At module load:
 *     pci_register_driver()
 *
 * At module unload:
 *     pci_unregister_driver()
 */
module_pci_driver(sk_e1000_driver);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Santosh Kumar");
MODULE_DESCRIPTION(
    "Linux PCIe network driver for QEMU Intel 82540EM");