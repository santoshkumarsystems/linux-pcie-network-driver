#include <stddef.h>
#include <string.h>

#include "unity.h"
#include "sk_e1000_frame.h"


/*
 * --------------------------------------------------------------------------
 * TEST FIXTURES
 * --------------------------------------------------------------------------
 */

static const unsigned char expected_tx_dst_mac[SK_E1000_ETH_ADDR_LEN] = {
    0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU
};

static const unsigned char expected_tx_src_mac[SK_E1000_ETH_ADDR_LEN] = {
    0x52U, 0x54U, 0x00U,
    0x12U, 0x34U, 0x57U
};

static const unsigned char expected_tx_payload_prefix[] =
    "SK_E1000_TX_TEST_001";


static const unsigned char expected_rx_dst_mac[SK_E1000_ETH_ADDR_LEN] = {
    0x52U, 0x54U, 0x00U,
    0x12U, 0x34U, 0x57U
};

static const unsigned char expected_rx_src_mac[SK_E1000_ETH_ADDR_LEN] = {
    0x0aU, 0xd4U, 0x85U,
    0xefU, 0x53U, 0x63U
};

static const unsigned char expected_rx_payload_prefix[] =
    "SK_E1000_RX_TEST_001";


/*
 * Construct the reference RX frame independently inside the test suite.
 *
 * The production checker does not build this frame. Keeping the test fixture
 * separate helps catch mistakes in the byte-by-byte validation logic.
 */
static void build_expected_rx_frame(
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN])
{
    const size_t prefix_len =
        sizeof(expected_rx_payload_prefix) - 1U;
    size_t i;

    for (i = 0U; i < SK_E1000_ETH_ADDR_LEN; ++i) {
        frame[i] = expected_rx_dst_mac[i];
    }

    for (i = 0U; i < SK_E1000_ETH_ADDR_LEN; ++i) {
        frame[SK_E1000_ETH_ADDR_LEN + i] =
            expected_rx_src_mac[i];
    }

    frame[12] = SK_E1000_RX_TEST_ETHERTYPE_HI;
    frame[13] = SK_E1000_RX_TEST_ETHERTYPE_LO;

    for (i = 0U;
         i < (SK_E1000_RX_TEST_FRAME_LEN -
              SK_E1000_ETH_HEADER_LEN);
         ++i) {

        frame[SK_E1000_ETH_HEADER_LEN + i] =
            (unsigned char)(
                0xB0U +
                (i & 0x0fU));
    }

    memcpy(
        &frame[SK_E1000_ETH_HEADER_LEN],
        expected_rx_payload_prefix,
        prefix_len);
}


void setUp(void)
{
}


void tearDown(void)
{
}


/*
 * --------------------------------------------------------------------------
 * FAILURE-PATH TESTS
 * --------------------------------------------------------------------------
 */

static void test_frame_builder_rejects_null_buffer(void)
{
    TEST_ASSERT_EQUAL_size_t(
        0U,
        sk_e1000_build_tx_test_frame(
            NULL,
            SK_E1000_TX_TEST_FRAME_LEN));
}


static void test_frame_builder_rejects_buffer_smaller_than_frame(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];
    size_t i;

    /*
     * Fill with a sentinel first so we can verify that a rejected request
     * does not partially modify the caller's buffer.
     */
    memset(frame, 0x5a, sizeof(frame));

    TEST_ASSERT_EQUAL_size_t(
        0U,
        sk_e1000_build_tx_test_frame(
            frame,
            SK_E1000_TX_TEST_FRAME_LEN - 1U));

    for (i = 0U; i < sizeof(frame); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x5aU, frame[i]);
    }
}


/*
 * --------------------------------------------------------------------------
 * SUCCESS-PATH TESTS
 * --------------------------------------------------------------------------
 */

static void test_frame_builder_returns_exact_frame_length(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));
}


