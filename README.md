# Linux PCI Network Driver

A Linux kernel network-driver project for a **QEMU-emulated Intel 82540EM Gigabit Ethernet Controller**.

The project is built incrementally around the hardware/software boundary:

- PCI device discovery and driver binding
- Base Address Registers (BARs)
- Memory-Mapped I/O (MMIO)
- PCI bus mastering
- legacy INTx interrupts
- Direct Memory Access (DMA)
- Intel legacy RX/TX descriptor formats
- coherent DMA descriptor rings
- descriptor-ring register programming
- producer / consumer ring logic
- streaming DMA packet buffers
- deterministic transmit and receive validation
- exact packet-byte verification
- dependency-safe teardown
- future Linux `net_device` integration

The current hardware target is the QEMU `e1000` device:

- Device: Intel 82540EM Gigabit Ethernet Controller
- PCI Vendor ID: `0x8086`
- PCI Device ID: `0x100e`
- QEMU device model: `e1000`

The Intel 82540EM uses a conventional PCI system interface.

The Linux mechanisms exercised here—PCI discovery, MMIO, bus mastering, interrupts, DMA, ownership, ordering, and cleanup—are also fundamental to PCIe device-driver development.

---

## Current Validated Checkpoint

The current checkpoint implements and validates:

- Linux PCI match / `probe()` / `remove()` lifecycle
- BAR0 discovery, ownership, and MMIO mapping
- PCI bus mastering
- 64-bit DMA negotiation with 32-bit fallback
- legacy shared INTx interrupt handling
- Intel 82540EM legacy RX/TX descriptor definitions
- separate 64-entry RX and TX coherent-DMA descriptor rings
- hardware-independent producer / consumer ring logic
- RX descriptor-ring base/length programming through `RDBAL/RDBAH/RDLEN`
- TX descriptor-ring base/length programming through `TDBAL/TDBAH/TDLEN`
- descriptor-register MMIO readback validation
- deterministic 60-byte TX frame construction
- streaming TX packet mapping with `DMA_TO_DEVICE`
- one-descriptor TX submission through `TDH/TDT`
- TX engine enablement for the validation transaction
- TX descriptor completion through `DD` write-back
- independent host-side TX packet-capture proof
- opt-in deterministic external RX validation
- 2048-byte streaming RX packet mapping with `DMA_FROM_DEVICE`
- one-descriptor RX ownership through `RDH/RDT`
- RX engine enablement for the validation transaction
- external host frame injection through `AF_PACKET` and `tap-e1000`
- RX `DD` / `EOP` completion validation
- exact 60-byte RX frame comparison after DMA ownership returns to the CPU
- dependency-safe unwind and teardown

Current automated results:

```text
Unity unit tests:                    47 / 47 passed
Regression integration checks:      35 / 35 passed
Real TX packet DMA validation:       passed
Real external RX DMA validation:     passed
Full e1000 validation gate:          passed
```

This is a **raw packet-DMA bring-up milestone**, not yet a production Linux network interface.

The project does **not** yet register a Linux `net_device`, deliver received packets into the Linux networking stack, accept normal stack-originated transmit traffic, use NAPI, or run an asynchronous production RX/TX datapath.

---

## Bidirectional Packet DMA

The driver now validates controlled packet movement in both directions against the QEMU-emulated Intel 82540EM.

### Transmit

```text
                         TRANSMIT

Guest CPU RAM
    │
    ▼
TX packet buffer
    │
    │ dma_map_single(..., DMA_TO_DEVICE)
    ▼
TX descriptor
    │
    │ TDT doorbell
    ▼
QEMU Intel 82540EM
    │
    ▼
tap-e1000 / host
    │
    └── independent packet capture proof
```

The TX path is validated twice:

1. inside the guest through descriptor completion and head/tail state
2. outside the guest through an independent host-side packet capture

The deterministic TX frame is:

```text
Length:       60 bytes before Ethernet FCS
Destination:  ff:ff:ff:ff:ff:ff
Source:       52:54:00:12:34:57
EtherType:    0x88B5
Payload:      SK_E1000_TX_TEST_001 + deterministic A0..AF pattern
```

