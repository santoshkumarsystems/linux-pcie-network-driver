#!/usr/bin/env bash
#
# test_pci_mmio_dma_irq.sh
#
# Integration validation for the sk_e1000 Linux network driver.
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
#   - custom driver binding
#   - BAR0 discovery
#   - MMIO register access
#   - PCI bus mastering
#   - DMA addressing configuration
#   - independent RX descriptor-ring DMA allocation
#   - independent TX descriptor-ring DMA allocation
#   - RX/TX device-visible DMA address creation
#   - RX descriptor-ring base/length MMIO programming
#   - TX descriptor-ring base/length MMIO programming
#   - descriptor-ring register readback validation
#   - verification that RX/TX packet engines remain disabled
#   - legacy INTx interrupt registration
#   - e1000 interrupt generation through ICS
#   - interrupt delivery through Linux
#   - ICR cause handling
#   - Link Status Change interrupt handling
#   - TX descriptor-ring DMA release
#   - RX descriptor-ring DMA release
#   - IRQ release
#   - PCI bus-master cleanup
#   - PCI device release
#
# Hardware-independent interrupt and ring-management logic is tested
# separately with Unity under tests/unit/.
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


# Intel 82540EM legacy RX/TX descriptor-ring geometry.
#
# Each legacy descriptor is 16 bytes and this driver currently owns
# 64 descriptors per ring:
#
#     64 * 16 = 1024 bytes
#
# RX and TX use independent DMA-coherent descriptor-ring allocations.
#

EXPECTED_RING_COUNT="64"
EXPECTED_RING_SIZE="1024"


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

printf '%s\n' "========================================================"
printf '%s\n' " sk_e1000 PCI/MMIO/DMA/Ring/IRQ Integration Test"
printf '%s\n' "========================================================"
printf '\n'


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
# 4. Normalize a previous custom-driver development run
# -----------------------------------------------------------------------------

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
# 13. Preserve PCI/MMIO milestone validation
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: PCI/MMIO initialization PASSED"; then

    fail "PCI/MMIO initialization success log not found"
fi


pass "PCI/MMIO initialization completed"


# -----------------------------------------------------------------------------
# 14. Verify PCI bus mastering initialization
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: PCI bus mastering enabled"; then

    fail "PCI bus-master initialization log not found"
fi


pass "PCI bus-master initialization verified"


# -----------------------------------------------------------------------------
# 15. Verify PCI COMMAND Bus Master bit
# -----------------------------------------------------------------------------

if ! sudo lspci -vv -s "${PCI_DEVICE#0000:}" |
    grep -qE 'Control:.*BusMaster\+'; then

    fail "PCI Bus Master bit is not enabled"
fi


pass "PCI COMMAND register reports BusMaster+"


# -----------------------------------------------------------------------------
# 16. Verify DMA addressing configuration
# -----------------------------------------------------------------------------
#
# The driver prefers 64-bit DMA addressing and can fall back to 32-bit.
#
# Do not hard-code 64-bit into the test because the negotiated width is
# platform-dependent.
#

DMA_BITS="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: DMA addressing configured: \([0-9][0-9]*\)-bit.*/\1/p' |
    tail -n 1
)"


case "${DMA_BITS}" in

    64|32)
        ;;

    *)
        fail "valid DMA addressing configuration log not found"
        ;;

esac


pass "DMA addressing configured (${DMA_BITS}-bit)"


# -----------------------------------------------------------------------------
# 17. Verify RX descriptor-ring geometry
# -----------------------------------------------------------------------------
#
# The driver owns one DMA-coherent RX descriptor ring containing the
# configured number of Intel legacy receive descriptors.
#

if ! sudo dmesg |
    grep -q \
    "sk_e1000: RX descriptor ring allocated count=${EXPECTED_RING_COUNT} size=${EXPECTED_RING_SIZE} bytes"; then

    fail "expected RX descriptor-ring allocation log not found"
fi


pass \
    "RX descriptor ring verified (${EXPECTED_RING_COUNT} descriptors, ${EXPECTED_RING_SIZE} bytes)"


# -----------------------------------------------------------------------------
# 18. Verify RX device-visible DMA address
# -----------------------------------------------------------------------------
#
# A DMA address is not assumed to equal the CPU virtual address.
#
# DMA address zero can be valid on some platforms, so this test verifies
# that Linux supplied and the driver logged an address without imposing
# a non-zero requirement.
#

RX_DMA_ADDRESS="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: RX descriptor ring DMA address=\(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' |
    tail -n 1
)"


if [[ -z "${RX_DMA_ADDRESS}" ]]; then
    fail "RX descriptor-ring DMA address log not found"
fi


pass \
    "RX device-visible DMA address recorded (${RX_DMA_ADDRESS})"


