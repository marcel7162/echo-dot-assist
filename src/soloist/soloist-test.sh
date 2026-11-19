#!/system/bin/sh
# Spotify Soloist on the Echo by hand (tools/soloist-kit.sh makes the kit): as root from adb shell,
#   sh /data/local/tmp/soloist/soloist-test.sh <Soloist API key>
# with Spotify's arm32 build unpacked as /data/local/tmp/soloist/soloist.  Then pick "Echo Soloist" in the Spotify app.
# What it does:
#   - /etc is /system/etc here: glibc's resolver reads /etc/resolv.conf (Android has none, bionic asks netd) and
#     Soloist's curl /etc/ssl/certs/ca-certificates.crt.  Both written once onto the system partition (remounted
#     writable for that, then read-only again).  To take them away: soloist-test.sh --undo
#   - mixplay plays what comes to TCP 28940 on the mixer's Music stream, as the daemon's user (AIPC refuses root)
#   - Soloist runs as the daemon's user with the network group 3990 first (lockdown.sh lets that out to Spotify), under
#     glibc's own loader with this kit's libraries, libpulse.so.0 (hmpulse.c) preloaded: its sound to 28940, its
#     zeroconf port 28936 (the firewall admits 16384-32767)
D=$(cd "$(dirname "$0")" && pwd)
H=/system/hassmic; [ -x /data/local/hassmic/ota/current/mixplay ] && H=/data/local/hassmic/ota/current
U=$(grep '^DAEMON_USER=' $H/device.conf 2>/dev/null | cut -d= -f2); U=${U:-puffin}
[ "$(id -u)" = 0 ] || { echo "run it as root (adb shell)"; exit 1; }

if [ "$1" = --undo ]; then
    $H/otatool remount rw /system/etc && rm -f /system/etc/resolv.conf /system/etc/ssl/certs/ca-certificates.crt &&
        rmdir /system/etc/ssl/certs /system/etc/ssl 2>/dev/null; $H/otatool remount ro /system/etc
    echo "resolv.conf and the certificates are off the system partition again"; exit 0
fi
KEY=$1
[ -n "$KEY" ] || { echo "usage: sh $0 <Soloist API key> | --undo"; exit 2; }
[ -f $D/soloist ] || { echo "$D/soloist missing: Spotify's arm32 build (soloist_release_arm32.tar.gz) unpacked here"; exit 1; }
[ "$(date +%Y)" -ge 2026 ] || { echo "the Echo's clock says $(date): set it first (adb shell \"date -u @\$(date +%s)\" from the PC)"; exit 1; }

dns=$(getprop net.dns1); [ -n "$dns" ] || dns=$(getprop dhcp.wlan0.dns1)
[ -n "$dns" ] || { echo "no DNS server known (getprop net.dns1, dhcp.wlan0.dns1)"; exit 1; }
if [ "$(cat /system/etc/resolv.conf 2>/dev/null)" != "nameserver $dns" ] || ! cmp -s $D/ca-certificates.crt /system/etc/ssl/certs/ca-certificates.crt; then
    $H/otatool remount rw /system/etc || { echo "cannot remount the system partition"; exit 1; }
    mkdir -p /system/etc/ssl/certs
    cp $D/ca-certificates.crt /system/etc/ssl/certs/ca-certificates.crt
    echo "nameserver $dns" > /system/etc/resolv.conf
    chmod 755 /system/etc/ssl /system/etc/ssl/certs; chmod 644 /system/etc/resolv.conf /system/etc/ssl/certs/ca-certificates.crt
    chcon u:object_r:system_file:s0 /system/etc/resolv.conf /system/etc/ssl /system/etc/ssl/certs /system/etc/ssl/certs/ca-certificates.crt 2>/dev/null
    sync; $H/otatool remount ro /system/etc
    echo "DNS ($dns) and certificates for glibc in /system/etc"
fi

# Soloist's arm32 build runs on ARMv6 too and sets its memory barriers with CP15 (mcr p15, c7, c10, 5); the Echo's
# arm64 kernel emulates those for 32-bit programs, a trap into the kernel each: it spun for minutes at its first
# request (all system time, 2026-11-19).  2: the CPU executes them itself (ARMv8 has them in AArch32), until a reboot
[ -w /proc/sys/abi/cp15_barrier ] && echo 2 > /proc/sys/abi/cp15_barrier
pkill -f 'mixplay -t Music -r 44100 -c 2 -l 28940' 2>/dev/null
chmod 755 $D $D/soloist $D/*.so* 2>/dev/null
mkdir -p $D/data $D/cache; chown -R $U $D/data $D/cache

# inet: Android lets only that group open sockets, a listening one too ("listen: Permission denied" without it)
$H/runas $U aipc,audio,inet $H/mixplay -t Music -r 44100 -c 2 -l 28940 &
MP=$!
trap 'kill $MP 2>/dev/null' EXIT INT TERM
sleep 1
echo "Soloist as $U, group 3990; pick \"Echo Soloist\" in the Spotify app (Ctrl-C ends it)"
HOME=$D/data HMPULSE_TCP=127.0.0.1:28940 HMPULSE_ZC_PORT=28936 HMPULSE_DEBUG=1 \
    $H/runas $U 3990,inet $D/ld-linux-armhf.so.3 --library-path $D --preload $D/libpulse.so.0 \
    $D/soloist -n "Echo Soloist" -k "$KEY" -D $D/data -C $D/cache -z 200 -v
