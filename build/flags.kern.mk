# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# This is a common makefile used to supplement the compilation flags with
# the kernel specific flags.
#
# The following make variables are set by the including makefile:
# -LOCKDEP: Lock dependency validator flag.

include $(TOPDIR)/build/flags.mk

CFLAGS   += -fno-builtin -nostdinc -nostdlib -ffreestanding -DLOCKDEP=$(LOCKDEP)
CPPFLAGS += -I$(TOPDIR)/include -D_KERNEL
LDFLAGS  += -nostdlib

KERNEL := 1
