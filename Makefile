# SPDX-License-Identifier: GPL-2.0
#
# Makefile
#
# Build configuration for the sk_e1000 Linux network driver
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
#       Build and execute all Unity-based unit tests in user space.
#
#   make unit-test-build
#       Build all unit-test executables without running them.
#
#   make clean
#       Remove kernel-module and unit-test build artifacts.
#
# Hardware-dependent integration tests remain separate because they
# require the QEMU development VM, root privileges, and access to the
# dedicated Intel 82540EM device.


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
#         PCI lifecycle, BAR/MMIO access, interrupt handling,
#         initialization, teardown, and device ownership.
#
#     src/sk_e1000_logic.c
#         Hardware-independent interrupt decision logic shared
#         with user-space unit tests.
#
#     src/sk_e1000_dma.c
#         Linux DMA API integration, DMA mask negotiation,
#         coherent allocation, and DMA resource cleanup.
#
#     src/sk_e1000_ring.c
#         Hardware-independent descriptor-ring index and
#         producer/consumer accounting logic shared with
#         user-space unit tests.
#
sk_e1000-objs := \
	src/sk_e1000.o \
	src/sk_e1000_logic.o \
	src/sk_e1000_dma.o \
	src/sk_e1000_ring.o


#
# Project headers.
#
# $(src) is provided by Kbuild and identifies the root directory
# of this external kernel-module source tree.
#
# This allows source files to use project headers such as:
#
#     #include "sk_e1000_logic.h"
#     #include "sk_e1000_dma.h"
#     #include "sk_e1000_desc.h"
#     #include "sk_e1000_ring.h"
#
ccflags-y += -I$(src)/include


#
# Build against the currently running kernel unless KDIR is
# explicitly supplied by the caller.
#
# Inside the QEMU VM this resolves to the matching Ubuntu kernel
# build directory.
#
KDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)


# ============================================================================
# User-space unit tests
# ============================================================================

#
# Unit tests run outside the kernel and exercise pure,
# hardware-independent production logic.
#
# Production implementations are shared directly with the kernel module:
#
#     src/sk_e1000_logic.c
#         interrupt decision logic
#
#     src/sk_e1000_ring.c
#         producer/consumer ring mathematics
#
# Linux PCI, MMIO, interrupt, and DMA APIs are intentionally not mocked.
# Hardware-dependent behavior is validated through QEMU integration tests.
#

UNIT_CC := gcc

UNITY_DIR := third_party/unity/src
UNIT_BUILD_DIR := build/tests


#
# Individual Unity test executables.
#
IRQ_TEST_BIN := $(UNIT_BUILD_DIR)/test_irq_logic
RING_TEST_BIN := $(UNIT_BUILD_DIR)/test_ring_logic

UNIT_TEST_BINS := \
	$(IRQ_TEST_BIN) \
	$(RING_TEST_BIN)


#
# IRQ unit-test sources.
#
IRQ_TEST_SRCS := \
	tests/unit/test_irq_logic.c \
	src/sk_e1000_logic.c \
	$(UNITY_DIR)/unity.c


#
# Descriptor-ring unit-test sources.
#
RING_TEST_SRCS := \
	tests/unit/test_ring_logic.c \
	src/sk_e1000_ring.c \
	$(UNITY_DIR)/unity.c


#
# Compiler diagnostics for project-owned unit-test code.
#
# -MMD and -MP generate dependency information so changes to included
# project headers cause the corresponding test executable to rebuild.
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
# Build and execute all Unity unit tests.
#
# These tests do not require:
#
#     - QEMU
#     - a PCI device
#     - root privileges
#     - Linux kernel headers
#
unit-test: unit-test-build
	@echo
	@echo "=== IRQ LOGIC UNIT TESTS ==="
	./$(IRQ_TEST_BIN)
	@echo
	@echo "=== RING LOGIC UNIT TESTS ==="
	./$(RING_TEST_BIN)


#
# Build all Unity unit-test executables.
#
unit-test-build: $(UNIT_TEST_BINS)


#
# IRQ decision-logic test executable.
#
$(IRQ_TEST_BIN): $(IRQ_TEST_SRCS) include/sk_e1000_logic.h
	@mkdir -p $(UNIT_BUILD_DIR)
	$(UNIT_CC) $(UNIT_CFLAGS) $(IRQ_TEST_SRCS) -o $@


#
# Descriptor-ring producer/consumer test executable.
#
$(RING_TEST_BIN): $(RING_TEST_SRCS) include/sk_e1000_ring.h
	@mkdir -p $(UNIT_BUILD_DIR)
	$(UNIT_CC) $(UNIT_CFLAGS) $(RING_TEST_SRCS) -o $@


# ============================================================================
# Cleanup
# ============================================================================

#
# Remove both user-space test artifacts and kernel-module artifacts.
#
# Kernel cleanup is performed only when the matching Kbuild directory
# exists. This keeps "make clean" usable from WSL even when Microsoft
# WSL kernel headers are not installed.
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
# Automatically generated user-space header dependencies.
#
-include $(UNIT_BUILD_DIR)/*.d