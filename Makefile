# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR = $(CURDIR)

# Directories which require calling make recursively
ifeq ($(BOARD), pc)
# XXX: just build the kernel for now.
SUBDIR = sys include
else
SUBDIR = sys lib bin usr.bin etc include
endif

all: install

format: setup
build: setup
install: build
distclean: clean

bin-before: lib-install
usr.bin-before: lib-install

# Detecting whether initrd.cpio requires rebuilding is tricky, because even if
# this target was to depend on $(shell find sysroot -type f), then make compares
# sysroot files timestamps BEFORE recursively entering bin and installing user
# programs into sysroot. This sounds silly, but apparently make assumes no files
# appear "without their explicit target". Thus, the only thing we can do is
# forcing make to always rebuild the archive.
ifeq ($(BOARD), pc)
INITRD_DEPENDENCIES = include-install
else
INITRD_DEPENDENCIES = bin-install
endif

initrd.cpio: $(INITRD_DEPENDENCIES)
	@echo "[INITRD] Building $@..."
	cd sysroot && \
	  find -depth \( ! -name "*.dbg" -and -print \) | sort | \
	    $(CPIO) -o -R +0:+0 -F ../$@ 2> /dev/null

CLEAN-FILES += initrd.cpio

include $(TOPDIR)/config.mk

mimiker.iso: initrd.cpio sys/$(ARCH)/grub.cfg
	@echo "[ISO] Building $@..."
	$(MKDIR) iso/mimiker
	$(CP) sys/mimiker.elf iso/mimiker
	$(CP) initrd.cpio iso/mimiker
	$(MKDIR) iso/boot/grub
	$(CP) sys/$(ARCH)/grub.cfg iso/boot/grub
	grub-mkrescue -o $@ iso
	$(RM) -r iso

ifeq ($(BOARD), pc)
INSTALL-FILES += mimiker.iso
CLEAN-FILES += mimiker.iso
else
INSTALL-FILES += initrd.cpio
endif

distclean-here:
	$(RM) -r sysroot

setup:
	$(MAKE) -C include setup

test: sys-build initrd.cpio
	./run_tests.py --board $(BOARD)

PHONY-TARGETS += setup test

IMGVER = 1.8.0
IMGNAME = cahirwpz/mimiker-ci

docker-build:
	docker build . -t $(IMGNAME):latest

docker-tag:
	docker image tag $(IMGNAME):latest $(IMGNAME):$(IMGVER)

docker-push:
	docker push $(IMGNAME):latest
	docker push $(IMGNAME):$(IMGVER)

PHONY-TARGETS += docker-build docker-tag docker-push

include $(TOPDIR)/build/common.mk
