# Linux PCI Network Driver

A Linux kernel network-driver project for a QEMU-emulated Intel 82540EM Gigabit Ethernet Controller.

The project demonstrates low-level hardware/software interaction through:

- PCI device discovery
- Base Address Registers (BARs)
- Memory-Mapped I/O (MMIO)
- PCI bus mastering
- hardware interrupts
- Direct Memory Access (DMA)
- Intel RX/TX descriptor formats
- DMA-backed descriptor-ring memory
- producer / consumer ring management
- Receive (RX) and Transmit (TX) packet processing
- Linux networking integration

The current hardware target is the QEMU `e1000` device:

- Intel 82540EM Gigabit Ethernet Controller
- PCI Vendor ID: `0x8086`
- PCI Device ID: `0x100e`
- QEMU device model: `e1000`

The Intel 82540EM uses a conventional PCI system interface.

The Linux PCI driver mechanisms demonstrated here—resource discovery, MMIO, bus mastering, interrupts, DMA, ownership, ordering, and cleanup—are also fundamental to PCIe device drivers.

## Use Case

Embedded Linux systems frequently communicate with high-performance peripherals through PCI-family buses.

An Ethernet Network Interface Card (NIC) provides a practical systems-programming target because a single device combines:

- PCI device discovery
- Base Address Registers (BARs)
- Memory-Mapped I/O (MMIO)
- PCI bus mastering
- hardware interrupts
- Direct Memory Access (DMA)
- Receive (RX) and Transmit (TX) descriptor rings
- producer / consumer queue management
- memory ordering
- Linux kernel resource management
- Linux networking integration

The goal is to build one coherent driver that eventually moves Ethernet frames between Linux and a dedicated QEMU-emulated NIC while exposing the hardware/software contract clearly.

## Why This Project

The same low-level mechanisms used by network drivers also appear in:

- storage controllers
- GPU and accelerator drivers
- FPGA devices
- cameras and imaging systems
- wireless controllers
- high-speed data-acquisition hardware

The broader systems problem is:

**How does Linux safely and efficiently communicate with hardware?**

This project focuses on that boundary rather than reimplementing networking functionality already provided by the Linux kernel.

## Hardware / VM Architecture

The QEMU virtual machine uses two network devices.

```text
                         QEMU Linux VM
                 +--------------------------+
                 |                          |
                 |                          |
          Management NIC              Project NIC
              Virtio                   Intel e1000
                 |                       82540EM
                 |                          |
        Linux virtio driver          sk_e1000 driver
                 |                          |
          SSH / management             PCI resources
                                         BAR0 / MMIO
                                         bus mastering
                                         interrupts
                                         DMA addressing
                                             |
                           +-----------------+-----------------+
                           |                                   |
                           v                                   v
                  RX descriptor ring                  TX descriptor ring
                  64 x 16-byte descriptors            64 x 16-byte descriptors
                  1024-byte coherent DMA              1024-byte coherent DMA
                           |                                   |
                           +---------------+-------------------+
                                           |
                                           v
                                 Hardware ring registers
                                      NOT YET PROGRAMMED
                                           |
                                           v
                                     Packet buffers
                                      NOT YET MAPPED
                                           |
                                           v
                                  Linux networking path
                                      NOT YET WIRED
```

The Virtio NIC remains controlled by the standard Linux driver so management and SSH connectivity are not interrupted while the custom driver takes ownership of the dedicated Intel 82540EM device.

## Current Driver Initialization

The currently implemented initialization path is:

```text
Linux PCI subsystem
        |
        v
PCI Vendor / Device ID match
        |
        v
     probe()
        |
        v
Enable PCI memory resources
        |
        v
Validate BAR0 as MMIO
        |
        v
Enable PCI bus mastering
        |
        v
Configure DMA addressing
        |
        +--> try 64-bit DMA
        |
        +--> fall back to 32-bit DMA if required
        |
        v
Discover BAR0
        |
        v
Claim BAR0 ownership
        |
        v
Allocate driver-private state
        |
        v
Map BAR0 into kernel virtual address space
        |
        v
Read CTRL and STATUS through MMIO
        |
        v
Allocate RX descriptor-ring coherent DMA memory
        |
        +--> 64 descriptors
        +--> 16 bytes per descriptor
        +--> 1024 bytes total
        |
        v
Allocate TX descriptor-ring coherent DMA memory
        |
        +--> 64 descriptors
        +--> 16 bytes per descriptor
        +--> 1024 bytes total
        |
        v
Mask / clear stale interrupt causes
        |
        v
Register legacy INTx handler
        |
        v
Enable Link Status Change interrupt
        |
        v
Trigger deterministic interrupt through ICS
        |
        v
PCI INTx delivery
        |
        v
Linux interrupt subsystem
        |
        v
sk_e1000 interrupt service routine
        |
        v
Read / acknowledge ICR
        |
        v
Validate Link Status Change cause
        |
        v
PCI/MMIO/DMA/IRQ initialization PASS
```

