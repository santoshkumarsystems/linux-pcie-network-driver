# Linux PCIe Network Driver
#
# Author: Santosh Kumar
#
# Build the sk_e1000 Linux kernel module using the kernel's
# external-module Kbuild system.

obj-m += sk_e1000.o

# Source file for the module.
sk_e1000-objs := src/sk_e1000.o

# Kernel build directory for the currently running kernel.
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current project directory.
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean