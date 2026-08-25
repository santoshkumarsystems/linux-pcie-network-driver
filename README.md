# Linux PCIe Network Driver

A Linux kernel network-driver project for a QEMU-emulated Ethernet NIC, designed to demonstrate real hardware/software interaction through PCIe, MMIO, interrupts, DMA, descriptor rings, and the Linux networking subsystem.

## Use Case

Embedded Linux systems frequently communicate with high-performance peripherals through PCIe.

An Ethernet Network Interface Card (NIC) provides a practical systems-programming use case because one device combines:

- PCI device discovery
- Base Address Registers (BARs)
- Memory-Mapped I/O (MMIO)
- hardware interrupts
- Direct Memory Access (DMA)
- RX and TX descriptor rings
- producer / consumer queue management
- memory ordering
- Linux kernel resource management
- Linux networking integration

The goal is to build one complete driver that moves real Ethernet frames between Linux and a dedicated QEMU-emulated NIC.

## Why This Project

The same low-level mechanisms used by network drivers also appear in:

- storage controllers
- GPU and accelerator drivers
- FPGA devices
- cameras and imaging systems
- wireless controllers
- high-speed data acquisition hardware

The project therefore focuses on a broader systems problem:

**How does Linux safely and efficiently communicate with PCIe hardware?**

## Architecture

The QEMU virtual machine uses two NICs.

    QEMU Linux VM
         |
         +-----------------------------+
         |                             |
         |                             |
    Virtio NIC                    Project NIC
    Management                    Emulated Ethernet NIC
         |                             |
    Linux stock driver            Custom Linux driver
         |                             |
    SSH / management              PCI / MMIO
                                  Interrupts
                                  DMA
                                  RX ring
                                  TX ring
                                       |
                                       v
                                    Network

The management NIC remains controlled by the standard Linux driver so SSH access is not interrupted while the project NIC is controlled by the custom driver.

## Driver Initialization

The driver will follow this hardware initialization path:

    Linux PCI subsystem
            |
            v
    Vendor / Device ID match
            |
            v
         probe()
            |
            v
    Enable PCI device
            |
            v
    Discover and claim BAR
            |
            v
       Map MMIO
            |
            v
    Enable bus mastering
            |
            v
    Configure interrupts
            |
            v
    Allocate DMA resources
            |
            v
    Initialize RX/TX rings
            |
            v
    Register Linux network interface

## Receive Path

RX = Receive.

    Ethernet network
            |
            v
    NIC receives frame
            |
            v
    NIC selects RX descriptor
            |
            v
    DMA writes packet into RAM
            |
            v
    NIC marks descriptor complete
            |
            v
    Interrupt / receive processing
            |
            v
    Driver consumes descriptor
            |
            v
    Linux networking stack

## Transmit Path

TX = Transmit.

    Linux networking stack
            |
            v
    Driver receives packet
            |
            v
    Prepare TX descriptor
            |
            v
    DMA address + packet length
            |
            v
    Memory ordering
            |
            v
    Notify NIC / update TX tail
            |
            v
    NIC DMA-reads packet from RAM
            |
            v
    NIC transmits Ethernet frame
            |
            v
         Network

## Descriptor Rings

RX and TX use circular descriptor rings shared between the driver and NIC.

The driver must correctly manage:

- producer and consumer positions
- ring wraparound
- descriptor ownership
- DMA buffer lifetime
- full and empty conditions
- hardware completion
- memory ordering

A descriptor must not be reused while hardware still owns it.

## Linux Networking Boundary

The NIC driver is responsible for moving Ethernet frames between Linux and hardware.

Higher-level functionality remains in the Linux networking stack:

    Application
        |
    TCP / UDP
        |
    IPv4 / IPv6
        |
    Routing / NAT / firewall
        |
    Ethernet
        |
    Custom NIC driver
        |
    DMA descriptor rings
        |
       NIC

The project does not reimplement TCP/IP functionality already provided by Linux.

## Project Structure

    linux-pcie-network-driver/
    |
    +-- README.md
    +-- Makefile
    +-- .gitignore
    |
    +-- src/
    |   Driver implementation
    |
    +-- include/
    |   Driver headers and hardware definitions
    |
    +-- docs/
    |   Architecture and design documentation
    |
    +-- tests/
    |   +-- unit/
    |   |   Pure C unit tests
    |   |
    |   +-- integration/
    |       QEMU / kernel / networking tests
    |
    +-- scripts/
        Build and test automation

## Testing Strategy

The project uses multiple levels of testing.

### Unit Tests

Hardware-independent C logic will be tested separately from the kernel.

