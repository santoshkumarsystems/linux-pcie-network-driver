#!/usr/bin/env bash
#
# test_pci_mmio_irq.sh
#
# Integration validation for the sk_e1000 Linux PCIe network driver.
#
# Hardware target:
#   QEMU-emulated Intel 82540EM
#   PCI ID: 8086:100e
#   PCI address: 0000:00:04.0
#
# This test validates real Linux/QEMU hardware interaction:
#
#   - PCI device discovery
#   - PCI Vendor / Device identity
#   - driver binding
#   - BAR0 discovery
#   - MMIO register access
#   - PCI bus mastering
#   - legacy INTx interrupt registration
#   - e1000 interrupt generation through ICS
#   - interrupt delivery through Linux
#   - ICR cause handling
#   - Link Status Change interrupt handling
#   - ordered module teardown
#   - IRQ release
#   - PCI bus-master cleanup
#   - PCI device release
#
# Hardware-independent decision logic is tested separately with
# Unity under tests/unit/.
#
# Author: Santosh Kumar
#

set -Eeuo pipefail


# -----------------------------------------------------------------------------
# Test configuration
# -----------------------------------------------------------------------------

PCI_DEVICE="0000:00:04.0"

EXPECTED_VENDOR="0x8086"
EXPECTED_DEVICE="0x100e"

DRIVER_NAME="sk_e1000"
STOCK_DRIVER="e1000"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MODULE_PATH="${REPO_ROOT}/sk_e1000.ko"

SYSFS_DEVICE="/sys/bus/pci/devices/${PCI_DEVICE}"

ORIGINAL_DRIVER=""

TESTS_RUN=0


# -----------------------------------------------------------------------------
# Output helpers
# -----------------------------------------------------------------------------

pass()
{
    TESTS_RUN=$((TESTS_RUN + 1))
    printf '[PASS] %s\n' "$1"
}


fail()
{
    printf '[FAIL] %s\n' "$1" >&2
    exit 1
}


# -----------------------------------------------------------------------------
# PCI ownership helpers
# -----------------------------------------------------------------------------

current_driver()
{
    if [[ -L "${SYSFS_DEVICE}/driver" ]]; then
        basename "$(readlink "${SYSFS_DEVICE}/driver")"
    else
        printf '%s' ""
    fi
}


unbind_driver()
{
    local driver="$1"

    printf '%s' "${PCI_DEVICE}" |
        sudo tee "/sys/bus/pci/drivers/${driver}/unbind" >/dev/null
}


restore_original_driver()
{
    local driver_now

    if [[ "${ORIGINAL_DRIVER}" != "${STOCK_DRIVER}" ]]; then
        return
    fi

    driver_now="$(current_driver)"

    if [[ -n "${driver_now}" ]]; then
        return
    fi

    if [[ -e "/sys/bus/pci/drivers/${STOCK_DRIVER}/bind" ]]; then
        printf '%s' "${PCI_DEVICE}" |
            sudo tee "/sys/bus/pci/drivers/${STOCK_DRIVER}/bind" \
            >/dev/null 2>&1 || true
    fi
}


# -----------------------------------------------------------------------------
# Failure / exit cleanup
# -----------------------------------------------------------------------------
#
# The test must not leave sk_e1000 loaded when an assertion fails.
#
# If the stock e1000 driver owned the NIC before the test, restore that
# ownership when the test exits.
#

cleanup()
{
    if lsmod | grep -q "^${DRIVER_NAME}[[:space:]]"; then
        sudo rmmod "${DRIVER_NAME}" >/dev/null 2>&1 || true
    fi

    restore_original_driver
}

trap cleanup EXIT


# -----------------------------------------------------------------------------
# Start
# -----------------------------------------------------------------------------

printf '%s\n' "===================================================="
printf '%s\n' " sk_e1000 PCI/MMIO/IRQ Integration Test"
printf '%s\n' "===================================================="
printf '\n'


#
# Validate sudo credentials before changing kernel or PCI state.
#

sudo -v


# -----------------------------------------------------------------------------
# 1. PCI device exists
# -----------------------------------------------------------------------------

if [[ ! -d "${SYSFS_DEVICE}" ]]; then
    fail "PCI device ${PCI_DEVICE} does not exist"
fi

pass "PCI device ${PCI_DEVICE} exists"


# -----------------------------------------------------------------------------
# 2. PCI identity
# -----------------------------------------------------------------------------