The current milestone establishes real RX and TX descriptor memory, but the NIC is not yet consuming those rings.

The driver does **not** yet program the RX/TX descriptor-ring base, length, head, or tail registers, does not map packet buffers, and does not claim packet DMA functionality.

## MMIO

MMIO = Memory-Mapped I/O.

The driver discovers BAR0 dynamically through the Linux PCI subsystem and maps it using the kernel PCI/MMIO APIs.

The physical BAR address is never hard-coded.

Current register access includes:

- `CTRL` — Device Control Register
- `STATUS` — Device Status Register
- `ICR` — Interrupt Cause Read
- `ICS` — Interrupt Cause Set
- `IMS` — Interrupt Mask Set
- `IMC` — Interrupt Mask Clear

Device registers are accessed with Linux MMIO accessors such as:

```c
ioread32()
iowrite32()
```

rather than normal memory dereferences.

RX/TX descriptor-ring registers are a future hardware-programming milestone.

## DMA and Descriptor-Ring Foundation

DMA = Direct Memory Access.

DMA allows a device to access system memory without requiring the CPU to copy every byte between the device and RAM.

Before using DMA, the driver must establish which DMA addresses the platform can safely provide to the device.

The current driver performs:

```text
Enable PCI bus mastering
        |
        v
Request 64-bit DMA addressing
        |
        +---- success ----> use 64-bit DMA
        |
        v
Request 32-bit DMA addressing
        |
        +---- success ----> use 32-bit DMA
        |
        v
Initialization failure
```

The implementation uses the Linux DMA API:

```c
dma_set_mask_and_coherent()
dma_alloc_coherent()
dma_free_coherent()
```

The driver never assumes that a CPU virtual address can be directly supplied to the NIC.

Each coherent allocation has two address views:

```text
                  one coherent allocation
                           |
              +------------+------------+
              |                         |
              v                         v
         CPU address               DMA address
          cpu_addr                  dma_addr
              |                         |
              |                         |
          CPU access                 NIC access
```

The current driver allocates two independent coherent DMA regions:

```text
RX descriptor ring
    64 descriptors x 16 bytes
    = 1024 bytes coherent DMA

TX descriptor ring
    64 descriptors x 16 bytes
    = 1024 bytes coherent DMA
```

Each ring is owned for the lifetime of the driver instance and released during dependency-safe teardown.

This milestone establishes:

- DMA mask negotiation
- coherent DMA allocation
- explicit CPU/device address separation
- independent RX and TX descriptor memory
- DMA ownership
- DMA lifetime tracking
- DMA cleanup

It does **not** yet establish:

- RX/TX ring-register programming
- RX packet-buffer DMA mappings
- TX packet-buffer DMA mappings
- actual packet receive DMA
- actual packet transmit DMA

## Intel Descriptor Formats

The project now defines the Intel 82540EM legacy receive and transmit descriptor layouts in `include/sk_e1000_desc.h`.

Current descriptor contract:

```text
Legacy RX descriptor size: 16 bytes
Legacy TX descriptor size: 16 bytes
Descriptor count per ring:  64
Ring size:                  1024 bytes
```

Compile-time assertions verify that the C structures remain 16 bytes and that the selected ring size satisfies the configured ring-length granularity.

The descriptor definitions are present and compiled into the kernel module, but descriptor contents are not yet connected to packet buffers or handed to hardware.

## Descriptor-Ring Management Logic

Hardware-independent circular-ring logic is implemented in:

```text
include/sk_e1000_ring.h
src/sk_e1000_ring.c
```

