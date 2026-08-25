# SPDX-License-Identifier: GPL-2.0
#
# Makefile
#
# Build configuration for the sk_e1000 Linux PCIe network driver
# and its hardware-independent user-space unit tests.
#
# Author: Santosh Kumar
#
# Build responsibilities:
#
#   make
#       Build the Linux kernel module.
#
#   make unit-test
#       Build and execute Unity-based unit tests in user space.
#
#   make unit-test-build
#       Build the unit-test executable without running it.
#
#   make clean
#       Remove kernel-module and unit-test build artifacts.
#
# Hardware-dependent integration tests are intentionally kept
# separate because they require the QEMU development VM, root
# privileges, and access to the dedicated emulated e1000 device.


# ============================================================================
# Kernel module
# ============================================================================

#
# Final kernel module:
#
#     sk_e1000.ko
#
obj-m += sk_e1000.o


#
# sk_e1000.ko is a composite kernel module built from:
#
#     src/sk_e1000.c
#         PCI lifecycle, MMIO access, interrupt handling,
#         and hardware/resource management.
#
#     src/sk_e1000_logic.c
#         Hardware-independent decision logic shared with
#         user-space unit tests.
#
sk_e1000-objs := \
	src/sk_e1000.o \
	src/sk_e1000_logic.o


#
# Project headers.
#
# $(src) is supplied by Kbuild and identifies the root of this
# external kernel-module source tree.
#
ccflags-y += -I$(src)/include


#
# Build against the currently running kernel unless KDIR is
# explicitly supplied by the caller.
#
# Example inside the QEMU VM:
#
#     /lib/modules/6.8.0-138-generic/build
#
KDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)


# ============================================================================
# User-space unit tests
# ============================================================================

#
# Unit tests run outside the kernel and exercise only pure,
# hardware-independent production logic.
#
# The same src/sk_e1000_logic.c implementation is linked into:
#
#     1. sk_e1000.ko
#     2. the Unity unit-test executable
#
# This avoids duplicating production logic inside the tests.
#

UNIT_CC := gcc

UNITY_DIR := third_party/unity/src
UNIT_BUILD_DIR := build/tests

UNIT_TEST_BIN := $(UNIT_BUILD_DIR)/test_irq_logic

UNIT_TEST_SRCS := \
	tests/unit/test_irq_logic.c \
	src/sk_e1000_logic.c \
	$(UNITY_DIR)/unity.c


#
# Keep compiler diagnostics strict for project-owned unit-test code.
#
# -MMD/-MP generate dependency files so changes to included headers
# cause the test binary to be rebuilt automatically.
#
UNIT_CFLAGS := \
	-std=c11 \
	-Wall \
	-Wextra \
	-Werror \
	-O2 \
	-MMD \
	-MP \
	-Iinclude \
	-I$(UNITY_DIR)


# ============================================================================
# Public targets
# ============================================================================

.PHONY: all module unit-test unit-test-build clean


#
# Default target: build the kernel module.
#
all: module


#
# Build the out-of-tree Linux kernel module.
#
module:
	@test -d "$(KDIR)" || { \
		echo "ERROR: kernel build directory not found:"; \
		echo "       $(KDIR)"; \
		echo; \
		echo "Build sk_e1000.ko inside the QEMU VM where the"; \
		echo "matching Linux kernel headers are installed."; \
		exit 1; \
	}
	$(MAKE) -C $(KDIR) M=$(PWD) modules


#
# Build and execute the Unity unit tests.
#
# These tests do not require QEMU, a PCI device, root privileges,
# or Linux kernel headers.
#
unit-test: unit-test-build
	./$(UNIT_TEST_BIN)


#
# Build the Unity unit-test executable.
#
unit-test-build: $(UNIT_TEST_BIN)


$(UNIT_TEST_BIN): $(UNIT_TEST_SRCS) include/sk_e1000_logic.h
	@mkdir -p $(UNIT_BUILD_DIR)
	$(UNIT_CC) $(UNIT_CFLAGS) $(UNIT_TEST_SRCS) -o $@


# ============================================================================
# Cleanup
# ============================================================================

#
# Remove both user-space test artifacts and kernel-module artifacts.
#
# The kernel cleanup is performed only when a matching Kbuild
# directory exists. This allows "make clean" to remain usable from
# WSL, where Microsoft WSL kernel headers may not be installed.
#
clean:
	@rm -rf build
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		echo "Kernel build directory not found: $(KDIR)"; \
		echo "Skipped kernel-module clean; user-space artifacts removed."; \
	fi


#
# Include automatically generated user-space header dependencies.
#
-include $(UNIT_BUILD_DIR)/*.d