#!/bin/sh
# Spotify Soloist on an Echo by hand, before hassmic runs it (src/soloist/README.md): a kit of what Soloist's arm32
# build needs there and the Echo has not, into build/soloist-kit/ and build/soloist-kit.tar.gz:
#   ld-linux-armhf.so.3 and glibc's libraries (Ubuntu's armhf build, 2.39: Soloist asks for GLIBC_2.38; LGPL),
#   libpulse.so.0 = src/soloist/hmpulse.c (its sound to the Echo's mixplay over TCP, the zeroconf port fixed),
#   ca-certificates.crt (Soloist's curl looks in /etc/ssl/certs), soloist-test.sh (what to run on the Echo).
# Soloist itself is not in it: Spotify does not allow passing it on.  Each Echo's owner downloads it from Spotify.
# Needs (Ubuntu 24.04 or newer, for a glibc of 2.38 or later):
#   sudo apt install gcc-arm-linux-gnueabihf libc6-armhf-cross libatomic1-armhf-cross libgcc-s1-armhf-cross libpulse-dev ca-certificates
set -e
cd "$(dirname "$0")/.."
L=/usr/arm-linux-gnueabihf/lib
K=build/soloist-kit
for f in $L/ld-linux-armhf.so.3 $L/libc.so.6 /usr/include/pulse/pulseaudio.h; do
    [ -e $f ] || { echo "$f missing: sudo apt install gcc-arm-linux-gnueabihf libc6-armhf-cross libatomic1-armhf-cross libgcc-s1-armhf-cross libpulse-dev" >&2; exit 1; }
done
v=$(strings $L/libc.so.6 | sed -n 's/^GNU C Library .* version \([0-9][0-9.]*[0-9]\).*/\1/p' | head -1)
case "$v" in 2.3[89]*|2.[4-9]*) ;; *) echo "glibc $v of $L is older than Soloist's 2.38: Ubuntu 24.04 or newer" >&2; exit 1;; esac
rm -rf $K; mkdir -p $K build/soloist/inc
cp -r /usr/include/pulse build/soloist/inc/
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -Ibuild/soloist/inc -Wl,-soname,libpulse.so.0 \
    -Wl,--build-id=none src/soloist/hmpulse.c -o $K/libpulse.so.0 -lpthread -ldl -lm
for f in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libresolv.so.2 libatomic.so.1 libgcc_s.so.1; do
    cp -L $L/$f $K/
done
cp -L /etc/ssl/certs/ca-certificates.crt $K/
cp src/soloist/soloist-test.sh $K/
chmod 755 $K/*.so* $K/soloist-test.sh; chmod 644 $K/ca-certificates.crt
tar -C build -czf build/soloist-kit.tar.gz soloist-kit
echo "build/soloist-kit.tar.gz: glibc $v, $(ls $K | wc -l) files"