The current logic covers:

- advancing to the next descriptor index
- wraparound
- empty detection
- full detection
- one-slot guard semantics
- used-descriptor accounting
- free-descriptor accounting
- defensive handling of invalid indexes and small ring sizes

The implementation uses producer / consumer terminology.

```text
producer
   |
   | software advances as work is queued
   v
+-----+-----+-----+-----+-----+
|  0  |  1  |  2  |  3  | ... |
+-----+-----+-----+-----+-----+
   ^
   |
consumer
```

This logic is compiled into the production kernel module and is also tested directly in user space through Unity.

It is **not yet wired to the Intel RX/TX hardware head/tail registers**. That integration is a separate milestone because hardware ownership rules must be introduced deliberately rather than inferred from generic circular-queue behavior.

## Interrupt Handling

The current QEMU configuration uses legacy PCI INTx interrupts.

The driver:

1. masks device interrupt sources before handler registration
2. registers a shared Linux interrupt handler
3. enables the Link Status Change (LSC) interrupt cause
4. writes the LSC bit through the device Interrupt Cause Set register
5. allows the QEMU e1000 hardware model to assert PCI INTx
6. receives the interrupt through the Linux interrupt subsystem
7. reads the Interrupt Cause Register (ICR)
8. verifies the interrupt belongs to this device
9. acknowledges the reported cause
10. validates that LSC was observed

The deterministic validation path is:

```text
CPU
 |
 | MMIO write to ICS
 v
QEMU Intel e1000 hardware model
 |
 | interrupt cause asserted
 v
PCI INTx
 |
 v
Linux interrupt subsystem
 |
 v
sk_e1000 ISR
 |
 | read ICR
 | ICR = 0x00000004
 v
LSC detected
 |
 v
Interrupt validation PASS
```

ISR = Interrupt Service Routine.

The completion object currently used during `probe()` is a bring-up validation mechanism.

It is not intended to serialize the future RX/TX packet datapath.

## Shared Production Logic

Hardware-independent logic is separated from kernel/MMIO/DMA operations so the same production implementation can be compiled into the kernel module and exercised directly by user-space tests.

Current shared logic is organized as:

```text
Interrupt logic

include/sk_e1000_logic.h
          |
          v
src/sk_e1000_logic.c
      /           \
     /             \
    v               v
kernel module    Unity tests


Ring logic

include/sk_e1000_ring.h
          |
          v
src/sk_e1000_ring.c
      /           \
     /             \
    v               v
kernel module    Unity tests
```

Current shared production logic covers:

- whether an Interrupt Cause Register value represents a pending interrupt
- whether Link Status Change is present
- correct handling of multiple simultaneous interrupt-cause bits
- circular-ring index advancement
- wraparound
- full / empty detection
- used / free descriptor accounting

This avoids duplicating production logic inside the test suite.

## Planned Receive Path

RX = Receive.

```text
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
interrupt / receive processing
        |
        v
driver consumes descriptor
        |
        v
Linux networking stack
```

The RX descriptor format and coherent descriptor-ring memory now exist.

The remaining RX datapath—packet buffers, hardware register programming, descriptor ownership transitions, receive completion, and Linux packet delivery—is not yet implemented.

## Planned Transmit Path

TX = Transmit.

```text
Linux networking stack
        |
        v
driver receives packet
        |
        v
prepare TX descriptor
        |
        v
DMA address + packet length
        |
        v
memory ordering
        |
        v
update hardware TX tail
        |
        v
NIC DMA-reads packet from RAM
        |
        v
NIC transmits Ethernet frame
        |
        v
Network
```

The TX descriptor format and coherent descriptor-ring memory now exist.

The remaining TX datapath—packet mapping, hardware register programming, descriptor submission, completion, and reclamation—is not yet implemented.

## Linux Networking Boundary

The NIC driver is responsible for moving Ethernet frames between Linux and the hardware.

Higher-level networking functionality remains in the Linux networking stack.

```text
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
```

The project does not reimplement TCP/IP functionality already provided by Linux.

## Project Structure

