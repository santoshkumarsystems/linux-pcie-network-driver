#!/usr/bin/env python3
"""
inject_rx_frame.py

Author: Santosh Kumar
Project: linux-pcie-network-driver

Purpose
-------
Inject one deterministic raw Ethernet frame from the WSL/Linux host into
the TAP interface connected to QEMU's emulated Intel 82540EM NIC.

Why this script exists
----------------------
The receive (RX) path must be validated with a packet that originates
OUTSIDE the guest driver.

If the driver created its own RX test packet internally, the test would
not prove that a real external Ethernet frame traveled through:

    Python on WSL/Linux host
            |
            | AF_PACKET raw Ethernet socket
            v
       tap-e1000
            |
            v
      QEMU labnet backend
            |
            v
  QEMU Intel 82540EM NIC
            |
            | DMA_FROM_DEVICE
            v
      guest RX buffer
            |
            v
      sk_e1000 driver

This script therefore acts as an independent packet generator. The driver
can later prove that the emulated NIC received the frame, DMA-wrote it into
guest RAM, completed the RX descriptor, and preserved every byte exactly.

Python / systems concepts demonstrated
--------------------------------------
- Python bytes and bytearray
- deterministic binary-data generation
- Ethernet frame construction
- Linux AF_PACKET raw sockets
- Layer-2 interface binding
- direct raw Ethernet transmission
- explicit frame-length validation
- stderr error reporting
- integration tooling for Linux kernel / QEMU driver validation

Deterministic frame
-------------------
Destination MAC : 52:54:00:12:34:57
Source MAC      : 0a:d4:85:ef:53:63
EtherType       : 0x88B6
Payload prefix  : SK_E1000_RX_TEST_001
Frame length    : 60 bytes before Ethernet FCS

The remaining payload bytes use a deterministic repeating pattern:

    B0 B1 B2 ... BF B0 B1 ...

Usage
-----
    sudo python3 tests/integration/inject_rx_frame.py

Expected output
---------------
    sent 60-byte deterministic RX test frame on tap-e1000
    dst=52:54:00:12:34:57
    src=0a:d4:85:ef:53:63
    ethertype=0x88b6
    payload-prefix=SK_E1000_RX_TEST_001
"""

import socket
import sys


# -----------------------------------------------------------------------------
# QEMU TAP INTERFACE
# -----------------------------------------------------------------------------
#
# QEMU's project e1000 NIC is attached to this Linux TAP interface.
# Sending a raw Ethernet frame through this interface injects the packet
# into QEMU's network backend.
#
INTERFACE = "tap-e1000"


# -----------------------------------------------------------------------------
# ETHERNET HEADER
# -----------------------------------------------------------------------------
#
# Ethernet header layout:
#
#     6 bytes  destination MAC
#     6 bytes  source MAC
#     2 bytes  EtherType
#    -------------------------
#    14 bytes total
#
# Destination MAC:
#     MAC address assigned to the QEMU-emulated Intel 82540EM.
#
# Source MAC:
#     Host-side TAP MAC used for this deterministic integration frame.
#
# EtherType:
#     0x88B6 is reserved here for RX validation traffic.
#     The driver's deterministic TX test uses 0x88B5, so the two directions
#     are easy to distinguish in tcpdump and PCAP evidence.
#
DST_MAC = bytes.fromhex("52 54 00 12 34 57")
SRC_MAC = bytes.fromhex("0a d4 85 ef 53 63")
ETHERTYPE = bytes.fromhex("88 b6")


# -----------------------------------------------------------------------------
# FRAME LENGTH
# -----------------------------------------------------------------------------
#
# Ethernet minimum frame length before the 4-byte Frame Check Sequence (FCS)
# is 60 bytes:
#
#     14-byte Ethernet header
#   + 46-byte payload
#   ----------------------
#     60 bytes
#
# The FCS is not part of this Python buffer.
#
FRAME_LEN = 60
ETH_HEADER_LEN = 14
PAYLOAD_LEN = FRAME_LEN - ETH_HEADER_LEN


# -----------------------------------------------------------------------------
# RECOGNIZABLE PAYLOAD PREFIX
# -----------------------------------------------------------------------------
#
# This ASCII marker makes the RX validation frame easy to identify in:
#
#     - tcpdump output
#     - Wireshark
#     - QEMU PCAP evidence
#     - kernel logs
#     - byte-for-byte driver validation
#
PREFIX = b"SK_E1000_RX_TEST_001"


