#ifndef SK_E1000_DESC_H
#define SK_E1000_DESC_H

#include <linux/build_bug.h>
#include <linux/types.h>

/*
 * Intel 82540EM legacy descriptor-ring contract.
 *
 * Both receive and legacy transmit descriptors are 16 bytes.
 * The hardware descriptor-ring length programmed into RDLEN/TDLEN
 * must be a multiple of 128 bytes.
 *
 * We start with 64 descriptors:
 *
 *     64 * 16 = 1024 bytes
 *
 * 1024 bytes satisfies the Intel 128-byte ring-length requirement.
 */
#define SK_E1000_DESC_SIZE       16U
#define SK_E1000_RING_COUNT      64U
#define SK_E1000_RING_SIZE       (SK_E1000_RING_COUNT * SK_E1000_DESC_SIZE)
#define SK_E1000_RING_BASE_ALIGNMENT     16U
#define SK_E1000_RING_LEN_GRANULARITY   128U

/*
 * Receive descriptor status bits.
 *
 * DD  = Descriptor Done
 * EOP = End Of Packet
 *
 * Hardware writes these bits back after receiving packet data.
 */
#define SK_E1000_RXD_STAT_DD     0x01U
#define SK_E1000_RXD_STAT_EOP    0x02U

/*
 * Legacy transmit descriptor command bits.
 *
 * EOP  = End Of Packet
 * IFCS = Insert Frame Check Sequence
 * RS   = Report Status
 */
#define SK_E1000_TXD_CMD_EOP     0x01U
#define SK_E1000_TXD_CMD_IFCS    0x02U
#define SK_E1000_TXD_CMD_RS      0x08U

/*
 * Legacy transmit descriptor status bits.
 *
 * DD = Descriptor Done.
 * Hardware sets this when transmission associated with the descriptor
 * has completed and status write-back was requested.
 */
#define SK_E1000_TXD_STAT_DD     0x01U

/*
 * Intel 82540EM receive descriptor.
 *
 * Before handing a descriptor to the NIC:
 *
 *     buffer_addr = DMA address of the packet buffer
 *
 * Hardware later writes:
 *
 *     length
 *     checksum
 *     status
 *     errors
 *     special
 *
 * The address stored here is a device-visible DMA address, not a CPU
 * virtual address.
 */
struct sk_e1000_rx_desc {
    __le64 buffer_addr;
    __le16 length;
    __le16 checksum;
    u8 status;
    u8 errors;
    __le16 special;
};

/*
 * Intel 82540EM legacy transmit descriptor.
 *
 * Software supplies:
 *
 *     buffer_addr
 *     length
 *     command fields
 *
 * Hardware writes completion state into status.
 *
 * The layout intentionally mirrors the documented 16-byte legacy
 * descriptor instead of representing it as an opaque byte array.
 */
struct sk_e1000_tx_desc {
    __le64 buffer_addr;
    __le16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    __le16 special;
};

/*
 * These compile-time checks protect the hardware ABI.
 *
 * If either structure changes such that it is no longer exactly
 * 16 bytes, compilation must fail rather than silently programming
 * an invalid descriptor layout into the NIC.
 */
static_assert(sizeof(struct sk_e1000_rx_desc) == SK_E1000_DESC_SIZE);
static_assert(sizeof(struct sk_e1000_tx_desc) == SK_E1000_DESC_SIZE);
static_assert((SK_E1000_RING_SIZE %
               SK_E1000_RING_LEN_GRANULARITY) == 0U);

#endif /* SK_E1000_DESC_H */