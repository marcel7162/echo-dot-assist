#!/bin/sh
# Build and push everything to /data/local/hassmic on the device (adb, root shell).  The model is the one on adb.
# The binaries are built here or this commit's release build (PREBUILT, scripts/lib/build.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb; device_check_firmware warn
. scripts/lib/build.sh; build_binaries
D=/data/local/hassmic
adb shell "mkdir -p $D"
adb push $(ls $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $OUT/hassmic $OUT/runas $DDIR/device.conf scripts/device/*.sh testdata/alexa_espeak.raw $D/ >/dev/null
# the wake word aligners (src/hassmic/wakealign.h)
adb push wakealign/*.wa wakealign/*.was $D/ >/dev/null
[ -n "$ANDROID" ] && adb push scripts/device/wifictl.dex ${SCREEN_BAND:+scripts/device/ringbar.dex} $D/ >/dev/null
[ -f $OUT/libhmui.so ] && adb push $OUT/libhmui.so scripts/device/screen.dex $D/ >/dev/null
[ -f $OUT/hassmic-spotify ] && adb push $OUT/hassmic-spotify $D/ >/dev/null      # Spotify Connect (spotify.c finds it next to hassmic)
[ -f $OUT/libpulse.so.0 ] && adb push $OUT/libpulse.so.0 $OUT/ld-linux-armhf.so.3 $OUT/libc.so.6 $OUT/libm.so.6 $OUT/libpthread.so.0 \
    $OUT/libdl.so.2 $OUT/librt.so.1 $OUT/libresolv.so.2 $OUT/libatomic.so.1 $OUT/libgcc_s.so.1 $OUT/ca-certificates.crt $D/ >/dev/null   # Soloist's runtime
adb shell "chmod 755 $D/*"
echo "pushed $DEVICE build to $D"