The legacy TX descriptor sets `IFCS`, which requests Ethernet FCS insertion under the Intel descriptor contract. QEMU's host network backend and packet capture expose the 60-byte frame payload without a captured FCS; the e1000 model accounts for the additional four FCS bytes in its controller statistics.

Observed completion:

```text
TX descriptor status: 0x01
TDH:                  1
TDT:                  1
Length:               60 bytes
```

### Receive

```text
                          RECEIVE

Python AF_PACKET injector
    │
    ▼
tap-e1000
src 0a:d4:85:ef:53:63
    │
    ▼
QEMU Intel 82540EM
    │
    │ DMA_FROM_DEVICE
    ▼
Guest RX buffer
    │
    ▼
RX descriptor DD + EOP
    │
    ▼
exact 60-byte comparison
    │
    ▼
PASS
```

The host injector is implemented independently in:

```text
tests/integration/inject_rx_frame.py
```

It sends this deterministic Ethernet frame through the host-side TAP interface:

```text
Length:       60 bytes
Destination:  52:54:00:12:34:57
Source:       0a:d4:85:ef:53:63
EtherType:    0x88B6
Payload:      SK_E1000_RX_TEST_001 + deterministic B0..BF pattern
```

In the reference lab, `0a:d4:85:ef:53:63` is the actual MAC assigned to `tap-e1000`. The local TAP setup helper restores that MAC when the interface is recreated, keeping the external test deterministic across WSL restarts.

The driver:

1. allocates a 2048-byte RX buffer
2. maps it with `dma_map_single(..., DMA_FROM_DEVICE)`
3. places the DMA address in RX descriptor 0
4. initializes the validation ring state with `RDH=0` and `RDT=1`
5. enables the receive engine for the controlled test
6. waits for the independently generated host frame
7. observes descriptor completion
8. verifies descriptor status, length, and head/tail state
9. disables the packet engines
10. unmaps the streaming DMA buffer, returning ownership to the CPU
11. compares all 60 received bytes against the expected frame

Observed completion:

```text
RX descriptor status: 0x07
RX length:            60 bytes
RDH:                  1
RDT:                  1
Exact frame compare:  passed
```

For the QEMU `e1000` validation run, `0x07` includes `DD`, `EOP`, and `IXSM`.

This controlled one-descriptor validation proves that the QEMU e1000 model DMA-wrote the externally generated frame into guest memory and that the guest driver recovered the exact expected bytes.

---

## Use Case

Embedded Linux systems frequently communicate with high-performance peripherals through PCI-family buses.

An Ethernet Network Interface Card (NIC) is a useful systems-programming target because one device combines:

- PCI device discovery
- BAR ownership
- MMIO
- PCI bus mastering
- hardware interrupts
- DMA
- descriptor rings
- producer / consumer ownership
- memory ordering
- packet movement
- Linux kernel resource management
- Linux networking integration

The project goal is to build one coherent driver from device discovery through a real Linux network interface while exposing the hardware/software contract clearly at each milestone.

---

## Why This Project

The same low-level mechanisms used by network drivers also appear in:

- storage controllers
- GPU and accelerator drivers
- FPGA devices
- cameras and imaging systems
- wireless controllers
- high-speed data-acquisition hardware

The broader systems question is:

> **How does Linux safely and efficiently communicate with hardware?**

This project focuses on that boundary rather than reimplementing higher-level networking functionality already provided by Linux.

---

## Hardware / VM Architecture

The QEMU virtual machine uses two network devices so management connectivity remains independent from the project NIC.

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
                          +------------------+------------------+
                          |                                     |
                          v                                     v
                 RX descriptor ring                   TX descriptor ring
                 64 x 16-byte descriptors             64 x 16-byte descriptors
                 1024-byte coherent DMA               1024-byte coherent DMA
                          |                                     |
                          +------------------+------------------+
                                             |
                          ring base / length programming
                          + MMIO readback validation
                                             |
                         +-------------------+-------------------+
                         |                                       |
                         v                                       v
                controlled RX validation                controlled TX validation
                RDH / RDT + RX buffer                   TDH / TDT + TX buffer
                DMA_FROM_DEVICE                         DMA_TO_DEVICE
                         |                                       |
                         +-------------------+-------------------+
                                             |
                                             v
                                  QEMU e1000 packet backend
                                             |
                                             v
                                         tap-e1000
                                             |
                          +------------------+------------------+
                          |                                     |
                          v                                     v
                Python AF_PACKET injector            host packet capture
