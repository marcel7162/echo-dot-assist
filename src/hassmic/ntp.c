/* The clock from the internet (SNTP, RFC 4330) where Home Assistant has not set it.  An Echo's clock starts at 1970 or
 * at whatever its RTC kept (the Dots' has no battery worth the name), and until now only Home Assistant set it
 * (clock.c clock_from_ha): an Echo without Home Assistant, or before its first link, had no TLS (rustls and Soloist
 * refuse certificates "not yet valid"), so Spotify never came up (the tester, twice, 2026-11-19).
 *
 * One thread: while root has not set the clock (hassmic.clock.synced), a query every 30 s, servers in turn; once set,
 * one every 12 h (an Echo away from Home Assistant drifts ~1 s a day).  The answer goes the way Home Assistant's does
 * (clock_hand: state/clock, root's main.sh clock_set), marked "ntp".  Home Assistant's time, when it comes, wins on its
 * own: it is handed on every link, and clock_hand skips what is within 2 s.  The socket is hassmic's (net_socket:
 * group 3990, the egress lock lets it out); its answer comes back as an established flow (lockdown.sh INPUT).
 * Setting `ntp_time` (on): off keeps the Echo off NTP servers, Home Assistant then the only source.
 * HASSMIC_NTP=host[:port] for tests (one server, every 2 s while unset). */
#include "ntp.h"
#include "clock.h"
#include "net.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NTP_UNIX 2208988800u            /* seconds from 1900 (NTP's era 0) to 1970 */
static const char *const servers[] = { "pool.ntp.org", "time.cloudflare.com", "time.google.com" };
static atomic_int on = 1;

int ntp_enable(int set) { if (set >= 0) atomic_store(&on, set != 0); return atomic_load(&on); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* One query: 0 and the server's transmit time in Unix seconds (rounded, plus half the round trip), -1 none */
int ntp_query(const char *host, const char *port, uint32_t *unix_out)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hints, &ai) || !ai) return -1;
    int s = net_socket(AF_INET, SOCK_DGRAM, 0), r = -1;
    /* connected: only the server's answers reach us */
    if (s >= 0 && !connect(s, ai->ai_addr, ai->ai_addrlen)) {
        uint8_t q[48] = { 0x23 }, a[48];                /* LI 0, version 4, mode 3 (client) */
        /* our transmit time: random, as RFC 5905 suggests for clients, so an answer must echo it as its origin */
        uint32_t nonce[2]; struct timespec n0, n1;
        clock_gettime(CLOCK_MONOTONIC, &n0);
        nonce[0] = (uint32_t)rand() ^ (uint32_t)n0.tv_nsec; nonce[1] = (uint32_t)rand() ^ (uint32_t)getpid();
        memcpy(q + 40, nonce, 8);
        if (send(s, q, sizeof q, 0) == sizeof q) {
            struct pollfd p = { s, POLLIN, 0 };
            while (poll(&p, 1, 3000) == 1) {
                ssize_t n = recv(s, a, sizeof a, 0);
                if (n < 48) { if (n < 0) break; continue; }
                clock_gettime(CLOCK_MONOTONIC, &n1);
                int mode = a[0] & 7, stratum = a[1], li = a[0] >> 6;
                if (memcmp(a + 24, nonce, 8) || mode != 4 || stratum == 0 || stratum > 15 || li == 3) continue;
                uint32_t sec = be32(a + 40), frac = be32(a + 44);
                if (!sec) continue;
                long long rtt_ms = (n1.tv_sec - n0.tv_sec) * 1000LL + (n1.tv_nsec - n0.tv_nsec) / 1000000;
                /* era 0 ends 2036-02-07: later timestamps have wrapped (RFC 4330 section 3) */
                uint64_t u = sec >= 0x80000000u ? (uint64_t)sec - NTP_UNIX : (uint64_t)sec + 0x100000000ull - NTP_UNIX;
                u += ((uint64_t)frac * 1000 / 0x100000000ull + (uint64_t)rtt_ms / 2 + 500) / 1000;
                *unix_out = (uint32_t)u;
                r = 0;
                break;
            }
        }
    }
    if (s >= 0) close(s);
    freeaddrinfo(ai);
    return r;
}

static void *ntp_thread(void *arg)
{
    (void)arg;
    const char *env = getenv("HASSMIC_NTP");
    char host[128] = "", port[8] = "123";
    if (env) {
        snprintf(host, sizeof host, "%s", env);
        char *c = strrchr(host, ':');
        if (c) { *c = 0; snprintf(port, sizeof port, "%s", c + 1); }
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    int next = 0, told = 0;
    for (;;) {
        int synced = clock_synced();
        if (atomic_load(&on)) {
            uint32_t t;
            const char *h = env ? host : servers[next];
            if (!ntp_query(h, port, &t)) {
                clock_hand(t, "ntp");
                told = 0;
            } else {
                if (!told) fprintf(stderr, "clock: no answer from NTP (%s)%s\n", h, synced ? "" : ", trying again");
                told = 1;
                next = (next + 1) % (int)(sizeof servers / sizeof *servers);
            }
        }
        /* unset: soon again (root takes state/clock within its 10 s loop); set: twice a day */
        int wait = clock_synced() ? 12 * 3600 : env ? 2 : 30;
        for (int i = 0; i < wait; i++) {
            sleep(1);
            if (!synced && clock_synced()) break;
        }
    }
    return NULL;
}

void ntp_start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, ntp_thread, NULL)) pthread_detach(t);
}
