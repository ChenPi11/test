# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux UFS (BSD FFS) read-only driver.
#
# Build as an out-of-tree kernel module:
#
#   make            – build linux_ufs.ko
#   make clean      – remove build artefacts
#   make install    – install linux_ufs.ko into the running kernel's module dir
#   make load       – insmod the freshly built module
#   make unload     – rmmod the module
#

obj-m := linux_ufs.o
linux_ufs-objs := super.o inode.o dir.o file.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

load:
	sudo insmod linux_ufs.ko

unload:
	sudo rmmod linux_ufs

.PHONY: all clean install load unload