```

The Virtio NIC remains controlled by the standard Linux driver so SSH and management connectivity are not interrupted while `sk_e1000` takes ownership of the dedicated Intel 82540EM device.

The e1000 peripheral is emulated by QEMU. The Linux PCI, MMIO, DMA, interrupt, driver-binding, resource-ownership, and cleanup mechanisms exercised by the module are real Linux mechanisms.

---

## Driver Initialization and Validation Flow

The implemented path is:

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
Discover and claim BAR0
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
Allocate coherent RX descriptor ring
        |
        v
Allocate coherent TX descriptor ring
        |
        v
Disable / quiesce RX and TX packet engines
        |
        v
Program RDBAL / RDBAH / RDLEN
        |
        v
Program TDBAL / TDBAH / TDLEN
        |
        v
Read back and validate programmed ring registers
        |
        v
Register legacy INTx handler
        |
        v
Trigger and validate deterministic LSC interrupt
        |
        v
Run deterministic TX packet-DMA validation
        |
        +--> map TX packet DMA_TO_DEVICE
        +--> populate legacy TX descriptor
        +--> initialize TDH / TDT
        +--> enable TX
        +--> ring TDT doorbell
        +--> poll DD completion
        +--> validate TDH / TDT
        +--> disable engine and unmap
        |
        v
If run_rx_test=1:
        |
        +--> map RX buffer DMA_FROM_DEVICE
        +--> populate legacy RX descriptor
        +--> initialize RDH / RDT
        +--> enable RX
        +--> wait for host-injected frame
        +--> validate DD / EOP / errors / length
        +--> validate RDH / RDT
        +--> disable engines
        +--> unmap DMA_FROM_DEVICE buffer
        +--> exact byte-for-byte frame validation
        |
        v
Validation PASS
```

The deterministic packet paths are deliberately synchronous bring-up tests.

A production network driver will require asynchronous descriptor ownership, interrupt/NAPI processing, queue state, packet delivery, reclamation, and Linux networking integration.

---

## MMIO

MMIO = Memory-Mapped I/O.

BAR0 is discovered dynamically through the Linux PCI subsystem and mapped with Linux PCI/MMIO APIs. The physical BAR address is never hard-coded.

Registers currently used include:

- `CTRL` — Device Control
- `STATUS` — Device Status
- `RCTL` — Receive Control
- `TCTL` — Transmit Control
- `RDBAL` / `RDBAH` — RX descriptor-ring DMA base
- `RDLEN` — RX descriptor-ring byte length
- `RDH` — RX descriptor head
- `RDT` — RX descriptor tail
- `TDBAL` / `TDBAH` — TX descriptor-ring DMA base
- `TDLEN` — TX descriptor-ring byte length
- `TDH` — TX descriptor head
- `TDT` — TX descriptor tail
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

The packet engines are kept disabled during ring programming and teardown. They are enabled only for the controlled packet validation transaction.

---

## DMA and Descriptor Memory

DMA = Direct Memory Access.

The driver first establishes the DMA address width supported by the platform/device combination:

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

### Coherent descriptor rings

RX and TX descriptor rings use coherent DMA memory:

```c
dma_alloc_coherent()
dma_free_coherent()
```

Each ring contains:

```text
64 descriptors x 16 bytes = 1024 bytes
```

The driver tracks both views of every coherent allocation:

```text
                  one coherent allocation
                           |
              +------------+------------+
              |                         |
              v                         v
         CPU address                DMA address
         cpu_addr                   dma_addr
              |                         |
              v                         v
         CPU access                  NIC access
```

### Streaming packet buffers

Packet buffers use streaming DMA mappings:

```text
TX packet:
    dma_map_single(..., DMA_TO_DEVICE)
    ...
    dma_unmap_single(..., DMA_TO_DEVICE)

RX packet:
    dma_map_single(..., DMA_FROM_DEVICE)
    ...
    dma_unmap_single(..., DMA_FROM_DEVICE)
```

This separation is intentional:

- descriptor rings are persistent coherent control structures
- validation packet buffers are streaming DMA data buffers

The driver never assumes that a CPU virtual address can be supplied directly to the NIC.

---

