#!/usr/bin/env python3
"""Spotify Connect (spotify.c, spotcmd.c) against build/hassmic-host with a stand-in speaker (tests/fake_spotify.py) as
Home Assistant and the settings page: off by default and nothing running; switched on, the speaker started with the
Echo's name, a device id and the settings, its mDNS record in avahi's folder; spoken commands taken before Home
Assistant answers them ("Spiel Queen auf Spotify": a search, the run cancelled) and the rest left to it; its PCM on a
stream of its own; the app's volume slider moving the Echo's volume and the Echo's going back; the action button
pausing and playing again; a refusal heard as the error sound; renaming, logging out and switching off.  Then the real
speaker (build/hassmic-spotify-host) for what needs no account: zeroconf's getInfo, a command without a login, its
end with its stdin."""
import asyncio, json, os, subprocess, sys, tempfile, time, urllib.request
from aioesphomeapi import VoiceAssistantEventType as Ev
from esphome_common import ROOT, KEY_HELP, check, until, home_assistant, run, sync

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17181, 17182, 17183, 17184, 17185
tmp = tempfile.mkdtemp()
LOG, CTL, OUT, MDNS = (os.path.join(tmp, n) for n in ("spotify.log", "spotify.ctl", "spot.raw", "spotify.service"))
open(CTL, "w").close()
ENV = {"HASSMIC_SPOTIFY": os.path.join(ROOT, "tests", "fake_spotify.py"), "FAKE_SPOTIFY_LOG": LOG, "FAKE_SPOTIFY_CTL": CTL,
       "FAKE_SPOTIFY_USER": "tester", "HASSMIC_SPOT": OUT, "HASSMIC_SPOTIFY_MDNS": MDNS}


def lines(): return open(LOG).read().splitlines() if os.path.exists(LOG) else []
def args(): return [json.loads(l[5:]) for l in lines() if l.startswith("args ")]
def size(): return os.path.getsize(OUT) if os.path.exists(OUT) else 0
def app(line):                                           # the Spotify app does something: an event from the speaker
    with open(CTL, "a") as f: f.write(line + "\n")


