#ifndef SK_E1000_FRAME_H
#define SK_E1000_FRAME_H

/*
 * This header is shared by:
 *
 *     1. the Linux kernel module
 *     2. user-space Unity unit tests
 *
 * Kernel code does not use the userspace C library's <stddef.h>.
 * Kbuild defines __KERNEL__, allowing the correct headers to be
 * selected for each compilation environment.
 */

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/stddef.h>

#else

#include <stddef.h>

#endif


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC ETHERNET TEST FRAMES
 * --------------------------------------------------------------------------
 *
 * The TX and RX data-path milestones use deterministic Ethernet frames so
 * software behavior can be compared against independent integration evidence.
 *
 * Ethernet frame layout used by both tests:
 *
 *     6 bytes   destination MAC
 *     6 bytes   source MAC
 *     2 bytes   EtherType
 *    46 bytes   payload / padding
 *   --------------------------------
 *    60 bytes   frame before Ethernet FCS
 *
 * For TX, the NIC inserts the 4-byte Frame Check Sequence when the transmit
 * descriptor requests IFCS.
 *
 * For RX validation, RCTL.SECRC instructs the NIC to strip the Ethernet FCS
 * before the driver inspects the DMA-written packet buffer.
 */

#define SK_E1000_ETH_ADDR_LEN          6U
#define SK_E1000_ETH_HEADER_LEN       14U

#define SK_E1000_TX_TEST_FRAME_LEN    60U
#define SK_E1000_RX_TEST_FRAME_LEN    60U


/*
 * Locally chosen experimental EtherTypes for deterministic validation.
 *
 * TX uses 0x88B5 and RX uses 0x88B6 so the two directions are immediately
 * distinguishable in packet captures and runtime evidence.
 */
#define SK_E1000_TX_TEST_ETHERTYPE_HI  0x88U
#define SK_E1000_TX_TEST_ETHERTYPE_LO  0xB5U

#define SK_E1000_RX_TEST_ETHERTYPE_HI  0x88U
#define SK_E1000_RX_TEST_ETHERTYPE_LO  0xB6U


/*
 * Result of hardware-independent deterministic RX frame validation.
 *
 * The checker deliberately does not return Linux errno values because this
 * source is compiled both inside the kernel module and by user-space tests.
 * Kernel-specific error mapping and logging remain in sk_e1000.c.
 */
enum sk_e1000_rx_test_frame_status {
    SK_E1000_RX_TEST_FRAME_VALID = 0,
    SK_E1000_RX_TEST_FRAME_NULL,
    SK_E1000_RX_TEST_FRAME_LENGTH_MISMATCH,
    SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH
};


/*
 * Build the deterministic Ethernet frame used by TX integration testing.
 *
 * frame:
 *     destination buffer owned by the caller.
 *
 * capacity:
 *     number of writable bytes available at frame.
 *
 * Return:
 *
 *     SK_E1000_TX_TEST_FRAME_LEN
 *         frame constructed successfully.
 *
 *     0
 *         frame is NULL or the destination buffer is too small.
 */
size_t sk_e1000_build_tx_test_frame(
    unsigned char *frame,
    size_t capacity);


/*
 * Validate the exact deterministic Ethernet frame injected by the WSL host
 * during RX integration testing.
 *
 * Reference RX frame:
 *
 *     destination  52:54:00:12:34:57
 *     source       0a:d4:85:ef:53:63
 *     EtherType    0x88B6
 *     payload      "SK_E1000_RX_TEST_001" + deterministic B0..BF pattern
 *
 * mismatch_offset / expected_byte:
 *     optional diagnostic outputs. They are initialized to zero when
 *     supplied and are populated with the first differing byte only when
 *     SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH is returned.
 *
 * The Python AF_PACKET injector is intentionally implemented independently
 * and does not call this helper. Exact agreement therefore remains useful
 * end-to-end evidence rather than a shared-code self-test.
 */
enum sk_e1000_rx_test_frame_status
sk_e1000_check_rx_test_frame(
    const unsigned char *frame,
    size_t length,
    size_t *mismatch_offset,
    unsigned char *expected_byte);

#endif /* SK_E1000_FRAME_H */
