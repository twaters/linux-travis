# Ibeos Edge-1100 Secondary Boot Loader 
## (Ubuntu 22.04 Install Guide)

This is the source code for the Ibeos Edge-1100 NOR-Flash kernel used as a secondary boot stage.

This kernel is built with EFI_STUB, meaning it's bootable directly from UEFI firmware.
This kernel comes with a default configuration (defconfig) that is used to configure it in a way that it can be built with the needed components to boot, bring up the NAND storage subsystem, and either use the NAND as a root filesystem, or load another kernel/initramfs off the NAND and jump into via kexec.

The files in the initramfs directory make up a minimal filesystem that is shell-capable, and included in the kernel as a built-in initramfs. The "init" script on the root of this filesystem is where you can customize your boot commands. 
Currently, it is set up to load the Ubuntu server kernel and initramfs off the NAND, then boot that kernel via kexec.

To use the NOR flash kernel as a bootable kernel that brings up the NAND as a rootfs, you will have to build a command line into the kernel. The init script mentioned above shows an example of a kernel command line that uses a UBIFS root filesystem.

This is a kernel version 6.6.12 that has several patches to support the Ibeos Edge-1100.

### Tools Needed
* bison
* flex
* libelf Development Libraries
* build-essential (This packages gcc/g++ and a few other tools needed for building)
* bc
* amd64-microcode (This isn't installed by default on minimal installations of Ubuntu)
* DediProg (Optional, you can use a different tool if you'd like)

Your development system will need some helper tools installed for the build process:

```
apt-get update && apt -y install bison flex libelf-dev mtools build-essential bc amd64-microcode
```

#### Optional Install (DediProg)
```
apt -y install libusb-1.0-0-dev git pkg-config
git clone https://github.com/DediProgSW/SF100Linux
```

Ideally you should follow the readme on the github page to install DediProg for the latest instructions.

Per the instructions as of 6/17/2024:
```
cd SF100Linux && make && cd ..
```
This should build DediProg and return back to our root directory

### Need to clone both of these repositories to build
```
git clone git@github.com:ibeos-dev/edge1100_initramfs.git
git clone git@github.com:ibeos-dev/edge1100_kernel.git
```

For the purposes of this build document, we are building everything with a specific directory structure and in a specific working directory. This is to make it easier for the user but is certainly not a requirement to successfully build.


> home  
> │    initramfs (symbolic link)  
> └─── edge1100_initramfs  
> └─── edge1100_kernel  
> └─── SF100Linux  


initramfs needs to be symbolically linked for later in our build process. For ease of use we do that now:
```
ln -s edge1100_initramfs/initramfs
```

Now your directory structure should exactly match the one above.

### Build Process
To build the kernel, issue the following commands (you can use make -j10 for 10 jobs to speed up the process instead of just typing "make" on the last line):
```
cd edge1100_kernel
make edge1100_norflash_defconfig
make
```

At this point, the kernel resides at: 

> arch/x86/boot/bzImage

Go to the directory that contains bzImage:
```
cd arch/x86/boot
```

The SPI NOR flash IC on the Edge-1100 is 16MiB in size, Micron P/N MT25QU128ABA and the lower 8MiB is used to hold the BIOS. This region of the chip is used by the hardware to bring up the system. 
The upper 8MiB of the chip is used as a bootable "ROM disk", which is accessed using a custom Ibeos-developed UEFI ROM disk driver.

To build the custom image, the "mtools" package in Linux is best. It provides a way to make DOS/FAT images without needing to mount any files or devices. 
The following command will create an image:
```
fallocate -l 8388608 img
mformat -t 64 -h 8 -s 32 -i img
```

Now, you have an 8MiB file named "img" that can be used with mtools. mtools provides a simple dos-like interface to these images. 
For example, to list the root contents:
```
mdir -i img
```

The kernel image (bzImage) must be placed on the image in the directory EFI/BOOT/bootx64. Make the directory path with the following commands:
```
mmd -i img ::EFI
mmd -i img ::EFI/BOOT
```

The bzImage file can be placed in the image with the following command:
```
mcopy -i img bzImage ::EFI/BOOT/bootx64.efi
```

At this point, the "img" file can be used with dediprog (or similar tool) to be programmed into the chip. The same image can be used again, with only repeating the mcopy command with your new image.

WARNING: Address 0x820000 of the flash chip (address 0x20000 in your 8MiB image) cannot have the value 0x55aa55aa or the system will not boot. This is a side effect of how the boot process works, and that explanation is out of scope here. 
By our calculations, the odds of this happening are 1 in 1.8e16, so it's pretty unlikely.

In order to program the upper 8MiB of the flash chip using the dediprog programmer, you can use the following command:
```
../../../../SF100Linux/dpcmd -u img -a0x800000
```

This will update the upper half of the chip, while leaving the BIOS image alone. You can also concatenate your 8MiB image to the end of the BIOS image and program the entire chip with that image.

The BIOS chip can also be programmed from Linux, but this requires a BIOS update and setting of a BIOS menu option to enable exposure of the chip. 
There's also a patch required (included in the kernel in this tarball) that fixes a bug in the AMD SPI device. Since this requires a BIOS update, it's out of scope for this document.