#!/usr/bin/env bash
#
# prepare_rx_test_device.sh
#
# Author: Santosh Kumar
# Project: linux-pcie-network-driver
#
# Purpose
# -------
# Prepare the dedicated QEMU-emulated Intel 82540EM NIC for the custom
# sk_e1000 driver before running the opt-in deterministic RX DMA test.
#
# Why this script exists
# ----------------------
# The Linux guest may automatically bind PCI device 0000:00:04.0 to the
# stock e1000 driver during boot.
#
# The custom sk_e1000 module cannot probe or own that PCI function while
# another driver is already bound to it.
#
# This script performs the repeatable preparation sequence:
#
#     unload any previously loaded sk_e1000 module
#          ->
#     set driver_override=sk_e1000
#          ->
#     unbind the stock e1000 driver when necessary
#          ->
#     verify that the PCI device is unbound and ready
#
# It intentionally does NOT load sk_e1000.ko. The RX validation module is
# loaded separately with:
#
#     sudo insmod ./sk_e1000.ko run_rx_test=1
#
# Keeping preparation and test execution separate makes the 15-second RX
# injection window deterministic and easy to coordinate with the WSL host
# packet injector.
#
# Expected hardware
# -----------------
# PCI BDF    : 0000:00:04.0
# Vendor ID  : 0x8086
# Device ID  : 0x100e
# Device     : Intel 82540EM Gigabit Ethernet Controller (QEMU-emulated)
#
# Run from inside the VM:
#
#     cd ~/e1000-driver-test
#     ./tests/integration/prepare_rx_test_device.sh
#
# Expected final state:
#
#     DEVICE READY: 0000:00:04.0 is unbound
#
# Then start the RX test:
#
#     sudo insmod ./sk_e1000.ko run_rx_test=1
#

set -euo pipefail


# -----------------------------------------------------------------------------
# FIXED LAB IDENTIFIERS
# -----------------------------------------------------------------------------

PCI_BDF="0000:00:04.0"
EXPECTED_VENDOR="0x8086"
EXPECTED_DEVICE="0x100e"

CUSTOM_DRIVER="sk_e1000"
STOCK_DRIVER="e1000"

SYSFS_DEVICE="/sys/bus/pci/devices/${PCI_BDF}"


# -----------------------------------------------------------------------------
# HELPER: READ CURRENTLY BOUND DRIVER
# -----------------------------------------------------------------------------
#
# Prints the basename of the currently bound driver, or an empty string when
# the PCI device is not bound to any driver.
#
current_driver()
{
    local link

    link="$(readlink "${SYSFS_DEVICE}/driver" 2>/dev/null || true)"

    if [[ -z "${link}" ]]; then
        printf '%s\n' ""
        return 0
    fi

    basename "${link}"
}


# -----------------------------------------------------------------------------
# VERIFY EXPECTED PCI DEVICE
# -----------------------------------------------------------------------------

if [[ ! -d "${SYSFS_DEVICE}" ]]; then
    echo "[FAIL] PCI device ${PCI_BDF} does not exist" >&2
    exit 1
fi

vendor="$(cat "${SYSFS_DEVICE}/vendor")"
device="$(cat "${SYSFS_DEVICE}/device")"

if [[ "${vendor}" != "${EXPECTED_VENDOR}" ||
      "${device}" != "${EXPECTED_DEVICE}" ]]; then

    echo "[FAIL] unexpected PCI identity at ${PCI_BDF}" >&2
    echo "       observed: vendor=${vendor} device=${device}" >&2
    echo "       expected: vendor=${EXPECTED_VENDOR} device=${EXPECTED_DEVICE}" >&2
    exit 1
fi

echo "[PASS] PCI device ${PCI_BDF} is Intel ${EXPECTED_VENDOR}:${EXPECTED_DEVICE}"


# -----------------------------------------------------------------------------
# REMOVE PREVIOUS CUSTOM-DRIVER INSTANCE
# -----------------------------------------------------------------------------
#
# rmmod automatically invokes the driver's remove() callback for any device it
# owns, so this also returns the PCI function to an unbound state when a prior
# sk_e1000 test run is still active.
#

if lsmod | awk '{print $1}' | grep -qx "${CUSTOM_DRIVER}"; then
    echo "[INFO] unloading previously loaded ${CUSTOM_DRIVER} module"
    sudo rmmod "${CUSTOM_DRIVER}"
else
    echo "[INFO] ${CUSTOM_DRIVER} module is not currently loaded"
fi


# -----------------------------------------------------------------------------
# FORCE FUTURE MATCHING TO THE CUSTOM DRIVER
# -----------------------------------------------------------------------------
#
# driver_override tells the Linux PCI core that this specific PCI function
# should bind only to sk_e1000 until the override is cleared.
#
# This prevents the stock e1000 driver from immediately reclaiming the device
# while we prepare the custom-driver test.
#

echo "${CUSTOM_DRIVER}" | sudo tee "${SYSFS_DEVICE}/driver_override" >/dev/null

echo "[PASS] driver_override set to ${CUSTOM_DRIVER}"


# -----------------------------------------------------------------------------
# UNBIND THE STOCK DRIVER WHEN NECESSARY
# -----------------------------------------------------------------------------

driver="$(current_driver)"

case "${driver}" in
    "")
        echo "[INFO] ${PCI_BDF} is already unbound"
        ;;

    "${STOCK_DRIVER}")
        echo "[INFO] unbinding stock ${STOCK_DRIVER} driver from ${PCI_BDF}"

        echo "${PCI_BDF}" |
            sudo tee "/sys/bus/pci/drivers/${STOCK_DRIVER}/unbind" >/dev/null
        ;;

    "${CUSTOM_DRIVER}")
        # This should normally have disappeared when rmmod ran above.
        # Refuse to continue silently if the state is inconsistent.
        echo "[FAIL] ${PCI_BDF} is still bound to ${CUSTOM_DRIVER} after rmmod" >&2
        exit 1
        ;;

    *)
        echo "[FAIL] ${PCI_BDF} is bound to unexpected driver: ${driver}" >&2
        echo "       refusing to unbind an unknown driver automatically" >&2
        exit 1
        ;;
esac


# -----------------------------------------------------------------------------
# FINAL OWNERSHIP CHECK
# -----------------------------------------------------------------------------

driver="$(current_driver)"

if [[ -n "${driver}" ]]; then
    echo "[FAIL] ${PCI_BDF} is still bound to driver: ${driver}" >&2
    exit 1
fi


echo
echo "========================================================"
echo " sk_e1000 RX Test Device Preparation"
echo "========================================================"
echo
echo "[PASS] PCI identity verified"
echo "[PASS] custom driver override configured"
echo "[PASS] stock driver ownership removed"
echo
echo "DEVICE READY: ${PCI_BDF} is unbound"
echo
echo "Next, from this VM:"
echo
echo "    sudo insmod ./sk_e1000.ko run_rx_test=1"
echo
echo "Then, within the RX-test timeout, inject the frame from WSL:"
echo
echo "    sudo python3 tests/integration/inject_rx_frame.py"
echo