ACTUAL_VENDOR="$(cat "${SYSFS_DEVICE}/vendor")"
ACTUAL_DEVICE="$(cat "${SYSFS_DEVICE}/device")"

if [[ "${ACTUAL_VENDOR}" != "${EXPECTED_VENDOR}" ]]; then
    fail "unexpected PCI Vendor ID: ${ACTUAL_VENDOR}"
fi

if [[ "${ACTUAL_DEVICE}" != "${EXPECTED_DEVICE}" ]]; then
    fail "unexpected PCI Device ID: ${ACTUAL_DEVICE}"
fi

pass "PCI identity is ${EXPECTED_VENDOR}:${EXPECTED_DEVICE}"


# -----------------------------------------------------------------------------
# 3. Kernel module exists
# -----------------------------------------------------------------------------

if [[ ! -f "${MODULE_PATH}" ]]; then
    fail "kernel module not found: ${MODULE_PATH}"
fi

pass "Kernel module exists"


# -----------------------------------------------------------------------------
# 4. Normalize any previous sk_e1000 development run
# -----------------------------------------------------------------------------
#
# A developer may have manually loaded sk_e1000 immediately before
# running this integration test.
#
# Remove that previous instance first so this test starts from a known
# state and validates a fresh probe().
#

if lsmod | grep -q "^${DRIVER_NAME}[[:space:]]"; then
    sudo rmmod "${DRIVER_NAME}"
fi


# -----------------------------------------------------------------------------
# 5. Record and release stock-driver ownership
# -----------------------------------------------------------------------------

ORIGINAL_DRIVER="$(current_driver)"

case "${ORIGINAL_DRIVER}" in

    "")
        ;;

    "${STOCK_DRIVER}")
        unbind_driver "${STOCK_DRIVER}"
        ;;

    *)
        fail "PCI device unexpectedly owned by ${ORIGINAL_DRIVER}"
        ;;

esac


if [[ -n "$(current_driver)" ]]; then
    fail "PCI device could not be released for testing"
fi

pass "PCI device available for custom driver"


# -----------------------------------------------------------------------------
# 6. Clear previous kernel log evidence
# -----------------------------------------------------------------------------

sudo dmesg -C


# -----------------------------------------------------------------------------
# 7. Load sk_e1000
# -----------------------------------------------------------------------------

sudo insmod "${MODULE_PATH}"

if ! lsmod | grep -q "^${DRIVER_NAME}[[:space:]]"; then
    fail "sk_e1000 module did not load"
fi

pass "sk_e1000 module loaded"


# -----------------------------------------------------------------------------
# 8. Verify PCI ownership
# -----------------------------------------------------------------------------

BOUND_DRIVER="$(current_driver)"

if [[ "${BOUND_DRIVER}" != "${DRIVER_NAME}" ]]; then
    fail "PCI device is owned by '${BOUND_DRIVER}', expected sk_e1000"
fi

pass "sk_e1000 owns ${PCI_DEVICE}"


# -----------------------------------------------------------------------------
# 9. Verify PCI ID match reached probe()
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: matched PCI device 8086:100e"; then

    fail "PCI ID match log not found"
fi

pass "PCI ID match verified"


# -----------------------------------------------------------------------------
# 10. Verify BAR0 discovery
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: BAR0 physical start="; then

    fail "BAR0 physical address log not found"
fi

if ! sudo dmesg |
    grep -q "sk_e1000: BAR0 size="; then

    fail "BAR0 size log not found"
fi

pass "BAR0 discovery verified"


# -----------------------------------------------------------------------------
# 11. Verify CTRL MMIO access
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: CTRL   = 0x"; then

    fail "CTRL MMIO read log not found"
fi

pass "CTRL register MMIO read verified"


# -----------------------------------------------------------------------------
# 12. Verify STATUS MMIO access
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: STATUS = 0x"; then

    fail "STATUS MMIO read log not found"
fi

pass "STATUS register MMIO read verified"


# -----------------------------------------------------------------------------
# 13. Preserve previous PCI/MMIO milestone validation
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: PCI/MMIO initialization PASSED"; then

    fail "PCI/MMIO initialization success log not found"
fi

pass "PCI/MMIO initialization completed"


# -----------------------------------------------------------------------------
# 14. Verify PCI bus mastering in driver log
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: PCI bus mastering enabled"; then

    fail "PCI bus-master initialization log not found"
