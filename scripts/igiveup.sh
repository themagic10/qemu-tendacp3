#!/usr/bin/env bash
set -eo pipefail

cd "$(dirname "$0")"
cd ../build/


./qemu-system-arm -s -M fullhan8626v100 \
	-cpu arm1176 \
	-m 64M \
	-d unimp,mmu,guest_errors -D errvirdump.log \
	-icount shift=0 -rtc clock=vm \
	-kernel ../tendaimages/uboot.dd \
	-drive file=../tendaimages/igiveup.bin,format=raw,if=mtd,snapshot=on \
	-serial stdio -monitor telnet:0.0.0.0:8888,server,nowait
