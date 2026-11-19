#!/usr/bin/env python3
"""Stand-in for hassmic-spotify (src/spotify/src/main.rs) in tests/fake_ha_spotify.py: its protocol without Spotify.
stdin: commands, a line each; stdout: events; fd 3: PCM (s16le stereo 44.1 kHz, a 440 Hz tone) at real time while
"playing".  What it was started with and every command go to $FAKE_SPOTIFY_LOG; lines a test appends to
$FAKE_SPOTIFY_CTL are sent as events, as if the Spotify app did something (the volume slider, a login).  Logged in
while $FAKE_SPOTIFY_USER is set (else "login -"); "Nichtda" is a search that finds nothing."""
import json, math, os, struct, sys, threading, time

log = open(os.environ["FAKE_SPOTIFY_LOG"], "a", buffering=1)
log.write("args " + json.dumps(sys.argv[1:]) + "\n")
audio = os.fdopen(3, "wb", buffering=0)
lock = threading.Lock()
playing = threading.Event()
user = os.environ.get("FAKE_SPOTIFY_USER", "")


def emit(line):
    with lock:
        sys.stdout.write(line + "\n"); sys.stdout.flush()


def pump():
    """20 ms of tone at a time, paced by the clock (the real one is paced by the pipe)"""
    n, t, k = 882, time.monotonic(), 0
    while True:
        if not playing.wait(0.1): t = time.monotonic(); continue
        frames = b"".join(struct.pack("<hh", v, v) for v in (int(8000 * math.sin(2 * math.pi * 440 * (k + i) / 44100)) for i in range(n)))
        k += n
        try: audio.write(frames)
        except OSError: os._exit(0)
        t += 0.02
        time.sleep(max(0, t - time.monotonic()))


def ctl():
    path, seen = os.environ.get("FAKE_SPOTIFY_CTL"), 0
    while path:
        try:
            with open(path) as f: lines = f.read().splitlines()
        except OSError: lines = []
        for l in lines[seen:]:
            # "!<name>:<command>": a command to the speaker of that name only, as if from hassmic (the group's tests)
            if l.startswith("!"):
                who, _, c = l[1:].partition(":")
                if who == name: command(c)
            else: emit(l)
        seen = len(lines)
        time.sleep(0.05)


def play(cmd, what):
    if not user: emit(f"result {cmd} fail nologin"); return
    playing.set(); emit("state playing"); emit("track Bohemian Rhapsody\tQueen\tA Night at the Opera\thttps://i.scdn.co/x\t354000")
    emit(f"result {cmd} ok {what}")


name = sys.argv[sys.argv.index("--name") + 1] if "--name" in sys.argv else ""


def command(line):
    global user
    log.write(line + "\n")
    cmd, _, rest = line.partition(" ")
    if cmd == "play": play("play", "resumed")
    elif cmd == "liked": play("liked", "liked")
    elif cmd == "search":
        kind, _, words = rest.partition(" ")
        if "Nichtda" in words: emit("result search fail notfound")
        else: play("search", f"{'artist' if kind == 'any' else kind} {words}")
    elif cmd == "pause": playing.clear(); emit("state paused")
    elif cmd == "resume": playing.set(); emit("state playing")
    elif cmd == "logout": playing.clear(); user = ""; emit("state stopped"); emit("login -")


threading.Thread(target=pump, daemon=True).start()
threading.Thread(target=ctl, daemon=True).start()
emit("ready")
emit(f"login {user}" if user else "login -")
for line in sys.stdin:
    line = line.rstrip("\n")
    if line == "quit": log.write(line + "\n"); break
    command(line)
playing.clear()