fi

pass "PCI bus-master initialization verified"


# -----------------------------------------------------------------------------
# 15. Verify PCI COMMAND Bus Master bit while driver owns device
# -----------------------------------------------------------------------------

if ! sudo lspci -vv -s "${PCI_DEVICE#0000:}" |
    grep -qE 'Control:.*BusMaster\+'; then

    fail "PCI Bus Master bit is not enabled"
fi

pass "PCI COMMAND register reports BusMaster+"


# -----------------------------------------------------------------------------
# 16. Verify Linux IRQ handler registration
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: IRQ handler registered on IRQ"; then

    fail "IRQ registration log not found"
fi

if ! grep -q "${DRIVER_NAME}" /proc/interrupts; then
    fail "sk_e1000 not present in /proc/interrupts"
fi

pass "Linux IRQ handler registration verified"


# -----------------------------------------------------------------------------
# 17. Verify deterministic hardware interrupt was triggered
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: triggering LSC interrupt test"; then

    fail "LSC interrupt trigger log not found"
fi

pass "e1000 LSC interrupt trigger verified"


# -----------------------------------------------------------------------------
# 18. Verify interrupt reached the ISR through the hardware path
# -----------------------------------------------------------------------------
#
# Expected deterministic cause:
#
#     ICR = 0x00000004
#
# Bit 2 is Link Status Change.
#

if ! sudo dmesg |
    grep -q "sk_e1000: interrupt received ICR=0x00000004"; then

    fail "expected ICR interrupt cause was not observed"
fi

pass "ISR received expected ICR=0x00000004"


# -----------------------------------------------------------------------------
# 19. Verify LSC cause handling
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: LSC interrupt handled"; then

    fail "LSC interrupt handling log not found"
fi

pass "Link Status Change interrupt handled"


# -----------------------------------------------------------------------------
# 20. Verify interrupt synchronization completed
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: interrupt test PASSED"; then

    fail "interrupt completion validation did not pass"
fi

pass "Interrupt delivery validation completed"


# -----------------------------------------------------------------------------
# 21. Verify complete PCI/MMIO/IRQ initialization
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: PCI/MMIO/IRQ initialization PASSED"; then

    fail "complete IRQ initialization success log not found"
fi

pass "PCI/MMIO/IRQ initialization completed"


# -----------------------------------------------------------------------------
# 22. Unload driver and exercise remove()
# -----------------------------------------------------------------------------

sudo rmmod "${DRIVER_NAME}"

if lsmod | grep -q "^${DRIVER_NAME}[[:space:]]"; then
    fail "sk_e1000 module remains loaded after rmmod"
fi

pass "sk_e1000 module unloaded"


# -----------------------------------------------------------------------------
# 23. Verify remove() cleanup path
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: device removed and resources released"; then

    fail "driver cleanup log not found"
fi

pass "Driver cleanup path verified"


# -----------------------------------------------------------------------------
# 24. Verify IRQ registration was released
# -----------------------------------------------------------------------------

if grep -q "${DRIVER_NAME}" /proc/interrupts; then
    fail "sk_e1000 IRQ remains registered after module unload"
fi

pass "IRQ handler released"


# -----------------------------------------------------------------------------
# 25. Verify custom driver released PCI device
# -----------------------------------------------------------------------------

if [[ "$(current_driver)" == "${DRIVER_NAME}" ]]; then
    fail "sk_e1000 still owns PCI device after unload"
fi

pass "PCI device released by sk_e1000"


# -----------------------------------------------------------------------------
# 26. Verify PCI bus mastering was cleared
# -----------------------------------------------------------------------------
#
# remove() calls pci_clear_master() before releasing the PCI device.
#
# Perform this check before the EXIT trap optionally restores the stock
# e1000 driver, because the stock driver may enable bus mastering again.
#

if ! sudo lspci -vv -s "${PCI_DEVICE#0000:}" |
    grep -qE 'Control:.*BusMaster-'; then

    fail "PCI Bus Master bit remains enabled after driver removal"
fi

pass "PCI COMMAND register reports BusMaster- after cleanup"


# -----------------------------------------------------------------------------
# Final result
# -----------------------------------------------------------------------------

printf '\n'
printf '%s\n' "===================================================="
printf ' ALL %d PCI/MMIO/IRQ INTEGRATION CHECKS PASSED\n' \
    "${TESTS_RUN}"
printf '%s\n' "===================================================="