async def main(e):
    page = e.page
    sp = lambda: json.loads(page.call("GET", "/api/spotify")[2])
    def say(t):
        st, _, d = page.call("POST", "/api/spotify", f"say {t}".encode())
        return json.loads(d) if st == 200 else {"status": st}
    names = {x["name"]: x for x in page.state()["settings"]}
    check(all(n in names for n in ("spotify", "spotify_voice", "spotify_bitrate", "spotify_normalise")) and names["spotify"]["value"] == 0,
          "the settings are there, Spotify off by default")
    await asyncio.sleep(1)
    check(not args() and not os.path.exists(MDNS) and not sp()["running"], "off: no speaker running, no mDNS record")

    # on: started with the Echo's name, the zeroconf port, a device id; announced
    page.set(spotify="on")
    check(await until(lambda: args(), 5), "on: the speaker starts")
    a = args()[0]
    opt = dict(zip(a[0::2], a[1::2]))
    check(opt.get("--name") == "Echo Dot" and opt.get("--port") == "28933" and len(opt.get("--device-id", "")) == 40 and opt.get("--bitrate") == "160"
          and opt.get("--normalise") == "1" and opt.get("--audio-fd") == "3", f"  with the Echo's name, port, device id and settings ({args()[0]})")
    check(await until(lambda: os.path.exists(MDNS), 3) and "_spotify-connect._tcp" in open(MDNS).read() and "<port>28933</port>" in open(MDNS).read()
          and "<name>Echo Dot</name>" in open(MDNS).read(), "  its mDNS record in avahi's folder")
    check(await until(lambda: sp()["logged_in"] and sp()["user"] == "tester", 3), "  logged in as the app's account (the page says so)")
    app("error clock")                      # what the speaker says while the Echo's clock is 1970 (no Home Assistant yet)
    check(await until(lambda: sp()["error"] == "clock", 3), "why no session comes up reaches the page (the clock not set)")
    app("login tester")
    check(await until(lambda: sp()["error"] == "", 3), "  and goes once a session is up")

    # spoken: Home Assistant's run is cancelled, the speaker gets the search
    ha = await home_assistant(e)
    async def spoken(text):
        ha.va.started.clear(); n = len(ha.va.stopped); e.wake()
        await asyncio.wait_for(ha.va.started.wait(), 5)
        ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
        ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
        ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
        ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_END, {"text": text})
        await until(lambda: len(ha.va.stopped) > n, 3)
        return ha.va.stopped[n:]
    stops = await spoken("Spiel Queen auf Spotify.")
    check(await until(lambda: "search any Queen" in lines(), 3), "\"Spiel Queen auf Spotify\": the speaker searches for Queen")
    check(stops and stops[-1] is True, f"  and Home Assistant's run is cancelled, not answered ({stops})")
    s0 = size(); check(await until(lambda: size() > s0 + 44100 * 4 // 2, 3), "  its music plays on the Spotify stream")
    check(await until(lambda: sp()["playing"] and sp()["title"] == "Bohemian Rhapsody" and sp()["result"].startswith("ok artist Queen"), 3),
          "  the page shows what plays and the last command")
    n = len(lines()); stops = await spoken("Wie spät ist es?")
    await asyncio.sleep(0.5)
    check(len(lines()) == n and not any(stops), f"\"Wie spät ist es\": Home Assistant's, the speaker hears nothing ({stops})")
    ha.cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await asyncio.sleep(0.5)

    # the page's "Try a command"
    check(say("Nächstes Lied")["playing"] == 1 and await until(lambda: lines()[-1] == "next", 2), "\"Nächstes Lied\" while it plays: next")
    check(say("Schalte das Licht ein") == {"said": False}, "\"Schalte das Licht ein\": not Spotify's (Home Assistant would answer it)")

    # volumes both ways
    app("volume 23")
    check(await until(lambda: page.state() and next(x for x in page.state()["settings"] if x["name"] == "volume")["value"] == 23, 3),
          "the app's slider at 23: the Echo's volume follows")
    page.set(volume=41)
    check(await until(lambda: "volume 41" in lines(), 3), "the Echo's volume at 41: the speaker is told (the app's slider follows)")
    check(lines().count("volume 23") == 0, "  no echo of the app's own 23 back to it")

    # the action button: pause, then play again
    e.keyev(KEY_HELP, 1); e.keyev(KEY_HELP, 0)
    check(await until(lambda: lines()[-1] == "pause", 3), "action button: Spotify pauses")
    await asyncio.sleep(1.0); s1 = size(); await asyncio.sleep(1.0)
    check(size() == s1, "  and its stream is quiet")
    e.keyev(KEY_HELP, 1); e.keyev(KEY_HELP, 0)
    check(await until(lambda: lines()[-1] == "resume", 3), "action button again: it plays on")

    # a refusal: the error sound
    say("Spiel Nichtda auf Spotify")
    check(await until(lambda: "sound: refused" in e.log(), 3) and await until(lambda: sp()["result"] == "fail notfound", 2),
          "nothing found: the error sound, and the page says why")

    # renamed: started again under the new name, announced under it
    st, _, _ = page.call("POST", "/api/spotify", b"name Wohnzimmer")
    named = lambda: len(args()) == 2 and dict(zip(args()[1][0::2], args()[1][1::2])).get("--name") == "Wohnzimmer"
    check(st == 200 and await until(named, 8), "renamed: the speaker starts again as \"Wohnzimmer\"")
    check(await until(lambda: "<name>Wohnzimmer</name>" in open(MDNS).read() if os.path.exists(MDNS) else False, 3), "  and is announced under it")
    check(sp()["name"] == "Wohnzimmer" and opt["--device-id"] == dict(zip(args()[1][0::2], args()[1][1::2]))["--device-id"], "  the same device id: the app keeps one entry for it")
    st, _, _ = page.call("POST", "/api/spotify", b"name <b>")
    check(st == 400, "a name with markup is refused")

    # logout
    await until(lambda: sp()["running"] and sp()["logged_in"], 5)
    page.call("POST", "/api/spotify", b"logout")
    check(await until(lambda: "logout" in lines() and not sp()["logged_in"], 3), "logout: the speaker forgets the login")

    # off: ended, unannounced
    page.set(spotify="off")
    check(await until(lambda: lines()[-1] == "quit" if lines() else False, 3) and await until(lambda: not os.path.exists(MDNS), 3)
          and await until(lambda: not sp()["running"], 3), "off: the speaker is told to quit, the record goes")
    check(say("Spiel Spotify") == {"said": False}, "off: \"Spiel Spotify\" is Home Assistant's again")
    await ha.cli.disconnect()

    # the real speaker (src/spotify/), without an account
    real = os.path.join(ROOT, "build", "hassmic-spotify-host")
    if not os.path.exists(real):
        check(False, f"{real} is missing: make build/hassmic-spotify-host")
        return
    d = tempfile.mkdtemp(); r, w = os.pipe()
    p = subprocess.Popen([real, "--name", "Küche", "--dir", d, "--port", "28934", "--device-id", "ab" * 20, "--audio-fd", str(w)],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, pass_fds=(w,), text=True)
    first = [p.stdout.readline().strip(), p.stdout.readline().strip()]
    check(first == ["ready", "login -"], f"the real speaker: ready, no login stored ({first})")
    info = json.loads(urllib.request.urlopen("http://127.0.0.1:28934/?action=getInfo", timeout=3).read())
    check(info["remoteName"] == "Küche" and info["deviceID"] == "ab" * 20 and info["deviceType"] == "SPEAKER" and len(info["publicKey"]) > 100
          and info["activeUser"] == "", "  zeroconf's getInfo: its name, id and a Diffie-Hellman key for the app")
    bad = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:28934/", data=b"action=addUser&userName=x&blob=AAAA&clientKey=AAAA"), timeout=3).read())
    check(bad["status"] == 102 and bad["spotifyError"] == 1, f"  a login that is no login is refused ({bad})")
    p.stdin.write("play\n"); p.stdin.flush()
    check(p.stdout.readline().strip() == "result play fail nologin", "  \"play\" without a login: refused, for the error sound")
    p.stdin.close()
    try: p.wait(5); ended = True
    except subprocess.TimeoutExpired: p.kill(); ended = False
    check(ended, "  its stdin closed (hassmic ended): it ends too")
    os.close(r); os.close(w)


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), ENV, log=True)