## Intel Legacy Descriptor Formats

The Intel 82540EM legacy receive and transmit descriptor layouts are defined in:

```text
include/sk_e1000_desc.h
```

Current contract:

```text
Legacy RX descriptor size: 16 bytes
Legacy TX descriptor size: 16 bytes
Descriptor count per ring: 64
Ring size:                 1024 bytes
```

Compile-time assertions verify descriptor layout and ring geometry.

The current packet-DMA milestone uses descriptor 0 for a controlled one-packet validation transaction.

The reusable producer / consumer queue helpers are not yet wired into a production asynchronous hardware datapath.

---

## Descriptor-Ring Management Logic

Hardware-independent circular-ring logic is implemented in:

```text
include/sk_e1000_ring.h
src/sk_e1000_ring.c
```

It covers:

- next-index advancement
- wraparound
- empty detection
- full detection
- one-slot guard semantics
- used-descriptor accounting
- free-descriptor accounting
- defensive handling of invalid indexes and small rings

The implementation uses producer / consumer terminology:

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

The same production implementation is compiled into the kernel module and exercised directly by Unity tests.

The helpers are not yet connected to a reusable multi-packet hardware ownership loop.

---

## Interrupt Handling

The reference QEMU configuration uses legacy PCI INTx interrupts.

The driver:

1. masks device interrupt sources before handler registration
2. registers a shared Linux interrupt handler
3. enables the Link Status Change (LSC) interrupt cause
4. writes the LSC cause through `ICS`
5. allows the QEMU e1000 model to assert PCI INTx
6. receives the interrupt through the Linux interrupt subsystem
7. reads `ICR`
8. verifies that the interrupt belongs to this device
9. validates that LSC was observed

```text
CPU
 |
 | MMIO write to ICS
 v
QEMU Intel e1000 model
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

The current TX/RX packet validation paths use synchronous completion polling for bring-up. They are not yet the future interrupt/NAPI-driven network datapath.

---

## Shared Production Logic

Hardware-independent logic is separated from kernel/MMIO/DMA operations so production implementations can be tested directly in user space.

```text
Interrupt logic
include/sk_e1000_logic.h
          |
          v
src/sk_e1000_logic.c
      /           \
     v             v
kernel module   Unity tests


Ring logic
include/sk_e1000_ring.h
          |
          v
src/sk_e1000_ring.c
      /           \
     v             v
kernel module   Unity tests


Frame logic
include/sk_e1000_frame.h
          |
          v
src/sk_e1000_frame.c
      /           \
     v             v
kernel module   Unity tests
```

Shared frame logic currently covers:

- deterministic TX frame construction
- deterministic RX frame specification
- exact RX byte validation
- mismatch offset / expected-byte / actual-byte diagnostics

The Python RX injector remains independently implemented so the external packet source does not share the driver's frame-construction implementation.

---

## Deterministic RX Frame Validation

The shared RX frame checker validates:

- null input rejection
- exact frame length
- destination MAC
- source MAC
- EtherType
- recognizable payload prefix
- deterministic remaining payload
- first/last-byte corruption
- optional diagnostic outputs

The driver performs hardware-specific checks separately before invoking the shared byte validator:

- `DD`
- `EOP`
- descriptor error field
- descriptor length
- `RDH`
- `RDT`
- DMA ownership transfer back to the CPU

This keeps hardware-independent packet specification separate from kernel-specific descriptor and DMA behavior.

---

## Linux Networking Boundary

The NIC driver is responsible for eventually moving Ethernet frames between Linux and hardware.

Higher-level networking functionality remains in the Linux networking stack:

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

The current milestone proves raw Ethernet DMA against the dedicated QEMU NIC. It does not yet expose that NIC as a normal Linux network interface.

---

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
|   +-- sk_e1000_frame.c
|
+-- include/
|   +-- sk_e1000_logic.h
|   +-- sk_e1000_dma.h
|   +-- sk_e1000_desc.h
|   +-- sk_e1000_ring.h
|   +-- sk_e1000_frame.h
|
+-- docs/
|   +-- hardware-target.md
|   |
|   +-- evidence/
|       +-- final-bidirectional-dma-integration.txt
|       +-- final-bidirectional-dma-dmesg.txt
|       +-- final-bidirectional-dma-rx-dmesg.txt
|       +-- final-bidirectional-dma-rx-injector.txt
|       +-- tx_packet_pcap.txt
|       +-- previous milestone evidence...
|
+-- tests/
|   +-- unit/
|   |   +-- test_irq_logic.c
|   |   +-- test_ring_logic.c
|   |   +-- test_frame_logic.c
|   |
|   +-- integration/
|       +-- test_pci_mmio_dma_irq.sh
|       +-- prepare_rx_test_device.sh
|       +-- inject_rx_frame.py
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

---

## Testing Strategy

The project deliberately separates hardware-independent unit testing from hardware-dependent QEMU/Linux validation.

### Unit Tests

User-space unit tests use Unity and compile the same production logic used by the kernel module.

Current results:

```text
IRQ logic:
    9 Tests
    0 Failures

