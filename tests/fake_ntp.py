#!/usr/bin/env python3
"""The clock from NTP (ntp.c) against build/hassmic-host and a stand-in time server: while nobody set the clock, the
server's time goes to root in state/clock marked "ntp" (main.sh clock_set), with the boot clock as Home Assistant's
does; an answer that does not echo the query's transmit time is not taken; switched off, the server hears nothing."""
import asyncio, os, re, socket, struct, threading, time
from esphome_common import check, until, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17191, 17192, 17193, 17194, 17195
NTP_PORT = 17196
NTP_UNIX = 2208988800
OFFSET = 3600                       # the server says an hour later than the PC, to tell its time from the PC's
queries, spoof = [], {"on": True}


def server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", NTP_PORT))
    while True:
        q, a = s.recvfrom(512)
        queries.append(time.monotonic())
        if len(q) < 48 or q[0] & 7 != 3: continue
        now = time.time() + OFFSET
        sec, frac = int(now) + NTP_UNIX, int((now % 1) * 2**32)
        if spoof["on"]:             # first one answer from someone who never saw the query: a wrong origin
            s.sendto(bytes([0x24, 2, 0, 0]) + bytes(20) + bytes(8) + struct.pack(">IIII", sec, frac, sec + 999, frac), a)
        s.sendto(bytes([0x24, 2, 0, 0]) + bytes(20) + q[40:48] + struct.pack(">IIII", sec, frac, sec, frac), a)


async def main(e):
    clock = os.path.join(e.state, "clock")
    # the first answer is a spoof only, so nothing may come of it; then the real one
    await until(lambda: len(queries) >= 1, 5)
    await asyncio.sleep(0.5)
    first = open(clock).read().split() if os.path.exists(clock) else []
    check(not first or abs(int(first[0]) - (time.time() + OFFSET)) <= 2, f"an answer without the query's transmit time is not taken ({first})")
    spoof["on"] = False
    check(await until(lambda: os.path.exists(clock), 6), "the clock unset: the time server's answer handed to root")
    t, at, src = (open(clock).read().split() + ["", "", ""])[:3]
    boot = time.clock_gettime(time.CLOCK_BOOTTIME)
    check(abs(int(t) - (time.time() + OFFSET)) <= 2 and re.fullmatch(r"\d+\.\d\d", at) and 0 <= boot - float(at) < 5 and src == "ntp",
          f"  the server's time, the boot clock, marked ntp: {t} {at} {src}")
    check("clock: NTP's time handed to root" in e.log(), "  and the log says where it came from")

    # off: no more queries
    os.remove(clock)
    e.page.set(ntp_time="off")
    await asyncio.sleep(2.5); n = len(queries); await asyncio.sleep(5)
    check(len(queries) == n and not os.path.exists(clock), f"switched off: the time server hears nothing more ({len(queries) - n} queries)")


threading.Thread(target=server, daemon=True).start()
run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_NTP": f"127.0.0.1:{NTP_PORT}"}, log=True)