static void test_frame_builder_sets_broadcast_destination_mac(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_tx_dst_mac,
        &frame[0],
        SK_E1000_ETH_ADDR_LEN);
}


static void test_frame_builder_sets_expected_source_mac(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_tx_src_mac,
        &frame[SK_E1000_ETH_ADDR_LEN],
        SK_E1000_ETH_ADDR_LEN);
}


static void test_frame_builder_sets_expected_ethertype(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    TEST_ASSERT_EQUAL_HEX8(
        SK_E1000_TX_TEST_ETHERTYPE_HI,
        frame[12]);

    TEST_ASSERT_EQUAL_HEX8(
        SK_E1000_TX_TEST_ETHERTYPE_LO,
        frame[13]);
}


static void test_frame_builder_sets_expected_tx_payload_prefix(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];
    const size_t prefix_len =
        sizeof(expected_tx_payload_prefix) - 1U;

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_tx_payload_prefix,
        &frame[SK_E1000_ETH_HEADER_LEN],
        prefix_len);
}


static void test_frame_builder_sets_deterministic_remaining_payload(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN];
    const size_t prefix_len =
        sizeof(expected_tx_payload_prefix) - 1U;
    size_t payload_index;
    size_t frame_index;

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    /*
     * The implementation fills the entire 46-byte payload using:
     *
     *     0xA0 + (payload_index & 0x0f)
     *
     * and then overlays the ASCII prefix at payload offset zero.
     *
     * Therefore verification of the remaining payload begins at
     * payload_index == prefix_len rather than restarting the pattern.
     */
    for (payload_index = prefix_len;
         payload_index <
             (SK_E1000_TX_TEST_FRAME_LEN -
              SK_E1000_ETH_HEADER_LEN);
         ++payload_index) {

        frame_index =
            SK_E1000_ETH_HEADER_LEN +
            payload_index;

        TEST_ASSERT_EQUAL_HEX8(
            (unsigned char)(
                0xa0U +
                (payload_index & 0x0fU)),
            frame[frame_index]);
    }
}


static void test_frame_builder_accepts_larger_destination_buffer(void)
{
    unsigned char frame[SK_E1000_TX_TEST_FRAME_LEN + 16U];

    memset(frame, 0x5a, sizeof(frame));

    TEST_ASSERT_EQUAL_size_t(
        SK_E1000_TX_TEST_FRAME_LEN,
        sk_e1000_build_tx_test_frame(
            frame,
            sizeof(frame)));

    /*
     * The builder owns only the first 60 bytes. Bytes beyond the returned
     * frame length must remain untouched.
     */
    TEST_ASSERT_EACH_EQUAL_HEX8(
        0x5aU,
        &frame[SK_E1000_TX_TEST_FRAME_LEN],
        16U);
}



/*
 * --------------------------------------------------------------------------
 * RX FRAME VALIDATION TESTS
 * --------------------------------------------------------------------------
 */

static void test_rx_frame_checker_rejects_null_frame(void)
{
    size_t mismatch_offset = 123U;
    unsigned char expected_byte = 0x5aU;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_NULL,
        sk_e1000_check_rx_test_frame(
            NULL,
            SK_E1000_RX_TEST_FRAME_LEN,
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(0U, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(0x00U, expected_byte);
}


static void test_rx_frame_checker_rejects_wrong_length(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    size_t mismatch_offset = 123U;
    unsigned char expected_byte = 0x5aU;

    build_expected_rx_frame(frame);

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_LENGTH_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            SK_E1000_RX_TEST_FRAME_LEN - 1U,
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(0U, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(0x00U, expected_byte);
}


static void test_rx_frame_checker_accepts_exact_frame(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];

    build_expected_rx_frame(frame);

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_VALID,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            NULL,
            NULL));
}


static void test_rx_frame_checker_reports_destination_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[0] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(0U, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        expected_rx_dst_mac[0],
        expected_byte);
}


