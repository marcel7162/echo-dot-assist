#!/usr/bin/env python3
"""Spotify on every Echo (ssgroup.c) with two Echos (build/hassmic-host) of one arbitration network on this PC: the
leader runs a stand-in Spotify (tests/fake_spotify.py) with "Spotify on every Echo" on and serves it as a Sendspin
server; both Echos' Sendspin players take it under the network's derived key (no pairing) and play it, the leader
none of it on its own Spotify stream; the app's volume reaches both; a player's action button pauses Spotify; the
music stopped ends the stream on both.  An Echo of another network is refused."""
import asyncio, base64, json, os, subprocess, sys, tempfile, time
from arbecho import Echo, ROOT, check, until
from webclient import wait_port, Browser

NET = (0x5eed, os.urandom(32))
LS, FS, XS = 17271, 17272, 17273          # the players' Sendspin ports: leader, follower, an Echo of another network


class GEcho(Echo):
    def __init__(self, name, port, ss, net, extra=None, config=""):
        super().__init__(name, port, 500, net=net)
        self.ss = ss
        self.music = os.path.join(self.state, "music.raw")
        self.env.update(HASSMIC_MUSIC=self.music, HASSMIC_CLOCK_SYNCED="0", HASSMIC_NTP="127.0.0.1:9", **(extra or {}))
        if config: open(os.path.join(self.state, "config"), "w").write(config)

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", str(self.ss), "-o", "0", "-a", "28990", "-W", str(self.web)], env=self.env, stderr=open(self.log, "a"))

    def size(self): return os.path.getsize(self.music) if os.path.exists(self.music) else 0


async def main():
    tmp = tempfile.mkdtemp()
    slog, sctl, spot = (os.path.join(tmp, n) for n in ("spotify.log", "spotify.ctl", "spot.raw"))
    open(sctl, "w").close()
    lead = GEcho("Echo Lead", 17241, LS, NET, config="spotify=on\nspotify_group=on\nspotify_engine=librespot\n", extra=dict(
        HASSMIC_SPOTIFY=os.path.join(ROOT, "tests", "fake_spotify.py"), FAKE_SPOTIFY_LOG=slog, FAKE_SPOTIFY_CTL=sctl,
        FAKE_SPOTIFY_USER="tester", HASSMIC_SPOT=spot, HASSMIC_SPOTIFY_MDNS=os.path.join(tmp, "mdns"),
        HASSMIC_SSGROUP_PEERS=f"127.0.0.1:{LS},127.0.0.1:{FS},127.0.0.1:{XS}"))
    fol = GEcho("Echo Follow", 17243, FS, NET)
    other = GEcho("Echo Other", 17245, XS, (0xbad, os.urandom(32)))
    for e in (lead, fol, other): e.start()
    try:
        wait_port(lead.web, fol.web, other.web, LS, FS, XS)
        page = Browser(lead.web)
        check(page.login_with_button(lead.proc), "the leader's settings page: logged in")
        names = {x["name"]: x for x in page.state()["settings"]}
        check(names.get("spotify_group", {}).get("value") == 1, "\"Spotify on every Echo\" is on (state/config)")
        await until(lambda: json.loads(page.call("GET", "/api/spotify")[2]).get("logged_in"), 8)

        st, _, d = page.call("POST", "/api/spotify", b"say Spiel Spotify")
        check(st == 200, "\"Spiel Spotify\" on the leader")
        check(await until(lambda: "player role active (paired)" in fol.text(), 15), "the follower's player takes the leader's server under the network's key")
        check("player role active (paired)" in lead.text(), "  and so does the leader's own")
        l0, f0 = lead.size(), fol.size()
        check(await until(lambda: fol.size() > f0 + 48000 * 4 and lead.size() > l0 + 48000 * 4, 10), "both play Spotify's music through Sendspin")
        check(not os.path.exists(spot) or os.path.getsize(spot) == 0, "  the leader's own Spotify stream stays silent")
        check("stream start (pcm)" in fol.text(), "  as PCM at 48 kHz")
        # in step: the PC's music sinks take PCM at real time once it is due, so what both wrote since the start
        # differs by how far apart they play (48 kHz stereo 16 bit: 192 bytes a millisecond)
        await asyncio.sleep(3)
        apart = []
        for _ in range(20):
            a, b = lead.size() - l0, fol.size() - f0
            apart.append(abs(a - b) / 192); await asyncio.sleep(0.05)
        apart.sort()
        check(apart[len(apart) // 2] < 40, f"  in step: {apart[len(apart) // 2]:.0f} ms apart (the median of 20 looks)")
        check(await until(lambda: "handshake failed" in lead.text(), 15) and "player role active" not in other.text(),
              "an Echo of another network: the handshake fails, it plays nothing")

        # the app's volume slider: both Echos follow
        with open(sctl, "a") as f: f.write("volume 37\n")
        fpage = Browser(fol.web); check(fpage.login_with_button(fol.proc), "the follower's settings page: logged in")
        fvol = lambda: next(x["value"] for x in fpage.state()["settings"] if x["name"] == "volume")
        check(await until(lambda: fvol() == 37, 6), "the app's volume at 37: the follower too")

        # the follower's action button: Spotify pauses
        n = open(slog).read().count("\npause")
        fol.proc.send_signal(__import__("signal").SIGUSR2)
        check(await until(lambda: open(slog).read().count("\npause") > n, 6), "the follower's action button pauses Spotify")
        check(await until(lambda: "stream end" in fol.text(), 6), "  and the stream ends on the follower")
    finally:
        for e in (lead, fol, other):
            e.proc.terminate()
            try: e.proc.wait(5)
            except subprocess.TimeoutExpired: e.proc.kill()
    if check.failed:
        for e in (lead, fol):
            print(f"--- {e.name}"); print("".join(l for l in e.text().splitlines(True) if "ssgroup" in l or "sendspin" in l or "spotify" in l)[-4000:])
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


asyncio.run(main())