Ring logic:
    19 Tests
    0 Failures

Frame logic:
    19 Tests
    0 Failures

Total:
    47 Tests
    0 Failures
```

Frame coverage includes both deterministic TX construction and RX exact-frame validation.

No PCI device, MMIO implementation, DMA subsystem, interrupt controller, or QEMU NIC is mocked simply to increase test count.

### Regression Integration Test

The main regression integration test is:

```text
tests/integration/test_pci_mmio_dma_irq.sh
```

It validates the real Linux/QEMU path for:

- PCI device presence
- `8086:100e` identity
- module presence
- device ownership
- custom driver loading and binding
- BAR0 discovery
- CTRL / STATUS MMIO access
- PCI bus mastering
- DMA mask negotiation
- RX/TX coherent descriptor rings
- runtime DMA addresses
- RX/TX ring base/length programming
- MMIO readback
- packet engines disabled during ring programming
- Linux IRQ registration
- deterministic LSC interrupt generation
- expected `ICR=0x00000004`
- cleanup
- descriptor-ring release
- IRQ release
- PCI device release
- `BusMaster-` after cleanup

Current result:

```text
35 integration checks
35 passed
0 failed
```

### Real Packet-DMA Validation

The real RX path uses:

```text
tests/integration/prepare_rx_test_device.sh
tests/integration/inject_rx_frame.py
```

`prepare_rx_test_device.sh`:

- verifies PCI device `0000:00:04.0`
- verifies `8086:100e`
- unloads an older `sk_e1000` instance if necessary
- sets `driver_override=sk_e1000`
- unbinds the stock `e1000` driver when appropriate
- verifies that the device is ready for the custom driver

`inject_rx_frame.py`:

- opens an `AF_PACKET` raw socket
- binds to `tap-e1000`
- constructs an independent deterministic Ethernet frame
- sends exactly 60 bytes into the QEMU network backend
- prints the frame identity used for validation

The driver is loaded with:

```bash
sudo insmod ./sk_e1000.ko run_rx_test=1
```

and validates real TX and external RX DMA in the same probe instance.

---

## Reference Lab Validation Helpers

The reference development shell uses local Bash convenience functions:

```text
e1000copy
e1000evidence
ensure_e1000_tap
e1000vm
e1000validate
e1000rxvalidate
e1000validateall
```

These names are **development-environment helpers**, not kernel-driver APIs.

Their roles are:

```text
e1000copy
    WSL source-of-truth -> VM test directory

e1000evidence
    VM docs/evidence -> WSL repository

ensure_e1000_tap
    create/verify tap-e1000
    restore MAC 0a:d4:85:ef:53:63
    bring the interface up

e1000vm
    prepare tap-e1000
    start the QEMU/KVM reference VM

e1000validate
    source check
    -> copy
    -> clean kernel build
    -> Unity tests
    -> 35-check regression integration test
    -> evidence collection

e1000rxvalidate
    TAP validation
    -> copy/build
    -> external RX injector retry loop
    -> PCI device preparation
    -> run_rx_test=1
    -> real TX DMA
    -> real external RX DMA
    -> evidence collection

e1000validateall
    e1000validate
    -> e1000rxvalidate
    -> complete validation gate