static void test_rx_frame_checker_reports_source_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    const size_t source_offset = SK_E1000_ETH_ADDR_LEN;
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[source_offset] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(source_offset, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        expected_rx_src_mac[0],
        expected_byte);
}


static void test_rx_frame_checker_reports_ethertype_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[13] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(13U, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        SK_E1000_RX_TEST_ETHERTYPE_LO,
        expected_byte);
}


static void test_rx_frame_checker_reports_payload_prefix_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    const size_t prefix_offset = SK_E1000_ETH_HEADER_LEN;
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[prefix_offset] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(prefix_offset, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        expected_rx_payload_prefix[0],
        expected_byte);
}


static void test_rx_frame_checker_reports_payload_pattern_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    const size_t prefix_len =
        sizeof(expected_rx_payload_prefix) - 1U;
    const size_t payload_index = prefix_len;
    const size_t frame_offset =
        SK_E1000_ETH_HEADER_LEN + payload_index;
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[frame_offset] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(frame_offset, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        (unsigned char)(
            0xB0U +
            (payload_index & 0x0fU)),
        expected_byte);
}


static void test_rx_frame_checker_reports_last_byte_mismatch(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];
    const size_t frame_offset =
        SK_E1000_RX_TEST_FRAME_LEN - 1U;
    const size_t payload_index =
        frame_offset - SK_E1000_ETH_HEADER_LEN;
    size_t mismatch_offset = 0U;
    unsigned char expected_byte = 0U;

    build_expected_rx_frame(frame);
    frame[frame_offset] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            &mismatch_offset,
            &expected_byte));

    TEST_ASSERT_EQUAL_size_t(frame_offset, mismatch_offset);
    TEST_ASSERT_EQUAL_HEX8(
        (unsigned char)(
            0xB0U +
            (payload_index & 0x0fU)),
        expected_byte);
}


static void test_rx_frame_checker_allows_null_diagnostic_outputs(void)
{
    unsigned char frame[SK_E1000_RX_TEST_FRAME_LEN];

    build_expected_rx_frame(frame);
    frame[SK_E1000_ETH_HEADER_LEN] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(
        SK_E1000_RX_TEST_FRAME_CONTENT_MISMATCH,
        sk_e1000_check_rx_test_frame(
            frame,
            sizeof(frame),
            NULL,
            NULL));
}


/*
 * --------------------------------------------------------------------------
 * TEST RUNNER
 * --------------------------------------------------------------------------
 */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_frame_builder_rejects_null_buffer);
    RUN_TEST(test_frame_builder_rejects_buffer_smaller_than_frame);

    RUN_TEST(test_frame_builder_returns_exact_frame_length);
    RUN_TEST(test_frame_builder_sets_broadcast_destination_mac);
    RUN_TEST(test_frame_builder_sets_expected_source_mac);
    RUN_TEST(test_frame_builder_sets_expected_ethertype);
    RUN_TEST(test_frame_builder_sets_expected_tx_payload_prefix);
    RUN_TEST(test_frame_builder_sets_deterministic_remaining_payload);
    RUN_TEST(test_frame_builder_accepts_larger_destination_buffer);

    RUN_TEST(test_rx_frame_checker_rejects_null_frame);
    RUN_TEST(test_rx_frame_checker_rejects_wrong_length);
    RUN_TEST(test_rx_frame_checker_accepts_exact_frame);
    RUN_TEST(test_rx_frame_checker_reports_destination_mismatch);
    RUN_TEST(test_rx_frame_checker_reports_source_mismatch);
    RUN_TEST(test_rx_frame_checker_reports_ethertype_mismatch);
    RUN_TEST(test_rx_frame_checker_reports_payload_prefix_mismatch);
    RUN_TEST(test_rx_frame_checker_reports_payload_pattern_mismatch);
    RUN_TEST(test_rx_frame_checker_reports_last_byte_mismatch);
    RUN_TEST(test_rx_frame_checker_allows_null_diagnostic_outputs);

    return UNITY_END();
}