```text
linux-pcie-network-driver/
|
+-- README.md
+-- Makefile
+-- .gitignore
|
+-- src/
|   +-- sk_e1000.c
|   +-- sk_e1000_logic.c
|   +-- sk_e1000_dma.c
|   +-- sk_e1000_ring.c
|
+-- include/
|   +-- sk_e1000_logic.h
|   +-- sk_e1000_dma.h
|   +-- sk_e1000_desc.h
|   +-- sk_e1000_ring.h
|
+-- docs/
|   +-- hardware-target.md
|   |
|   +-- evidence/
|       +-- pci-mmio-integration.txt
|       +-- pci-mmio-dmesg.txt
|       +-- pci-mmio-irq-integration.txt
|       +-- pci-mmio-irq-dmesg.txt
|       +-- pci-mmio-dma-irq-integration.txt
|       +-- pci-mmio-dma-irq-dmesg.txt
|       +-- descriptor-ring-integration.txt
|       +-- descriptor-ring-dmesg.txt
|       +-- irq-logic-unit-tests.txt
|
+-- tests/
|   +-- unit/
|   |   +-- test_irq_logic.c
|   |   +-- test_ring_logic.c
|   |
|   +-- integration/
|       +-- test_pci_mmio_dma_irq.sh
|
+-- third_party/
|   +-- unity/
|       +-- LICENSE.txt
|       +-- src/
|           +-- unity.c
|           +-- unity.h
|           +-- unity_internals.h
|
+-- scripts/
```

Generated kernel-module and user-space unit-test artifacts are excluded from source control.

## Testing Strategy

The project separates hardware-independent logic from hardware-dependent validation.

### Unit Tests

User-space unit tests use the Unity C test framework.

The tests compile the same production implementations contained in:

```text
src/sk_e1000_logic.c
src/sk_e1000_ring.c
```

No PCI device, MMIO implementation, interrupt controller, DMA subsystem, or QEMU hardware is mocked.

Hardware-dependent behavior is validated through integration testing against Linux and the QEMU e1000 device.

Current interrupt-logic coverage includes:

- zero interrupt cause
- Link Status Change interrupt cause
- unrelated interrupt cause
- multiple simultaneous causes
- LSC absent
- exact LSC match
- unrelated cause not mistaken for LSC
- LSC detected when multiple causes are present
- LSC detected across the full 32-bit cause mask

Current ring-logic coverage includes:

- normal index advancement
- last-slot wraparound
- zero-sized ring handling
- empty / non-empty state
- full detection
- wraparound full detection
- one-slot guard capacity
- out-of-range producer / consumer handling
- used-descriptor accounting
- free-descriptor accounting
- full-ring free-space behavior

Current result:

```text
IRQ logic:
9 Tests
0 Failures
0 Ignored

Ring logic:
19 Tests
0 Failures
0 Ignored

Total:
28 Tests
0 Failures
0 Ignored
```

### Kernel-Specific Tests

Kernel-specific helper logic may use KUnit where doing so provides meaningful coverage.

KUnit will not be added simply to increase test count.

Hardware behavior that is better validated against the actual QEMU device remains an integration-test responsibility.

### Integration Tests

The integration suite operates against the QEMU-emulated Intel 82540EM and the real Linux PCI, DMA, MMIO, and interrupt subsystems.

Current automated validation includes:

- PCI device presence
- Vendor ID `0x8086`
- Device ID `0x100e`
- kernel module presence
- dedicated device ownership
- custom driver loading
- custom driver binding
- PCI ID match
- BAR0 discovery
- CTRL MMIO register access
- STATUS MMIO register access
- PCI/MMIO initialization
- PCI bus-master initialization
- PCI `BusMaster+` state
- DMA addressing configuration
- 64-bit or 32-bit DMA negotiation validation
- RX descriptor-ring geometry: 64 descriptors / 1024 bytes
- RX device-visible DMA address creation
- TX descriptor-ring geometry: 64 descriptors / 1024 bytes
- TX device-visible DMA address creation
- RX/TX descriptor-ring DMA initialization
- Linux IRQ handler registration
- e1000 LSC interrupt generation
- ISR execution
- expected `ICR=0x00000004`
- Link Status Change handling
- interrupt completion validation
- complete PCI/MMIO/DMA/IRQ initialization
- module unload
- TX descriptor-ring DMA release
- RX descriptor-ring DMA release
- driver cleanup
- IRQ handler release
- PCI device release
- PCI `BusMaster-` state after cleanup