```

The current packet-DMA checkpoint has passed the complete gate.

The repository integration scripts remain independently runnable; the shell helpers simply automate the reference WSL-to-VM workflow.

---

## Runtime Evidence

Runtime output is committed so implementation claims can be traced to actual executions.

### Current Bidirectional Packet-DMA Validation

Regression integration result:

- [`docs/evidence/final-bidirectional-dma-integration.txt`](docs/evidence/final-bidirectional-dma-integration.txt)

Regression kernel log:

- [`docs/evidence/final-bidirectional-dma-dmesg.txt`](docs/evidence/final-bidirectional-dma-dmesg.txt)

Real TX/RX DMA kernel log:

- [`docs/evidence/final-bidirectional-dma-rx-dmesg.txt`](docs/evidence/final-bidirectional-dma-rx-dmesg.txt)

Host RX injector evidence:

- [`docs/evidence/final-bidirectional-dma-rx-injector.txt`](docs/evidence/final-bidirectional-dma-rx-injector.txt)

Independent TX packet capture:

- [`docs/evidence/tx_packet_pcap.txt`](docs/evidence/tx_packet_pcap.txt)

Test sources:

- [`tests/integration/test_pci_mmio_dma_irq.sh`](tests/integration/test_pci_mmio_dma_irq.sh)
- [`tests/integration/prepare_rx_test_device.sh`](tests/integration/prepare_rx_test_device.sh)
- [`tests/integration/inject_rx_frame.py`](tests/integration/inject_rx_frame.py)
- [`tests/unit/test_irq_logic.c`](tests/unit/test_irq_logic.c)
- [`tests/unit/test_ring_logic.c`](tests/unit/test_ring_logic.c)
- [`tests/unit/test_frame_logic.c`](tests/unit/test_frame_logic.c)

Previous milestone evidence remains under `docs/evidence/` as development history.

---

## Observed Test Environment

Observed values from the reference QEMU development VM:

```text
PCI device:                 0000:00:04.0
Vendor / Device:            8086:100e
BAR0 base:                  0xfeb00000
BAR0 size:                  128 KiB
Linux IRQ:                  20
CTRL:                       0x40140240
STATUS:                     0x80080783
Test ICR:                   0x00000004
DMA addressing:             64-bit

RX descriptor count:        64
RX descriptor size:         16 bytes
RX ring allocation:         1024 bytes

TX descriptor count:        64
TX descriptor size:         16 bytes
TX ring allocation:         1024 bytes

RDLEN readback:             1024 bytes
TDLEN readback:             1024 bytes

TX validation length:       60 bytes
TX completion status:       0x01
TX completion TDH/TDT:      1 / 1

RX validation buffer:       2048 bytes
RX validation length:       60 bytes
RX completion status:       0x07
RX completion RDH/RDT:      1 / 1
RX exact byte comparison:   passed

Host TAP:                   tap-e1000
Host TAP MAC:               0a:d4:85:ef:53:63
Guest project NIC MAC:      52:54:00:12:34:57
```

DMA addresses are intentionally not documented as fixed values because they are dynamically assigned and differ between runs.

These are runtime observations from the reference lab, not hard-coded platform assumptions in the driver.

---

## Error Handling and Resource Ownership

Initialization acquires resources and programs hardware in explicit stages.

Core ownership sequence:

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
RX descriptor-ring coherent DMA
        |
        v
TX descriptor-ring coherent DMA
        |
        v
ring programming
        |
        v
IRQ registration
        |
        v
controlled packet validation
```

Packet-buffer ownership is explicit:

```text
TX:
CPU owns buffer
    -> dma_map_single(DMA_TO_DEVICE)
    -> device may DMA-read
    -> completion
    -> dma_unmap_single()
    -> CPU ownership restored

RX:
CPU prepares buffer
    -> dma_map_single(DMA_FROM_DEVICE)
    -> device may DMA-write
    -> completion
    -> stop receive engine
    -> dma_unmap_single()
    -> CPU ownership restored
    -> inspect bytes
```

The remove/error paths keep device interrupts and packet engines controlled before DMA/MMIO resources disappear.

Descriptor-ring allocations are released while the PCI device and DMA API context are still valid.

---

## Current Status

### Implemented and validated

