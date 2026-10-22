#!/bin/bash
set -euo pipefail

export PATH="/opt/rblive4_build/flux/src:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export RX3="/opt/rblive4_build/rootfs"
export CC=arm-linux-gnueabi-gcc
export CFLAGS="-O2 -march=armv5t -mfloat-abi=soft -U_FILE_OFFSET_BITS -D_FILE_OFFSET_BITS=32 -U_TIME_BITS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-error=int-conversion -Wno-error=incompatible-pointer-types --sysroot=$RX3"
export LDFLAGS="--sysroot=$RX3 -L$RX3/lib -L$RX3/usr/lib -Wl,-rpath-link,$RX3/lib:$RX3/usr/lib"

cd /opt/rblive4_build/directfb
# Copy the updated compat_shim
cp -f /opt/rblive4_build/src/tools/build-directfb/compat_shim.c lib/direct/compat_shim.c
cp -f /opt/rblive4_build/src/tools/build-directfb/compat_shim.c systems/fbdev/compat_shim.c

./autogen.sh \
    --host=arm-linux-gnueabi \
    --prefix=/usr --libdir=/lib \
    --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
    --with-gfxdrivers=none \
    --without-tools \
    --disable-osx --disable-devmem

make -j$(nproc)
mkdir -p /opt/rblive4_build/src/work/dfb
make install DESTDIR="/opt/rblive4_build/src/work/dfb"

cd /opt/rblive4_build/src/work/dfb
for base in libdirect libfusion libdirectfb; do
  patchelf --set-soname "$base-1.4.so.0" "lib/$base-1.4.so.6.0.10"
  mv "lib/$base-1.4.so.6.0.10" "lib/$base-1.4.so.0.0.0"
  rm -f "lib/$base-1.4.so.6" "lib/$base-1.4.so"
  ln -sf "$base-1.4.so.0.0.0" "lib/$base-1.4.so.0"
done

for f in lib/lib*-1.4.so.0.0.0 lib/directfb-1.4-6/*/*.so; do
  for base in libdirect libfusion libdirectfb; do
    arm-linux-gnueabi-readelf -d "$f" | grep -q "\[$base-1.4.so.6\]" || continue
    patchelf --replace-needed "$base-1.4.so.6" "$base-1.4.so.0" "$f"
  done
done

echo "DIRECTFB BUILD AND PATCHELF FINISHED SUCCESSFULLY!"