# -----------------------------------------------------------------------------
# 19. Verify TX descriptor-ring geometry
# -----------------------------------------------------------------------------
#
# TX owns a separate DMA-coherent descriptor ring rather than sharing
# the receive descriptor allocation.
#

if ! sudo dmesg |
    grep -q \
    "sk_e1000: TX descriptor ring allocated count=${EXPECTED_RING_COUNT} size=${EXPECTED_RING_SIZE} bytes"; then

    fail "expected TX descriptor-ring allocation log not found"
fi


pass \
    "TX descriptor ring verified (${EXPECTED_RING_COUNT} descriptors, ${EXPECTED_RING_SIZE} bytes)"


# -----------------------------------------------------------------------------
# 20. Verify TX device-visible DMA address
# -----------------------------------------------------------------------------

TX_DMA_ADDRESS="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: TX descriptor ring DMA address=\(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' |
    tail -n 1
)"


if [[ -z "${TX_DMA_ADDRESS}" ]]; then
    fail "TX descriptor-ring DMA address log not found"
fi


pass \
    "TX device-visible DMA address recorded (${TX_DMA_ADDRESS})"


# -----------------------------------------------------------------------------
# 21. Verify descriptor-ring DMA initialization milestone
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q \
    "sk_e1000: RX/TX descriptor-ring DMA allocation PASSED"; then

    fail "RX/TX descriptor-ring DMA initialization log not found"
fi


pass "RX/TX descriptor-ring DMA initialization completed"


# -----------------------------------------------------------------------------
# 22. Verify RX descriptor-ring hardware programming
# -----------------------------------------------------------------------------
#
# The driver programs RDBAL/RDBAH/RDLEN, reads those registers back, and
# reports success only if the programmed values match the DMA allocation
# and expected ring length.
#
# This test independently compares the programmed RX base reported by the
# driver against the RX DMA address captured earlier in this run.
#

RX_PROGRAMMED_BASE="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: RX descriptor-ring registers programmed base=\(0x[0-9a-fA-F][0-9a-fA-F]*\) length=[0-9][0-9]* bytes.*/\1/p' |
    tail -n 1
)"

RX_PROGRAMMED_LENGTH="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: RX descriptor-ring registers programmed base=0x[0-9a-fA-F][0-9a-fA-F]* length=\([0-9][0-9]*\) bytes.*/\1/p' |
    tail -n 1
)"


if [[ -z "${RX_PROGRAMMED_BASE}" ]]; then
    fail "RX descriptor-ring hardware-programming log not found"
fi


if [[ "${RX_PROGRAMMED_BASE}" != "${RX_DMA_ADDRESS}" ]]; then
    fail \
        "RX programmed base ${RX_PROGRAMMED_BASE} does not match DMA address ${RX_DMA_ADDRESS}"
fi


if [[ "${RX_PROGRAMMED_LENGTH}" != "${EXPECTED_RING_SIZE}" ]]; then
    fail \
        "RX programmed length ${RX_PROGRAMMED_LENGTH} does not match expected ${EXPECTED_RING_SIZE}"
fi


pass \
    "RX hardware ring programming verified (base=${RX_PROGRAMMED_BASE}, length=${RX_PROGRAMMED_LENGTH})"


# -----------------------------------------------------------------------------
# 23. Verify TX descriptor-ring hardware programming
# -----------------------------------------------------------------------------
#
# The driver performs the same write/readback validation for
# TDBAL/TDBAH/TDLEN. This test confirms that the programmed base corresponds
# to the TX DMA allocation captured earlier in this run.
#

TX_PROGRAMMED_BASE="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: TX descriptor-ring registers programmed base=\(0x[0-9a-fA-F][0-9a-fA-F]*\) length=[0-9][0-9]* bytes.*/\1/p' |
    tail -n 1
)"

TX_PROGRAMMED_LENGTH="$(
    sudo dmesg |
    sed -n \
        's/.*sk_e1000: TX descriptor-ring registers programmed base=0x[0-9a-fA-F][0-9a-fA-F]* length=\([0-9][0-9]*\) bytes.*/\1/p' |
    tail -n 1
)"


if [[ -z "${TX_PROGRAMMED_BASE}" ]]; then
    fail "TX descriptor-ring hardware-programming log not found"
fi


if [[ "${TX_PROGRAMMED_BASE}" != "${TX_DMA_ADDRESS}" ]]; then
    fail \
        "TX programmed base ${TX_PROGRAMMED_BASE} does not match DMA address ${TX_DMA_ADDRESS}"
fi


if [[ "${TX_PROGRAMMED_LENGTH}" != "${EXPECTED_RING_SIZE}" ]]; then
    fail \
        "TX programmed length ${TX_PROGRAMMED_LENGTH} does not match expected ${EXPECTED_RING_SIZE}"