- [x] PCI Vendor / Device ID matching
- [x] Linux `probe()` / `remove()` lifecycle
- [x] PCI memory-resource enablement
- [x] BAR0 validation, discovery, ownership, and MMIO mapping
- [x] CTRL / STATUS MMIO access
- [x] PCI bus mastering
- [x] 64-bit DMA negotiation with 32-bit fallback
- [x] Intel legacy RX/TX descriptor definitions
- [x] compile-time descriptor-size validation
- [x] coherent RX descriptor-ring allocation
- [x] coherent TX descriptor-ring allocation
- [x] CPU/DMA address separation
- [x] descriptor-ring alignment validation
- [x] RX base/length programming and readback
- [x] TX base/length programming and readback
- [x] hardware-independent producer / consumer helpers
- [x] ring wraparound / full / empty / used / free accounting
- [x] legacy INTx registration and deterministic interrupt validation
- [x] dependency-safe error unwind and teardown
- [x] deterministic TX Ethernet frame construction
- [x] TX streaming DMA mapping
- [x] controlled `TDH/TDT` initialization
- [x] TX descriptor submission
- [x] memory ordering before TX tail notification
- [x] TX engine enablement for validation
- [x] TX `DD` completion
- [x] independent TX packet-capture proof
- [x] deterministic external RX test frame
- [x] RX streaming DMA mapping
- [x] controlled `RDH/RDT` initialization
- [x] RX engine enablement for validation
- [x] RX `DD` / `EOP` completion
- [x] RX descriptor length and error validation
- [x] exact RX frame validation
- [x] independent Python `AF_PACKET` injector
- [x] automated PCI-device preparation for RX validation
- [x] 47/47 Unity tests
- [x] 35/35 regression integration checks
- [x] automated full validation gate
- [x] committed runtime evidence

### Not yet implemented as a production Linux networking datapath

- [ ] reusable multi-packet hardware producer / consumer ownership loop
- [ ] asynchronous RX queue replenishment
- [ ] asynchronous TX descriptor reclamation
- [ ] packet-driven RX/TX interrupts or NAPI
- [ ] Linux `net_device` registration
- [ ] interface open / close operations
- [ ] normal Linux transmit callback
- [ ] receive delivery into the Linux networking stack
- [ ] MAC configuration through Linux networking APIs
- [ ] driver packet/error statistics
- [ ] production queue stop/wake behavior
- [ ] performance tuning

---

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
- [x] shared INTx ownership handling
- [x] deterministic hardware interrupt validation
- [x] IRQ teardown
- [x] PCI bus-master cleanup

### DMA

- [x] DMA mask negotiation
- [x] coherent RX descriptor-ring allocation
- [x] coherent TX descriptor-ring allocation
- [x] TX streaming packet mapping for deterministic validation
- [x] RX streaming packet mapping for deterministic validation
- [x] DMA ownership/lifetime tracking
- [x] DMA cleanup
- [ ] reusable production RX packet-buffer pool
- [ ] reusable production TX packet mapping/reclamation path

### Descriptor Rings

- [x] Intel legacy RX descriptor definition
- [x] Intel legacy TX descriptor definition
- [x] compile-time descriptor-size validation
- [x] fixed ring geometry
- [x] producer / consumer helpers
- [x] ring wraparound logic
- [x] full / empty detection
- [x] used / free accounting
- [x] RX/TX coherent descriptor-ring memory
- [x] RX base/length register programming
- [x] TX base/length register programming
- [x] MMIO readback validation
- [x] controlled one-descriptor `RDH/RDT` validation
- [x] controlled one-descriptor `TDH/TDT` validation
- [ ] reusable software-ring state connected to continuous hardware ownership

### Receive

- [x] deterministic RX packet buffer
- [x] `DMA_FROM_DEVICE` mapping
- [x] controlled RX descriptor ownership
- [x] RX engine enablement for validation
- [x] external packet receive
- [x] `DD` / `EOP` completion
- [x] exact packet-byte validation
- [ ] reusable buffer replenishment
- [ ] asynchronous receive processing
- [ ] Linux packet delivery

### Transmit

- [x] deterministic TX packet buffer
- [x] `DMA_TO_DEVICE` mapping
- [x] controlled TX descriptor submission
- [x] memory ordering before tail notification
- [x] TDT hardware notification
- [x] TX engine enablement for validation
- [x] `DD` completion
- [x] independent external packet proof
- [ ] reusable packet submission queue
- [ ] asynchronous completion/reclamation
- [ ] Linux transmit callback

