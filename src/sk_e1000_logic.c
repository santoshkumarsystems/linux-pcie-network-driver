/*
 * sk_e1000_logic.c
 *
 * Hardware-independent decision logic shared by the production
 * sk_e1000 kernel driver and user-space unit tests.
 *
 * Keeping this logic separate from MMIO and Linux kernel operations
 * allows unit tests to execute the same implementation used by the
 * real driver without mocking hardware access.
 *
 * Author: Santosh Kumar
 */

#include "sk_e1000_logic.h"


/*
 * --------------------------------------------------------------------------
 * INTERRUPT PENDING CHECK
 * --------------------------------------------------------------------------
 *
 * Legacy PCI INTx interrupt lines may be shared by several devices.
 *
 * The driver therefore reads the Intel e1000 Interrupt Cause Register
 * (ICR) before claiming an interrupt.
 *
 * cause == 0:
 *
 *     This e1000 device has no pending interrupt cause.
 *     The shared interrupt must not be claimed by this driver.
 *
 * cause != 0:
 *
 *     At least one interrupt cause belongs to this device.
 */

int sk_e1000_irq_is_pending(sk_e1000_u32 cause)
{
    return cause != 0U;
}


/*
 * --------------------------------------------------------------------------
 * LINK STATUS CHANGE CHECK
 * --------------------------------------------------------------------------
 *
 * More than one e1000 interrupt cause can be present in the ICR at
 * the same time.
 *
 * Therefore this function checks only the LSC bit rather than using:
 *
 *     cause == SK_E1000_INT_LSC
 *
 * Example:
 *
 *     cause = 0x00000005
 *
 * contains both bit 0 and the LSC bit. The LSC condition must still
 * be detected.
 */

int sk_e1000_irq_has_lsc(sk_e1000_u32 cause)
{
    return (cause & SK_E1000_INT_LSC) != 0U;
}