fi


pass \
    "TX hardware ring programming verified (base=${TX_PROGRAMMED_BASE}, length=${TX_PROGRAMMED_LENGTH})"


# -----------------------------------------------------------------------------
# 24. Verify RX/TX packet engines remained disabled
# -----------------------------------------------------------------------------
#
# Packet buffers and hardware head/tail ownership are not initialized yet.
# The driver therefore requires both packet engines to remain disabled while
# descriptor-ring base addresses and lengths are programmed.
#
# sk_e1000_program_descriptor_ring_addressing() verifies RCTL.EN and TCTL.EN
# after register programming and readback before emitting this success log.
#

if ! sudo dmesg |
    grep -q \
    "sk_e1000: descriptor-ring base/length programming PASSED with RX/TX engines disabled"; then

    fail "descriptor-ring programming safety validation log not found"
fi


pass "RX/TX descriptor-ring programming completed with packet engines disabled"


# -----------------------------------------------------------------------------
# 25. Verify Linux IRQ handler registration
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
# 26. Verify deterministic hardware interrupt trigger
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: triggering LSC interrupt test"; then

    fail "LSC interrupt trigger log not found"
fi


pass "e1000 LSC interrupt trigger verified"


# -----------------------------------------------------------------------------
# 27. Verify interrupt reached ISR
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: interrupt received ICR=0x00000004"; then

    fail "expected ICR interrupt cause was not observed"
fi


pass "ISR received expected ICR=0x00000004"


# -----------------------------------------------------------------------------
# 28. Verify Link Status Change handling
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: LSC interrupt handled"; then

    fail "LSC interrupt handling log not found"
fi


pass "Link Status Change interrupt handled"


# -----------------------------------------------------------------------------
# 29. Verify interrupt synchronization completed
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q "sk_e1000: interrupt test PASSED"; then

    fail "interrupt completion validation did not pass"
fi


pass "Interrupt delivery validation completed"


# -----------------------------------------------------------------------------
# 30. Verify complete initialization
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q \
    "sk_e1000: PCI/MMIO/DMA/RING/IRQ initialization PASSED"; then

    fail "complete DMA/RING/IRQ initialization success log not found"
fi


pass "PCI/MMIO/DMA/RING/IRQ initialization completed"


# -----------------------------------------------------------------------------
# 31. Unload driver and exercise remove()
# -----------------------------------------------------------------------------

sudo rmmod "${DRIVER_NAME}"


if lsmod | grep -q "^${DRIVER_NAME}[[:space:]]"; then
    fail "sk_e1000 module remains loaded after rmmod"
fi


pass "sk_e1000 module unloaded"


# -----------------------------------------------------------------------------
# 32. Verify TX descriptor-ring DMA release
# -----------------------------------------------------------------------------
#
# TX was allocated after RX and therefore is released first.
#

if ! sudo dmesg |
    grep -q \
    "sk_e1000: TX descriptor ring DMA memory released"; then

    fail "TX descriptor-ring DMA cleanup log not found"
fi


pass "TX descriptor-ring DMA memory released"


# -----------------------------------------------------------------------------
# 33. Verify RX descriptor-ring DMA release
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q \
    "sk_e1000: RX descriptor ring DMA memory released"; then

    fail "RX descriptor-ring DMA cleanup log not found"
fi


pass "RX descriptor-ring DMA memory released"


# -----------------------------------------------------------------------------
# 34. Verify complete driver cleanup path
# -----------------------------------------------------------------------------

if ! sudo dmesg |
    grep -q \
    "sk_e1000: device removed and resources released"; then

    fail "driver cleanup log not found"
fi


pass "Driver cleanup path verified"


# -----------------------------------------------------------------------------
# 35. Verify IRQ registration was released
# -----------------------------------------------------------------------------

if grep -q "${DRIVER_NAME}" /proc/interrupts; then
    fail "sk_e1000 IRQ remains registered after module unload"
fi


pass "IRQ handler released"


# -----------------------------------------------------------------------------
# 36. Verify custom driver released PCI device
# -----------------------------------------------------------------------------

if [[ "$(current_driver)" == "${DRIVER_NAME}" ]]; then
    fail "sk_e1000 still owns PCI device after unload"
fi


pass "PCI device released by sk_e1000"


# -----------------------------------------------------------------------------
# 37. Verify PCI bus mastering was cleared
# -----------------------------------------------------------------------------
#
# Perform this before the EXIT trap potentially restores the stock e1000
# driver because the stock driver may enable bus mastering again.
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
printf '%s\n' "========================================================"
printf ' ALL %d PCI/MMIO/DMA/RING/IRQ INTEGRATION CHECKS PASSED\n' \
    "${TESTS_RUN}"
printf '%s\n' "========================================================"