### Linux Networking

- [ ] `net_device` registration
- [ ] interface open / close
- [ ] transmit callback
- [ ] receive delivery
- [ ] MAC configuration
- [ ] driver statistics
- [ ] NAPI

### Validation

- [x] Unity unit-test framework
- [x] interrupt-logic tests
- [x] descriptor-ring logic tests
- [x] deterministic frame tests
- [x] RX frame-corruption tests
- [x] automated PCI/MMIO integration tests
- [x] automated interrupt validation
- [x] automated DMA/ring validation
- [x] TX packet-DMA validation
- [x] independent TX PCAP proof
- [x] external RX packet injection
- [x] RX packet-DMA validation
- [x] exact RX frame comparison
- [x] automated full validation gate
- [x] runtime evidence
- [ ] Linux network-interface traffic validation
- [ ] runtime packet statistics
- [ ] performance benchmarking

---

## Design Principles

1. Follow the actual NIC hardware contract.
2. Do not invent register layouts or descriptor formats.
3. Discover platform-assigned resources dynamically.
4. Keep hardware-specific operations separate from reusable pure logic.
5. Make CPU and NIC ownership explicit.
6. Keep CPU virtual addresses and device DMA addresses conceptually separate.
7. Distinguish coherent control structures from streaming packet mappings.
8. Treat DMA and interrupt execution as asynchronous even when bring-up validation polls synchronously.
9. Use memory ordering before notifying hardware.
10. Design cleanup paths together with initialization paths.
11. Release resources according to dependency order.
12. Unit test hardware-independent production logic.
13. Validate hardware-dependent behavior against Linux and the QEMU device model.
14. Preserve runtime evidence for validated milestones.
15. Make implementation claims only after the corresponding behavior has been demonstrated.

---

## Environment

Development and validation currently use:

- C
- Linux kernel modules
- Linux PCI subsystem
- Linux DMA API
- Linux MMIO APIs
- GCC
- Make / Kbuild
- QEMU
- KVM
- Intel 82540EM / QEMU `e1000`
- WSL Linux host
- host TAP networking
- Python `AF_PACKET`
- `tcpdump`
- legacy PCI INTx
- coherent DMA descriptor memory
- streaming DMA packet mappings
- Intel legacy RX/TX descriptors
- Unity C test framework

---

## Scope

The initial peripheral target is the QEMU-emulated Intel 82540EM.

QEMU provides a reproducible development environment while the driver exercises real Linux mechanisms including:

- PCI enumeration
- kernel driver matching and binding
- PCI resource ownership
- MMIO
- PCI bus mastering
- Linux DMA APIs
- coherent DMA allocation
- streaming DMA mapping
- interrupts
- kernel resource cleanup

The peripheral itself is emulated.

The current milestone proves controlled bidirectional raw Ethernet packet DMA against that emulated peripheral.

It does **not** yet claim:

- a production asynchronous RX/TX queue
- a Linux-visible network interface
- Linux-stack packet delivery
- Linux-stack packet transmission
- NAPI
- production networking performance

Physical NIC validation remains outside the current project scope.

---

## Hardware References

Implementation and validation are checked against the hardware/software contracts documented by:

- Intel 8254x Family of Gigabit Ethernet Controllers Software Developer's Manual
- QEMU `hw/net/e1000.c`
- upstream Linux Intel `e1000` driver sources

The reference implementation is also validated empirically against the QEMU device model rather than relying only on register definitions.

---

## Future Work

After `net_device` integration and a reusable packet path are operational, potential extensions include:

- NAPI receive processing
- interrupt-driven packet completion
- interrupt moderation
- Message Signaled Interrupts (MSI)
- checksum offload
- scatter/gather DMA
- multiple RX/TX queues
- Receive Side Scaling (RSS)
- packet/error statistics
- performance benchmarking
- fault-injection testing
- physical NIC validation

---

## Third-Party Software

The unit-test suite vendors Unity v2.7.0 under:

```text
third_party/unity/
```

Unity is distributed under the MIT license.

Its upstream license text is preserved in:

```text
third_party/unity/LICENSE.txt
```

---

## License

The kernel module currently declares:

```c
MODULE_LICENSE("GPL");
```

A repository-level license file should be added before treating the repository license as finalized.
