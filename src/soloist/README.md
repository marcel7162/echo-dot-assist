# Spotify Soloist on the Echo

librespot (`src/spotify/`) logs in, but Spotify refuses some accounts the key of every track ("error audio key 0 1",
librespot issue #1649: newer accounts, Duo among them; 2026-11-19 on the tester's Echo Dot 3). **Soloist** is
Spotify's own headless Spotify Connect client (github.com/spotify/soloist): it gets the keys, and on the tester's PC it
played with that same account.  Its arm32 build (`https://soloist-builds.spotifycdn.com/soloist_release_arm32.tar.gz`)
is a glibc program (GLIBC_2.38, libatomic; C++ inside) that dlopens PulseAudio (or PipeWire) for its sound.  Spotify
does not allow passing it on: each Echo's owner downloads it from Spotify, with a "Soloist API key" from the Spotify for
Developers dashboard; a build stops working 90 days after it was made.

What the Echo lacks, and what stands in:

- **glibc**: Ubuntu's armhf build (LGPL), run by its own loader (`ld-linux-armhf.so.3 --library-path`), beside bionic.
- **PulseAudio**: `hmpulse.c`, a `libpulse.so.0` of the 50 functions Soloist calls (its threaded mainloop, one context,
  one float32 stream, timing, the sink input's volume).  The PCM goes out as s16le stereo, paced by the clock, to a
  descriptor (hassmic) or TCP (`mixplay -l`, by hand).  `make unit-hmpulse` uses it as Soloist does.
- **A port in the firewall's range**: Soloist's zeroconf server takes any free port; `hmpulse.c`'s `bind()` (the
  library is preloaded) gives the first TCP listener on port 0 `HMPULSE_ZC_PORT`.  After the first login Soloist turns
  discovery off ("Discovery disabled") and is reached through Spotify's servers.
- **/etc/resolv.conf and /etc/ssl/certs/ca-certificates.crt**: /etc is /system/etc on the Echo; written there.
- **CP15 barriers**: the arm32 build runs on ARMv6 and sets memory barriers with `mcr p15`; the Echo Dot 3's arm64
  kernel emulates them for 32-bit programs (`/proc/sys/abi/cp15_barrier` 1), a trap each, and Soloist spun for minutes
  in system time at its first request.  `2` has the CPU execute them: logged in within 3 s (2026-11-19).
- **The first login**: from the app it timed out (30 s) while the barriers were emulated; paired on a PC instead
  (`soloist -p -D dir`), the data folder copied over, the session restored on the Echo.  To try again with `2`.

By hand first (`tools/soloist-kit.sh` makes the kit, `soloist-test.sh` runs it on the Echo); then hassmic runs it as
it runs hassmic-spotify (spotify.c).
