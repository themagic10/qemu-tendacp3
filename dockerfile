FROM debian:12

EXPOSE 1234 8888

ENV QEMU_ROOT="/qemuroot"
ENV QEMU_SRC="$QEMU_ROOT/qemu"
ENV QEMU_IMAGES="$QEMU_ROOT/images"

# get dipendencies and make dirs

RUN apt-get update && apt-get install -y gdb git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev ninja-build \
             python3-pip python3-setuptools python3-tomli python3-wheel \
            && mkdir -p "$QEMU_SRC" \
            && mkdir -p "$QEMU_IMAGES"

# copy data
COPY tendaimages $QEMU_IMAGES

COPY qemu $QEMU_SRC

WORKDIR $QEMU_SRC

# building
RUN ./configure --disable-kvm --target-list="arm-softmmu"

RUN make -j$(nproc)


WORKDIR $QEMU_ROOT

CMD $QEMU_SRC/build/qemu-system-arm -s -S -M fullhan8626v100 \
	-cpu arm1176 \
	-m 48M \
	-d unimp,int,mmu,guest_errors -D errvirdump.log \
	-icount shift=0 -rtc clock=vm \
	-kernel $QEMU_IMAGES/uboot.dd \
	-drive file=$QEMU_IMAGES/firmware.bin,format=raw,if=mtd \
	-serial stdio -monitor telnet:0.0.0.0:8888,server,nowait