#include "sk_e1000_frame.h"


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC TX TEST FRAME CONTENT
 * --------------------------------------------------------------------------
 *
 * Destination:
 *
 *     ff:ff:ff:ff:ff:ff
 *
 * Broadcast is deliberate for the first raw transmit milestone. The goal is
 * to prove that the exact Ethernet frame constructed in CPU memory reaches
 * the QEMU e1000 network backend.
 *
 * Source:
 *
 *     52:54:00:12:34:57
 *
 * This matches the MAC address assigned to the dedicated project e1000 NIC
 * in the QEMU launch configuration.
 *
 * EtherType:
 *
 *     0x88B5
 *
 * Payload:
 *
 *     starts with "SK_E1000_TX_TEST_001"
 *
 * Remaining payload bytes are filled with a deterministic A0..AF byte
 * pattern so the complete transmitted frame can be compared with capture
 * evidence.
 */

static const unsigned char sk_e1000_tx_test_dst_mac[
    SK_E1000_ETH_ADDR_LEN] = {
        0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU
};

static const unsigned char sk_e1000_tx_test_src_mac[
    SK_E1000_ETH_ADDR_LEN] = {
        0x52U, 0x54U, 0x00U,
        0x12U, 0x34U, 0x57U
};

static const unsigned char sk_e1000_tx_test_payload_prefix[] =
    "SK_E1000_TX_TEST_001";


size_t sk_e1000_build_tx_test_frame(
    unsigned char *frame,
    size_t capacity)
{
    size_t i;
    size_t offset;
    size_t prefix_len;

    if (frame == NULL) {
        return 0U;
    }

    if (capacity < SK_E1000_TX_TEST_FRAME_LEN) {
        return 0U;
    }


    /*
     * Ethernet destination MAC.
     */
    for (i = 0U; i < SK_E1000_ETH_ADDR_LEN; ++i) {
        frame[i] = sk_e1000_tx_test_dst_mac[i];
    }


    /*
     * Ethernet source MAC.
     */
    offset = SK_E1000_ETH_ADDR_LEN;

    for (i = 0U; i < SK_E1000_ETH_ADDR_LEN; ++i) {
        frame[offset + i] = sk_e1000_tx_test_src_mac[i];
    }


    /*
     * EtherType is stored in network byte order.
     */
    frame[12] = SK_E1000_TX_TEST_ETHERTYPE_HI;
    frame[13] = SK_E1000_TX_TEST_ETHERTYPE_LO;


    /*
     * Fill the complete 46-byte Ethernet payload with a deterministic
     * byte pattern first.
     *
     * The pattern repeats:
     *
     *     A0 A1 A2 ... AF A0 A1 ...
     */
    offset = SK_E1000_ETH_HEADER_LEN;

    for (i = 0U;
         i < (SK_E1000_TX_TEST_FRAME_LEN -
              SK_E1000_ETH_HEADER_LEN);
         ++i) {

        frame[offset + i] =
            (unsigned char)(
                0xA0U +
                (i & 0x0fU));
    }


    /*
     * Overlay the recognizable ASCII marker at the start of the payload.
     *
     * sizeof(array) includes the terminating '\0', which is intentionally
     * not copied into the Ethernet payload.
     */
    prefix_len =
        sizeof(sk_e1000_tx_test_payload_prefix) - 1U;

    for (i = 0U; i < prefix_len; ++i) {
        frame[offset + i] =
            sk_e1000_tx_test_payload_prefix[i];
    }


    return SK_E1000_TX_TEST_FRAME_LEN;
}


/*
 * --------------------------------------------------------------------------
 * DETERMINISTIC RX TEST FRAME CONTENT
 * --------------------------------------------------------------------------
 *
 * This is the software-side specification independently matched by
 * tests/integration/inject_rx_frame.py.
 *
 * Destination:
 *
 *     52:54:00:12:34:57
 *
 * Source:
 *
 *     0a:d4:85:ef:53:63
 *
 * This is the deterministic MAC configured on the host-side tap-e1000
 * interface by the reference WSL/QEMU lab setup.
 *
 * EtherType:
 *
 *     0x88B6
 *
 * Payload:
 *
 *     starts with "SK_E1000_RX_TEST_001"
 *
 * Remaining payload bytes follow the deterministic B0..BF pattern using
 * their original payload index.
 */

static const unsigned char sk_e1000_rx_test_dst_mac[
    SK_E1000_ETH_ADDR_LEN] = {
        0x52U, 0x54U, 0x00U,
        0x12U, 0x34U, 0x57U
};

static const unsigned char sk_e1000_rx_test_src_mac[
    SK_E1000_ETH_ADDR_LEN] = {
        0x0aU, 0xd4U, 0x85U,
        0xefU, 0x53U, 0x63U
};

static const unsigned char sk_e1000_rx_test_payload_prefix[] =
    "SK_E1000_RX_TEST_001";


enum sk_e1000_rx_test_frame_status
sk_e1000_check_rx_test_frame(
    const unsigned char *frame,
    size_t length,
    size_t *mismatch_offset,
    unsigned char *expected_byte)
{
    size_t i;
    size_t payload_index;
    size_t prefix_len;
    unsigned char expected;

    /*
     * Keep diagnostic outputs deterministic for every return path.
     */
    if (mismatch_offset != NULL) {
        *mismatch_offset = 0U;
    }

    if (expected_byte != NULL) {
        *expected_byte = 0U;
    }


    if (frame == NULL) {
        return SK_E1000_RX_TEST_FRAME_NULL;
    }

    if (length != SK_E1000_RX_TEST_FRAME_LEN) {
        return SK_E1000_RX_TEST_FRAME_LENGTH_MISMATCH;
    }


    prefix_len =
        sizeof(sk_e1000_rx_test_payload_prefix) - 1U;


    for (i = 0U; i < length; ++i) {

        if (i < SK_E1000_ETH_ADDR_LEN) {

            expected =
                sk_e1000_rx_test_dst_mac[i];

        } else if (i < (2U * SK_E1000_ETH_ADDR_LEN)) {

            expected =
                sk_e1000_rx_test_src_mac[
                    i - SK_E1000_ETH_ADDR_LEN];

        } else if (i == 12U) {

            expected =
                SK_E1000_RX_TEST_ETHERTYPE_HI;

        } else if (i == 13U) {

            expected =
                SK_E1000_RX_TEST_ETHERTYPE_LO;

        } else {

            payload_index =
                i - SK_E1000_ETH_HEADER_LEN;

            if (payload_index < prefix_len) {

                expected =
                    sk_e1000_rx_test_payload_prefix[
                        payload_index];

            } else {

                expected =
                    (unsigned char)(
                        0xB0U +
                        (payload_index & 0x0fU));
            }
        }


        if (frame[i] != expected) {

            if (mismatch_offset != NULL) {
                *mismatch_offset = i;
            }

            if (expected_byte != NULL) {
                *expected_byte = expected;
            }

            return SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH;
        }
    }


    return SK_E1000_RX_TEST_FRAME_VALID;
}