Planned coverage includes:

- producer / consumer ring behavior
- ring wraparound
- full / empty detection
- descriptor index calculations
- register bit manipulation
- endian helpers
- packet and buffer boundary helpers

### Kernel Tests

Kernel-side logic will be tested where practical, including:

- initialization state
- resource ownership
- error cleanup
- descriptor state transitions
- driver helper functions

KUnit may be used for kernel-specific tests.

### Integration Tests

QEMU integration tests will verify real driver behavior:

- PCI device detection
- custom driver binding
- BAR discovery
- MMIO access
- interrupt handling
- DMA initialization
- RX descriptor operation
- TX descriptor operation
- Linux interface creation
- packet receive
- packet transmit
- bidirectional network traffic

## Error Handling

Driver initialization acquires resources in stages:

    PCI enable
        |
    BAR ownership
        |
    MMIO mapping
        |
    DMA allocation
        |
    Interrupt registration
        |
    RX/TX initialization

If initialization fails, resources must be released in reverse order.

This prevents leaked PCI, MMIO, DMA, IRQ, and kernel resources.

## Design Principles

1. Follow the actual NIC hardware specification.
2. Do not invent register layouts or descriptor formats.
3. Keep hardware-specific definitions separate from reusable logic.
4. Make CPU and NIC ownership explicit.
5. Respect asynchronous DMA and interrupt behavior.
6. Use correct memory ordering before notifying hardware.
7. Design cleanup paths together with success paths.
8. Unit test hardware-independent logic.
9. Validate real hardware interaction through QEMU integration tests.
10. Make claims only after functionality has been implemented and demonstrated.

## Implementation Roadmap

### PCI / MMIO

- [x] PCI Vendor / Device ID matching
- [x] probe() and remove()
- [x] PCI device enablement
- [x] BAR discovery and ownership
- [x] MMIO mapping
- [x] register access

## Current Status

PCI/MMIO initialization is implemented and validated against the
QEMU-emulated Intel 82540EM (`8086:100e`).

Validated functionality:

- custom driver binding
- PCI probe/remove lifecycle
- BAR0 discovery and ownership
- BAR0 MMIO mapping
- Intel CTRL and STATUS register access
- clean driver unload and resource release

Observed test environment:

- BAR0 base: `0xfeb00000`
- BAR0 size: `128 KiB`
- CTRL: `0x40140240`
- STATUS: `0x80080783`

The next milestone adds PCI bus mastering, interrupt handling, and DMA
resources.

### Interrupts / DMA

- [ ] PCI bus mastering
- [ ] DMA addressing
- [ ] interrupt registration
- [ ] interrupt acknowledgement
- [ ] DMA-capable descriptor memory
- [ ] packet-buffer DMA mapping

### Receive

- [ ] RX descriptor ring
- [ ] RX DMA buffers
- [ ] producer / consumer management
- [ ] receive completion
- [ ] packet delivery to Linux

### Transmit

- [ ] TX descriptor ring
- [ ] TX DMA mapping
- [ ] producer / consumer management
- [ ] TX hardware notification
- [ ] transmit completion
- [ ] descriptor reclamation

### Linux Networking

- [ ] net_device registration
- [ ] interface open / close
- [ ] transmit callback
- [ ] receive delivery
- [ ] MAC configuration
- [ ] driver statistics

### Validation

- [ ] unit tests
- [ ] kernel tests
- [ ] QEMU integration tests
- [ ] RX validation
- [ ] TX validation
- [ ] bidirectional ping
- [ ] runtime statistics
- [ ] documented test evidence

## Environment

- Linux
- C
- Linux kernel modules
- GCC
- Make / Kbuild
- QEMU
- KVM
- PCI / PCIe
- MMIO
- DMA
- Ethernet networking

## Current Status

Project architecture and test strategy are defined.

Implementation begins with PCI device discovery, BAR ownership, and MMIO register access.

Roadmap items remain unchecked until they are implemented and validated.

## Scope

The project targets QEMU-emulated Ethernet hardware.

QEMU provides a reproducible environment while exercising real Linux mechanisms including:

- PCI enumeration
- kernel driver binding
- MMIO
- interrupts
- DMA
- descriptor rings
- Linux networking integration

Physical NIC validation is outside the initial project scope.

## Future Work

After the basic driver is operational, possible extensions include:

- NAPI receive processing
- interrupt moderation
- checksum offload
- scatter/gather DMA
- multiple RX/TX queues
- Receive Side Scaling
- performance benchmarking
- physical NIC validation

## License

A license will be selected before the first public release.