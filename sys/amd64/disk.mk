# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR := $(realpath ../..)
DISK_IMAGE := $(TOPDIR)/disk.img

SECTOR_SIZE := 512

# 550 MiB is the recommended ESP size.
ESP_SIZE := $(shell echo '550 * 2^20' | bc)

# LBA of the first sector.
ESP_START := 2048

# Number of sectors composing ESP.
ESP_SECTOR_COUNT := $(shell echo '$(ESP_SIZE) / $(SECTOR_SIZE)' | bc)

# LBA of the last sector.
ESP_END := $(shell echo '$(ESP_START) + $(ESP_SIZE) / $(SECTOR_SIZE) - 1' | bc)

# ESP image.
ESP_IMAGE := $(CURDIR)/esp.img

# Kernel image.
KERNEL_IMAGE := $(TOPDIR)/sys/mimiker.elf

# Initial RAM disk.
INITRD := $(TOPDIR)/initrd.cpio

# Bootloader UEFI application.
UEFI_APP := $(CURDIR)/BOOTX64.EFI

# Bootloader configuration.
CONFIG := $(CURDIR)/grub.cfg

disk-init: $(DISK_IMAGE)
	@echo "Create ESP"
	sgdisk -z $(DISK_IMAGE)
	sgdisk -n 1:$(ESP_START):$(ESP_END) \
	       -t 1:ef00 \
	       -c 1:esp \
	       $(DISK_IMAGE)
	partprobe

$(UEFI_APP): $(CONFIG)
	@echo "Creata a standalone GRUB binary"
	PATH=$(TOPDIR)/grub/bin:$$PATH \
		grub-mkstandalone -O x86_64-efi -o $(UEFI_APP) \
				  "boot/grub/grub.cfg=$(CONFIG)"

disk-boot-image: $(KERNEL_IMAGE) $(INITRD) $(UEFI_APP)
	@echo "Create ESP image"
	dd if=/dev/zero of=$(ESP_IMAGE) bs=$(SECTOR_SIZE) \
	   count=$(ESP_SECTOR_COUNT)

	@echo "Format the partition for FAT32"
	mkdosfs -F 32 $(ESP_IMAGE)

	@echo "Create the top level ESP directory"
	mmd -i $(ESP_IMAGE) ::/EFI

	@echo "Copy kernel image and initrd to the partition"
	mmd -i $(ESP_IMAGE) ::/EFI/mimiker
	mcopy -i $(ESP_IMAGE) $(KERNEL_IMAGE) ::/EFI/mimiker
	mcopy -i $(ESP_IMAGE) $(INITRD) ::/EFI/mimiker

	@echo "Place the bootloader at the default UEFI application load path"
	mmd -i $(ESP_IMAGE) ::/EFI/BOOT
	mcopy -i $(ESP_IMAGE) $(UEFI_APP) ::/EFI/BOOT

disk: disk-init disk-boot-image
	@echo "Save the ESP image on the disk"
	dd if=$(ESP_IMAGE) of=$(DISK_IMAGE) bs=$(SECTOR_SIZE) \
	   count=$(ESP_SECTOR_COUNT) seek=$(ESP_START) conv=notrunc

clean-here:
	$(RM) $(ESP_IMAGE)

distclean-here:
	$(RM) $(UEFI_APP)