Current result:

```text
32 integration checks
32 passed
0 failed
```

## Current Status

The driver currently implements and validates the PCI/MMIO/interrupt/DMA descriptor-ring foundation against a QEMU-emulated Intel 82540EM (`8086:100e`).

Implemented functionality:

- PCI Vendor / Device ID matching
- Linux `probe()` / `remove()` lifecycle
- PCI memory-resource enablement
- BAR0 type validation
- dynamic BAR0 discovery
- BAR0 ownership
- BAR0 MMIO mapping
- Intel CTRL register access
- Intel STATUS register access
- PCI bus mastering
- DMA addressing configuration
- 64-bit DMA negotiation with 32-bit fallback
- Intel legacy RX descriptor definition
- Intel legacy TX descriptor definition
- compile-time descriptor-size validation
- separate RX descriptor-ring coherent DMA allocation
- separate TX descriptor-ring coherent DMA allocation
- CPU virtual address and DMA address separation
- DMA ownership and lifetime tracking
- hardware-independent producer / consumer ring logic
- ring wraparound / full / empty / used / free accounting
- DMA cleanup
- legacy INTx registration
- interrupt masking
- interrupt cause detection
- interrupt acknowledgement
- deterministic hardware-model interrupt generation
- shared interrupt decision logic
- dependency-safe error unwind
- IRQ teardown
- RX/TX descriptor-ring teardown
- BAR/MMIO teardown
- PCI bus-master cleanup
- PCI resource release
- automated user-space unit testing
- automated QEMU integration testing
- committed runtime evidence

Not yet implemented:

- RX descriptor-ring register programming
- TX descriptor-ring register programming
- RX packet-buffer DMA mappings
- TX packet-buffer DMA mappings
- hardware RX/TX producer / consumer integration
- packet receive
- packet transmit
- Linux `net_device` integration

### Observed Test Environment

Current observed values from the QEMU development VM:

```text
PCI device:              0000:00:04.0
Vendor / Device:         8086:100e
BAR0 base:               0xfeb00000
BAR0 size:               128 KiB
Linux IRQ:               20
CTRL:                    0x40140240
STATUS:                  0x80080783
Test ICR:                0x00000004
DMA addressing:          64-bit
RX descriptor count:     64
RX descriptor size:      16 bytes
RX ring allocation:      1024 bytes
TX descriptor count:     64
TX descriptor size:      16 bytes
TX ring allocation:      1024 bytes
```

These values are runtime observations, not hard-coded driver assumptions.

DMA addresses are intentionally not documented as fixed values because they are dynamically assigned and may differ between runs.

## Runtime Evidence

Validation output is committed so implemented claims can be traced to actual test runs.

### Current Descriptor-Ring DMA Milestone

QEMU integration-test result:

- [`docs/evidence/descriptor-ring-integration.txt`](docs/evidence/descriptor-ring-integration.txt)

Kernel driver log:

- [`docs/evidence/descriptor-ring-dmesg.txt`](docs/evidence/descriptor-ring-dmesg.txt)

Integration-test source:

- [`tests/integration/test_pci_mmio_dma_irq.sh`](tests/integration/test_pci_mmio_dma_irq.sh)

Ring unit-test source:

- [`tests/unit/test_ring_logic.c`](tests/unit/test_ring_logic.c)

Interrupt unit-test source:

- [`tests/unit/test_irq_logic.c`](tests/unit/test_irq_logic.c)

### Previous PCI/MMIO/DMA/IRQ Milestone

The earlier generic DMA-foundation milestone remains available as development-history evidence:

- [`docs/evidence/pci-mmio-dma-irq-integration.txt`](docs/evidence/pci-mmio-dma-irq-integration.txt)
- [`docs/evidence/pci-mmio-dma-irq-dmesg.txt`](docs/evidence/pci-mmio-dma-irq-dmesg.txt)
- [`docs/evidence/irq-logic-unit-tests.txt`](docs/evidence/irq-logic-unit-tests.txt)

### Previous PCI/MMIO/IRQ Milestone

The earlier interrupt milestone remains available as development-history evidence:

- [`docs/evidence/pci-mmio-irq-integration.txt`](docs/evidence/pci-mmio-irq-integration.txt)
- [`docs/evidence/pci-mmio-irq-dmesg.txt`](docs/evidence/pci-mmio-irq-dmesg.txt)

### Previous PCI/MMIO Milestone

The original PCI/MMIO milestone remains available as development-history evidence:

- [`docs/evidence/pci-mmio-integration.txt`](docs/evidence/pci-mmio-integration.txt)
- [`docs/evidence/pci-mmio-dmesg.txt`](docs/evidence/pci-mmio-dmesg.txt)

## Error Handling and Resource Ownership

Driver initialization acquires hardware and kernel resources in stages.

Current acquisition sequence:

```text
PCI device enable
        |
        v
PCI bus-master enable
        |
        v
DMA mask configuration
        |
        v
BAR0 ownership
        |
        v
driver-private memory
        |
        v
BAR0 MMIO mapping
        |
        v
RX descriptor-ring coherent DMA allocation
        |
        v
TX descriptor-ring coherent DMA allocation
        |
        v
IRQ registration
```

Cleanup is dependency-safe and generally releases resources in reverse acquisition order.

Current remove path:

```text
mask device interrupts
        |
        v
free Linux IRQ
        |
        v
free TX descriptor-ring DMA memory
        |
        v
free RX descriptor-ring DMA memory
        |
        v
unmap BAR0
        |
        v
free driver-private state
        |
        v
clear PCI bus mastering
        |
        v
release BAR0
        |
        v
disable PCI device
```

The IRQ handler is released before DMA and MMIO resources are destroyed because the ISR already accesses BAR0 and future packet-processing versions will also consume descriptor state.

The TX ring is released before the RX ring because TX is allocated after RX.

The DMA allocations are released while the PCI device and its DMA API context are still valid.

Error paths follow explicit ownership boundaries:

```text
RX allocation failure
    -> unmap BAR0

TX allocation failure
    -> free RX ring
    -> unmap BAR0

IRQ registration / later probe failure
    -> free IRQ when registered
    -> free TX ring
    -> free RX ring
    -> unmap BAR0
```

Partially initialized devices therefore release only resources that were successfully acquired.

## Design Principles

1. Follow the actual NIC hardware specification.
2. Do not invent register layouts or descriptor formats.
3. Discover platform-assigned resources dynamically.
4. Keep hardware-specific operations separate from reusable pure logic.
5. Make CPU and NIC ownership explicit.
6. Keep CPU virtual addresses and device DMA addresses conceptually separate.
7. Treat DMA and interrupt execution as asynchronous.
8. Use correct memory ordering before notifying hardware.
9. Design cleanup paths together with initialization paths.
10. Use dependency-safe cleanup, generally in reverse acquisition order.
11. Unit test hardware-independent production logic.
12. Validate hardware-dependent behavior through QEMU integration tests.
13. Preserve runtime evidence for validated milestones.
14. Make implementation claims only after the corresponding behavior has been demonstrated.

## Implementation Roadmap

### PCI / MMIO

- [x] PCI Vendor / Device ID matching
- [x] `probe()` and `remove()`
- [x] PCI device enablement
- [x] BAR0 validation
- [x] BAR0 discovery and ownership
- [x] MMIO mapping
- [x] CTRL register access
- [x] STATUS register access

### Bus Mastering / Interrupts

- [x] PCI bus mastering
- [x] legacy INTx registration
- [x] interrupt masking
- [x] interrupt cause detection
- [x] interrupt acknowledgement
- [x] shared INTx ownership handling
- [x] deterministic hardware interrupt validation
- [x] IRQ teardown
- [x] PCI bus-master cleanup

### DMA

- [x] DMA addressing configuration
- [x] DMA mask negotiation
- [x] coherent DMA allocation helper
- [x] independent RX descriptor-ring DMA allocation
- [x] independent TX descriptor-ring DMA allocation
- [x] DMA ownership and lifetime management
- [x] DMA cleanup paths
- [ ] RX packet-buffer DMA mapping
- [ ] TX packet-buffer DMA mapping

### Descriptor Rings

