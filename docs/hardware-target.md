# Hardware Target

## Selected NIC

This project targets the QEMU `e1000` network device.

The QEMU `e1000` model emulates the Intel 82540EM Gigabit Ethernet Controller.

- Vendor: Intel
- PCI Vendor ID: `0x8086`
- PCI Device ID: `0x100E`
- QEMU device model: `e1000`

## Why This Device

The Intel 82540EM exposes the hardware mechanisms this project is designed to demonstrate:

- PCI device discovery
- BAR-based MMIO register access
- hardware interrupts
- PCI bus mastering
- DMA
- RX descriptor rings
- TX descriptor rings
- hardware head/tail registers
- Ethernet packet receive/transmit

It therefore provides a realistic networking device model for developing and testing a Linux PCI network driver in QEMU.

## Development Topology

The VM will use two network interfaces:

1. A Virtio NIC using the standard Linux driver for SSH and management.
2. A dedicated QEMU e1000 NIC controlled by this project driver.

Keeping the management NIC separate prevents driver development from breaking SSH connectivity.

## Hardware Contract

The driver will follow the Intel 8254x controller specification for:

- register offsets
- register bit definitions
- RX descriptor layout
- TX descriptor layout
- interrupt behavior
- DMA operation
- receive initialization
- transmit initialization

The driver will not invent its own device register or descriptor format.

## Validation

The hardware identity will be verified inside the VM using commands such as:

    lspci -nn
    lspci -vv -s <PCI_ADDRESS>
    lspci -k -s <PCI_ADDRESS>

Expected PCI identity for the project NIC:

    8086:100e

Implementation will begin only after the dedicated e1000 device is visible and verified in the VM.