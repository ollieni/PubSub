# Makefile for the pub-sub kernel module

# Specify the compiler to use
CC := gcc-12

# If the kernel source tree is not in /lib/modules/$(shell uname -r)/build, set KDIR to the correct path
KDIR := /lib/modules/$(shell uname -r)/build

# Module name
MODULE_NAME := pubsub

# Source files
obj-m := $(MODULE_NAME).o

# Default target
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean target
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Load the module with parameters
load:
	sudo insmod $(MODULE_NAME).ko num_publishers=$(PUBLISHERS) num_subscribers=$(SUBSCRIBERS)

# Unload the module
unload:
	sudo rmmod $(MODULE_NAME)

# Display kernel messages
dmesg:
	dmesg | tail

# Additional .PHONY to specify non-file targets
.PHONY: all clean load unload dmesg