- [x] Intel legacy RX descriptor definition
- [x] Intel legacy TX descriptor definition
- [x] compile-time descriptor-size validation
- [x] fixed descriptor-ring geometry
- [x] producer / consumer ring helpers
- [x] ring wraparound logic
- [x] full / empty detection
- [x] used / free descriptor accounting
- [x] ring-logic unit tests
- [x] coherent RX descriptor-ring memory
- [x] coherent TX descriptor-ring memory
- [ ] program RX ring base / length / head / tail registers
- [ ] program TX ring base / length / head / tail registers
- [ ] connect software ring state to hardware ownership

### Receive

- [ ] RX packet buffers
- [ ] RX buffer DMA mapping
- [ ] RX hardware configuration
- [ ] RX descriptor ownership transitions
- [ ] receive completion
- [ ] descriptor replenishment
- [ ] packet delivery to Linux

### Transmit

- [ ] TX packet DMA mapping
- [ ] TX hardware configuration
- [ ] TX descriptor submission
- [ ] memory ordering before hardware notification
- [ ] TX hardware notification
- [ ] transmit completion
- [ ] descriptor reclamation

### Linux Networking

- [ ] `net_device` registration
- [ ] interface open / close
- [ ] transmit callback
- [ ] receive delivery
- [ ] MAC configuration
- [ ] driver statistics

### Validation

- [x] Unity unit-test framework
- [x] interrupt-logic unit tests
- [x] descriptor-ring logic unit tests
- [x] automated PCI/MMIO integration tests
- [x] automated interrupt integration tests
- [x] automated DMA-foundation integration tests
- [x] automated descriptor-ring DMA integration tests
- [x] runtime evidence for PCI/MMIO milestone
- [x] runtime evidence for interrupt milestone
- [x] runtime evidence for DMA-foundation milestone
- [x] runtime evidence for descriptor-ring DMA milestone
- [ ] RX register-programming validation
- [ ] TX register-programming validation
- [ ] packet RX validation
- [ ] packet TX validation
- [ ] bidirectional network traffic
- [ ] runtime packet statistics

## Environment

Development and validation currently use:

- C
- Linux kernel modules
- Linux PCI subsystem
- Linux DMA API
- GCC
- Make / Kbuild
- QEMU
- KVM
- Intel 82540EM / QEMU `e1000`
- PCI
- MMIO
- PCI bus mastering
- legacy INTx
- coherent DMA memory
- Intel legacy RX/TX descriptors
- producer / consumer ring logic
- Unity C test framework

Future milestones add:

- RX/TX ring-register programming
- packet-buffer DMA mappings
- Linux `net_device`
- Linux networking integration

## Scope

The initial hardware target is the QEMU-emulated Intel 82540EM.

QEMU provides a reproducible development environment while exercising real Linux mechanisms including:

- PCI enumeration
- kernel driver matching and binding
- PCI resource ownership
- MMIO
- PCI bus mastering
- Linux DMA APIs
- coherent DMA allocation
- interrupts
- kernel resource cleanup

The peripheral itself is emulated.

The Linux driver, kernel APIs, PCI subsystem, DMA API, MMIO APIs, interrupt subsystem, resource ownership, error handling, and cleanup paths are real Linux mechanisms.

The project currently demonstrates real Linux allocation and lifetime management of RX/TX descriptor-ring memory, but it does **not** yet claim that the Intel 82540EM hardware is consuming those rings.

The following remain planned and are not yet claimed as implemented:

- RX descriptor-ring register programming
- TX descriptor-ring register programming
- packet-buffer DMA
- hardware descriptor ownership transitions
- packet receive / transmit
- Linux networking integration

Physical NIC validation is outside the initial project scope.

## Future Work

After the basic RX/TX driver is operational, potential extensions include:

- NAPI receive processing
- interrupt moderation
- Message Signaled Interrupts (MSI)
- checksum offload
- scatter/gather DMA
- multiple RX/TX queues
- Receive Side Scaling (RSS)
- performance benchmarking
- fault-injection testing
- physical NIC validation

## Third-Party Software

The unit-test suite vendors Unity v2.7.0 under `third_party/unity/`.

Unity is distributed under the MIT license.

Its upstream license text is preserved in:

- `third_party/unity/LICENSE.txt`

## License

The kernel module currently declares:

```c
MODULE_LICENSE("GPL");
```

A repository-level license file should be added before treating the project license as finalized.
