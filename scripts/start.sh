#!/usr/bin/env bash
set -eo pipefail

cd "$(dirname "$0")"
cd ../build/

./qemu-system-arm -s -S -M fullhan8626v100 \
	-cpu arm1176 \
	-m 64M \
	-d unimp,int,mmu,guest_errors -D errvirdump.log \
	-icount shift=0 -rtc clock=vm \
	-kernel ../tendaimages/uboot.dd \
	-drive file=../tendaimages/firmware.bin,format=raw,if=mtd \
	-serial stdio -monitor telnet:0.0.0.0:8888,server,nowait