# -----------------------------------------------------------------------------
# BUILD DETERMINISTIC PAYLOAD
# -----------------------------------------------------------------------------
#
# Fill all 46 payload bytes with this repeating sequence:
#
#     B0 B1 B2 ... BF B0 B1 ...
#
# Why:
#
#     i & 0x0F
#
# retains only the low 4 bits of i, so the value cycles from 0 through 15.
# Adding 0xB0 produces byte values 0xB0 through 0xBF repeatedly.
#
# Deterministic bytes are important because the RX driver can compare the
# received DMA buffer against the exact expected packet instead of merely
# checking that "some packet" arrived.
#
payload = bytearray(
    0xB0 + (i & 0x0F)
    for i in range(PAYLOAD_LEN)
)


# Replace the beginning of the binary pattern with a human-readable marker.
# The remainder of the payload stays deterministic.
payload[:len(PREFIX)] = PREFIX


# -----------------------------------------------------------------------------
# CONSTRUCT THE COMPLETE ETHERNET FRAME
# -----------------------------------------------------------------------------
#
# Wire-format order:
#
#     destination MAC
#          +
#     source MAC
#          +
#     EtherType
#          +
#     payload
#
frame = DST_MAC + SRC_MAC + ETHERTYPE + bytes(payload)


# -----------------------------------------------------------------------------
# DEFENSIVE LENGTH VALIDATION
# -----------------------------------------------------------------------------
#
# A malformed frame would weaken the integration evidence.
# Refuse to send anything unless the constructed packet is exactly 60 bytes.
#
if len(frame) != FRAME_LEN:
    print(
        f"internal error: frame length is {len(frame)}, "
        f"expected {FRAME_LEN}",
        file=sys.stderr,
    )
    sys.exit(1)


# -----------------------------------------------------------------------------
# CREATE RAW LAYER-2 SOCKET
# -----------------------------------------------------------------------------
#
# socket.AF_PACKET:
#     Linux-specific socket family that gives direct access to Layer 2
#     Ethernet frames.
#
# socket.SOCK_RAW:
#     The application supplies the full Ethernet header itself.
#
# This is lower level than normal TCP/UDP programming:
#
#     no TCP header
#     no UDP header
#     no IP header
#
# We are constructing and transmitting Ethernet bytes directly.
#
sock = socket.socket(
    socket.AF_PACKET,
    socket.SOCK_RAW,
)


try:
    # -------------------------------------------------------------------------
    # BIND TO THE QEMU TAP INTERFACE
    # -------------------------------------------------------------------------
    #
    # This prevents the packet from being sent through some unrelated Linux
    # interface. The frame goes specifically into the TAP endpoint connected
    # to QEMU's e1000 backend.
    #
    sock.bind((INTERFACE, 0))

    # -------------------------------------------------------------------------
    # SEND THE RAW ETHERNET FRAME
    # -------------------------------------------------------------------------
    #
    # send() returns the number of bytes accepted by the Linux networking
    # stack. The expected result is the complete 60-byte frame.
    #
    sent = sock.send(frame)

finally:
    # Always close the raw socket, even if bind() or send() raises an error.
    sock.close()


# -----------------------------------------------------------------------------
# VERIFY COMPLETE SEND
# -----------------------------------------------------------------------------
#
# Treat a short send as a test failure because the integration frame must be
# injected exactly as constructed.
#
if sent != FRAME_LEN:
    print(
        f"short send: {sent}/{FRAME_LEN} bytes",
        file=sys.stderr,
    )
    sys.exit(1)


# -----------------------------------------------------------------------------
# HUMAN-READABLE INTEGRATION EVIDENCE
# -----------------------------------------------------------------------------
#
# These values make a manual run or CI log self-describing. They also provide
# the identity that the kernel RX test and PCAP inspection should observe.
#
print(f"sent {sent}-byte deterministic RX test frame on {INTERFACE}")
print("dst=52:54:00:12:34:57")
print("src=0a:d4:85:ef:53:63")
print("ethertype=0x88b6")
print("payload-prefix=SK_E1000_RX_TEST_001")
