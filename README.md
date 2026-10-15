# TENDA CP3 EMULATION IN QEMU
Thesis project. This QEMU fork provides the `fh8626v100` machine, emulating the Fullhan FH8626V100 SoC and allowing for the firmware of the TENDA CP3 to run.


# Build
The `scripts/` directory contains a build script for an out-of-tree build of qemu-system-arm.


# Run
The `scripts/` directory contains several scripts to launch the emulator. The intended way to interact with the firmware is with `scripts/patched_start.sh` which executes the patched version of the firmware, removing 2 kernel modules that halts the regular boot process.

`scripts/start.sh` launches the emulator in suspended mode (you need to connect with gbd first) with the unpatched firmware image.

`scripts/uboot.sh` launched the emulator with the default image with a small delay and without the suspended mode. Hold 'E' to interrupt the boot process and access the U-Boot shell.

# Ports
QEMU exposes the gdb server at port 1234 (note that you need the arm gdb). the qemu monitor is available via telnet to port 8888. finally, in the patched start, you can telnet to the deivce via port 2323

You can download the gdb arm target binaries on arch based systems with

`pacman -S arm-none-eabi-gdb `

# Additional info
Please do note that in the making of this project both the virt machine, the m25p80 and the npcm_gmac files were modifed. 