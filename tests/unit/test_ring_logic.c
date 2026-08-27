#include "unity.h"
#include "sk_e1000_ring.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/*
 * sk_e1000_ring_next()
 */

static void test_ring_next_advances_normally(void)
{
    TEST_ASSERT_EQUAL_UINT(1U, sk_e1000_ring_next(0U, 8U));
    TEST_ASSERT_EQUAL_UINT(4U, sk_e1000_ring_next(3U, 8U));
}

static void test_ring_next_wraps_at_last_slot(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_next(7U, 8U));
}

static void test_ring_next_handles_zero_sized_ring(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_next(0U, 0U));
}

/*
 * sk_e1000_ring_empty()
 */

static void test_ring_empty_when_indexes_match(void)
{
    TEST_ASSERT_TRUE(sk_e1000_ring_empty(0U, 0U));
    TEST_ASSERT_TRUE(sk_e1000_ring_empty(5U, 5U));
}

static void test_ring_not_empty_when_indexes_differ(void)
{
    TEST_ASSERT_FALSE(sk_e1000_ring_empty(3U, 1U));
}

/*
 * sk_e1000_ring_full()
 *
 * One descriptor is reserved as the guard slot.
 *
 * For an 8-slot ring:
 *
 *     physical slots = 8
 *     usable slots   = 7
 */

static void test_ring_not_full_when_empty(void)
{
    TEST_ASSERT_FALSE(sk_e1000_ring_full(0U, 0U, 8U));
}

static void test_ring_full_before_producer_hits_consumer(void)
{
    /*
     * consumer = 7
     * producer = 6
     *
     * next(producer) = 7 = consumer
     */
    TEST_ASSERT_TRUE(sk_e1000_ring_full(6U, 7U, 8U));
}

static void test_ring_full_across_wraparound(void)
{
    /*
     * consumer = 0
     * producer = 7
     *
     * next(7) = 0
     */
    TEST_ASSERT_TRUE(sk_e1000_ring_full(7U, 0U, 8U));
}

static void test_ring_with_one_slot_has_zero_usable_capacity(void)
{
    TEST_ASSERT_TRUE(sk_e1000_ring_full(0U, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_free(0U, 0U, 1U));
}

static void test_ring_full_rejects_out_of_range_producer(void)
{
    TEST_ASSERT_FALSE(sk_e1000_ring_full(8U, 0U, 8U));
}

static void test_ring_full_rejects_out_of_range_consumer(void)
{
    TEST_ASSERT_FALSE(sk_e1000_ring_full(0U, 8U, 8U));
}

/*
 * sk_e1000_ring_used()
 */

static void test_ring_used_is_zero_when_empty(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_used(0U, 0U, 8U));
}

static void test_ring_used_without_wraparound(void)
{
    /*
     * consumer = 0
     * producer = 3
     *
     * Three descriptors are between them.
     */
    TEST_ASSERT_EQUAL_UINT(3U, sk_e1000_ring_used(3U, 0U, 8U));
}

static void test_ring_used_with_wraparound(void)
{
    /*
     * count    = 8
     * consumer = 6
     * producer = 1
     *
     * Used descriptors:
     *
     *     6, 7, 0
     *
     * total = 3
     */
    TEST_ASSERT_EQUAL_UINT(3U, sk_e1000_ring_used(1U, 6U, 8U));
}

static void test_ring_used_rejects_out_of_range_index(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_used(8U, 0U, 8U));
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_used(0U, 8U, 8U));
}

/*
 * sk_e1000_ring_free()
 */

static void test_ring_free_when_empty_equals_usable_capacity(void)
{
    /*
     * Eight physical descriptors with one guard slot:
     *
     *     free = 8 - 1 = 7
     */
    TEST_ASSERT_EQUAL_UINT(7U, sk_e1000_ring_free(0U, 0U, 8U));
}

static void test_ring_free_after_three_descriptors_are_used(void)
{
    /*
     * usable = 7
     * used   = 3
     * free   = 4
     */
    TEST_ASSERT_EQUAL_UINT(4U, sk_e1000_ring_free(3U, 0U, 8U));
}

static void test_ring_free_is_correct_after_wraparound(void)
{
    /*
     * consumer = 6
     * producer = 1
     *
     * used = 3
     * free = 7 - 3 = 4
     */
    TEST_ASSERT_EQUAL_UINT(4U, sk_e1000_ring_free(1U, 6U, 8U));
}

static void test_ring_free_is_zero_when_ring_is_full(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, sk_e1000_ring_free(7U, 0U, 8U));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_ring_next_advances_normally);
    RUN_TEST(test_ring_next_wraps_at_last_slot);
    RUN_TEST(test_ring_next_handles_zero_sized_ring);

    RUN_TEST(test_ring_empty_when_indexes_match);
    RUN_TEST(test_ring_not_empty_when_indexes_differ);

    RUN_TEST(test_ring_not_full_when_empty);
    RUN_TEST(test_ring_full_before_producer_hits_consumer);
    RUN_TEST(test_ring_full_across_wraparound);
    RUN_TEST(test_ring_with_one_slot_has_zero_usable_capacity);
    RUN_TEST(test_ring_full_rejects_out_of_range_producer);
    RUN_TEST(test_ring_full_rejects_out_of_range_consumer);

    RUN_TEST(test_ring_used_is_zero_when_empty);
    RUN_TEST(test_ring_used_without_wraparound);
    RUN_TEST(test_ring_used_with_wraparound);
    RUN_TEST(test_ring_used_rejects_out_of_range_index);

    RUN_TEST(test_ring_free_when_empty_equals_usable_capacity);
    RUN_TEST(test_ring_free_after_three_descriptors_are_used);
    RUN_TEST(test_ring_free_is_correct_after_wraparound);
    RUN_TEST(test_ring_free_is_zero_when_ring_is_full);

    return UNITY_END();
}