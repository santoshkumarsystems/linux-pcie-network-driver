/*
 * test_irq_logic.c
 *
 * Unit tests for hardware-independent interrupt decision logic used
 * by the sk_e1000 Linux PCIe network driver.
 *
 * These tests exercise the same production implementation compiled
 * from:
 *
 *     src/sk_e1000_logic.c
 *
 * Hardware-dependent behavior such as PCI enumeration, MMIO register
 * access, INTx delivery, and interrupt acknowledgement is validated
 * separately by QEMU integration tests.
 *
 * Test framework:
 *
 *     Unity v2.7.0
 *     https://github.com/ThrowTheSwitch/Unity
 *
 * Author: Santosh Kumar
 */

#include "unity.h"
#include "sk_e1000_logic.h"


/*
 * Unity calls setUp() before every test.
 *
 * The current interrupt helper functions are stateless, so there is
 * no per-test state to initialize.
 */

void setUp(void)
{
}


/*
 * Unity calls tearDown() after every test.
 *
 * The current tests allocate no resources and therefore require no
 * cleanup.
 */

void tearDown(void)
{
}


/*
 * --------------------------------------------------------------------------
 * INTERRUPT-PENDING TESTS
 * --------------------------------------------------------------------------
 *
 * Legacy PCI INTx interrupt lines may be shared by multiple devices.
 *
 * An Interrupt Cause Register (ICR) value of zero means this e1000
 * controller has no pending interrupt cause and the driver must not
 * claim the shared interrupt.
 */

static void test_irq_pending_returns_false_for_zero_cause(void)
{
    TEST_ASSERT_FALSE(
        sk_e1000_irq_is_pending(0x00000000U));
}


/*
 * Link Status Change (LSC) is a valid device interrupt cause.
 */

static void test_irq_pending_returns_true_for_lsc(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_is_pending(SK_E1000_INT_LSC));
}


/*
 * The pending decision must not depend specifically on LSC.
 *
 * Any non-zero ICR value indicates that at least one interrupt cause
 * has been reported by this e1000 controller.
 */

static void test_irq_pending_returns_true_for_other_cause(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_is_pending(0x00000001U));
}


/*
 * Multiple simultaneous interrupt causes must also be recognized as
 * pending.
 *
 * 0x00000005 contains bit 0 and bit 2 (LSC).
 */

static void test_irq_pending_returns_true_for_multiple_causes(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_is_pending(0x00000005U));
}


/*
 * --------------------------------------------------------------------------
 * LINK STATUS CHANGE TESTS
 * --------------------------------------------------------------------------
 */

/*
 * A zero ICR cannot contain the Link Status Change bit.
 */

static void test_lsc_returns_false_for_zero_cause(void)
{
    TEST_ASSERT_FALSE(
        sk_e1000_irq_has_lsc(0x00000000U));
}


/*
 * Verify detection when LSC is the only reported interrupt cause.
 */

static void test_lsc_returns_true_for_exact_lsc_cause(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_has_lsc(SK_E1000_INT_LSC));
}


/*
 * An unrelated interrupt cause must not be misidentified as LSC.
 */

static void test_lsc_returns_false_for_unrelated_cause(void)
{
    TEST_ASSERT_FALSE(
        sk_e1000_irq_has_lsc(0x00000001U));
}


/*
 * Several e1000 interrupt causes may be reported simultaneously.
 *
 * The production implementation must therefore test the LSC bit
 * rather than compare the entire ICR value against SK_E1000_INT_LSC.
 */

static void test_lsc_returns_true_when_multiple_causes_include_lsc(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_has_lsc(0x00000005U));
}


/*
 * Boundary case:
 *
 * If every interrupt-cause bit is set, LSC must still be detected.
 */

static void test_lsc_returns_true_when_all_bits_are_set(void)
{
    TEST_ASSERT_TRUE(
        sk_e1000_irq_has_lsc(0xffffffffU));
}


/*
 * --------------------------------------------------------------------------
 * TEST RUNNER
 * --------------------------------------------------------------------------
 */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(
        test_irq_pending_returns_false_for_zero_cause);

    RUN_TEST(
        test_irq_pending_returns_true_for_lsc);

    RUN_TEST(
        test_irq_pending_returns_true_for_other_cause);

    RUN_TEST(
        test_irq_pending_returns_true_for_multiple_causes);

    RUN_TEST(
        test_lsc_returns_false_for_zero_cause);

    RUN_TEST(
        test_lsc_returns_true_for_exact_lsc_cause);

    RUN_TEST(
        test_lsc_returns_false_for_unrelated_cause);

    RUN_TEST(
        test_lsc_returns_true_when_multiple_causes_include_lsc);

    RUN_TEST(
        test_lsc_returns_true_when_all_bits_are_set);

    return UNITY_END();
}