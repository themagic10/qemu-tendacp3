#!/usr/bin/env bash
set -eo pipefail

cd "$(dirname "$0")"
cd ../

mkdir -p build
cd build/
../qemu/configure --disable-kvm --target-list="arm-softmmu"
make -j$(nproc)

