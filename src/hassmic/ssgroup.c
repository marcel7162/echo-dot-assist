/*
 * Spotify on every Echo: the Echo that runs Spotify (spotify.c) is a Sendspin server for the Echos of its arbitration
 * network, itself included, and each plays the music through its own Sendspin player (sendspin.c), in step: the
 * players already follow a server's clock to the millisecond (Kalman filter, a rate of their own; sendspin.c).
 * Music Assistant does the same, but streams Spotify through librespot, which Spotify refuses some accounts (the
 * tester's Duo: every track's key), and its shared client id was rate limited into minutes a search.
 *
 * The server speaks Sendspin 1.0 as aiosendspin 10.2.1 does, only what a hassmic player needs (docs/sendspin-digest.md):
 *   - it dials each member's player (its WebSocket on the Sendspin port; the address from the network's beacons, arb.c)
 *     and ours on 127.0.0.1, while Spotify plays and 30 s after;
 *   - cleartext init, then Noise KKpsk2 as the initiator with a long-term ("lt") key every member derives from the
 *     network key and our server id (ssgroup_psk): no pairing, and nobody outside the network has it;
 *   - server/hello, server/activate (playback; player and controller roles), group/update, server/time answers,
 *     stream/start (PCM 48 kHz stereo 16 bit), audio chunks of 25 ms stamped on our clock 600 ms ahead (the players
 *     report 300 ms lead time; the rest is Wi-Fi's), stream/end when Spotify stops, server/command volume when the
 *     Spotify app's slider moves (each Echo then at the app's volume), server/state with the controller's commands;
 *   - client/command from a player's controller (its action button, multi presses, "<wake word>, stop"): to Spotify.
 * Spotify's 44.1 kHz goes to 48 kHz here (cubic, as resample.c's interpolation): the players only take 48 kHz.
 * Chunks wait in a ring of 4 s; a player that connects late starts with what is still ahead of its time.  A source
 * that stalls (Wi-Fi to Spotify, a skip) shifts the timeline on: the players read a jump as a gap, as from aiosendspin.
 */
#include "ssgroup.h"
#include "arb.h"
#include "core.h"
#include "hash.h"
#include "noise.h"
#include "spotify.h"
#include "statefile.h"
#include "ws.h"
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "../third_party/monocypher.h"

#define IN_RATE 44100
#define OUT_RATE 48000
#define CHUNK 1200                      /* frames of 25 ms at 48 kHz, as aiosendspin */
#define RING 160                        /* 4 s of chunks */
#define LEAD_US 600000                  /* a chunk's time ahead of its making */
#define MIN_AHEAD_US 150000             /* closer than this: the source stalled, the timeline moves on */
#define LINGER_MS 30000                 /* connections kept after the music stopped */
#define MAX_CONN 8

static atomic_int enabled, ss_port = 28928, net_ok;     /* net_ok: in a network, so the players can take us */
static uint8_t key_priv[32], key_pub[32]; static char server_id[48];
static atomic_int volume = -1;

/* The ring: chunks with their time, a generation per stretch of music (a new stream/start), written by the PCM's thread */
struct chunk { long long ts; int gen; int16_t pcm[CHUNK * 2]; };
static struct chunk ring[RING];
static long long seq;                   /* chunks made so far; ring[(seq - 1) % RING] is the newest */
static int playing, gen;
static long long last_pcm_ms;
static pthread_mutex_t gl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc = PTHREAD_COND_INITIALIZER;

/* the producer's own state (one thread: spotify.c's audio thread) */
static long long t0, out_frames;
static int16_t cur[CHUNK * 2]; static size_t cur_n;
static int16_t hist[3 * 2]; static int nhist; static double pos;

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static long long now_ms(void) { return now_us() / 1000; }

int ssgroup_enable(int set) { if (set >= 0) atomic_store(&enabled, set != 0); return atomic_load(&enabled); }
/* not in a network: nobody could take the music, this Echo plays it itself as before */
int ssgroup_routing(void) { return atomic_load(&enabled) && server_id[0] && atomic_load(&net_ok); }
int ssgroup_is_self(const char *id) { return server_id[0] && id && !strcmp(id, server_id); }

int ssgroup_psk(const char *id, uint8_t out[32])
{
    uint8_t k[32];
    if (arb_derive("sendspin group v1", k)) return -1;
    hmac_sha256(k, 32, id, strlen(id), NULL, 0, out);
    crypto_wipe(k, sizeof k);
    return 0;
}

void ssgroup_volume(int v)
{
    if (v < 0 || v > 100) return;
    pthread_mutex_lock(&gl); atomic_store(&volume, v); pthread_cond_broadcast(&gc); pthread_mutex_unlock(&gl);
}

/* ---------------------------------------------------------------- the music in */

static void chunk_done(void)
{
    long long now = now_us(), ts = t0 + out_frames * 1000000LL / OUT_RATE;
    if (ts < now + MIN_AHEAD_US) { t0 += now + LEAD_US - ts; ts = now + LEAD_US; }     /* stalled: a gap, then on */
    pthread_mutex_lock(&gl);
    struct chunk *c = &ring[seq % RING];
    c->ts = ts; c->gen = gen; memcpy(c->pcm, cur, sizeof cur);
    seq++;
    pthread_cond_broadcast(&gc);
    pthread_mutex_unlock(&gl);
    out_frames += CHUNK; cur_n = 0;
}

/* Catmull-Rom through the input frames, a step of 44100/48000: hist holds the frames before this block */
static void resample_in(const int16_t *in, size_t frames)
{
    const double step = (double)IN_RATE / OUT_RATE;
    for (size_t i = 0; i < frames; i++) {
        if (nhist < 3) { memcpy(hist + 2 * nhist, in + 2 * i, 4); nhist++; continue; }
        /* p0 p1 p2 p3 = hist[0..2], in[i]; output between p1 and p2 at pos in [0, 1) */
        while (pos < 1.0) {
            double t = pos, t2 = t * t, t3 = t2 * t;
            for (int ch = 0; ch < 2; ch++) {
                double p0 = hist[ch], p1 = hist[2 + ch], p2 = hist[4 + ch], p3 = in[2 * i + ch];
                double v = 0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3);
                cur[2 * cur_n + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            if (++cur_n == CHUNK) chunk_done();
            pos += step;
        }
        pos -= 1.0;
        memmove(hist, hist + 2, 4 * sizeof *hist); memcpy(hist + 4, in + 2 * i, 4);
    }
}

void ssgroup_pcm(const int16_t *pcm, size_t frames)
{
    pthread_mutex_lock(&gl);
    int start = !playing;
    if (start) { playing = 1; gen++; }
    last_pcm_ms = now_ms();
    pthread_mutex_unlock(&gl);
    if (start) {
        t0 = now_us() + LEAD_US; out_frames = 0; cur_n = 0; nhist = 0; pos = 0;
        fprintf(stderr, "ssgroup: Spotify to the group\n");
    }
    resample_in(pcm, frames);
}

void ssgroup_idle(void)
{
    pthread_mutex_lock(&gl);
    int was = playing; playing = 0; pthread_cond_broadcast(&gc);
    pthread_mutex_unlock(&gl);
    if (was) fprintf(stderr, "ssgroup: Spotify stopped\n");
}

/* ---------------------------------------------------------------- one player */

struct conn {
    char ip[24]; int self; atomic_int alive, stop;
    struct ws ws; struct noise_cs tx, rx; pthread_mutex_t sl;
    int state_seen, ctl;                /* its first client/state came; it took the controller role */
};
static struct conn *conns[MAX_CONN];
static pthread_mutex_t cl = PTHREAD_MUTEX_INITIALIZER;

/* "key":"value" or "key":number in a flat object; enough for what a hassmic player sends */
static int jstr(const char *j, const char *key, char *out, size_t cap)
{
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k); if (!p) return 0;
    p += strlen(k); while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++; size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 1;
}
static int jnum(const char *j, const char *key, long long *v)
{
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k); if (!p) return 0;
    p += strlen(k); while (*p == ' ' || *p == ':') p++;
    char *e; *v = strtoll(p, &e, 10);
    return e != p;
}

static int send_plain(struct conn *c, const void *pt, size_t len)
{
    static __thread uint8_t *out; if (!out && !(out = malloc(70000))) return -1;
    if (len > 65519) return -1;
    pthread_mutex_lock(&c->sl);
    size_t n = noise_encrypt(&c->tx, pt, len, out);
    int r = ws_send(&c->ws, WS_BINARY, out, n);
    pthread_mutex_unlock(&c->sl);
    return r;
}

static int send_json(struct conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int send_json(struct conn *c, const char *fmt, ...)
{
    char b[2048]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b + 1, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0 || n >= (int)sizeof b - 1) return -1;
    b[0] = 0;
    return send_plain(c, b, (size_t)n + 1);
}

/* Cleartext init and KKpsk2 as the initiator: client/init, server/init + message 1, message 2 */
static int handshake(struct conn *c)
{
    int op; uint8_t *d; size_t n; char cid[64] = "", data[256], b64[256], init[256], hs1[512];
    uint8_t prologue[1024], cpub[32], psk[32], m1[256], m2[256], pl[256]; struct noise_hs hs;
    if (ws_recv(&c->ws, &op, &d, &n) != 1 || op != WS_TEXT || n > 512) return -1;               /* client/init */
    if (!strstr((char *)d, "\"client/init\"") || !jstr((char *)d, "client_id", cid, sizeof cid) || b64_decode(cid, strlen(cid), cpub, 32) != 32) return -1;
    memcpy(prologue, d, n); size_t pl_len = n;
    int il = snprintf(init, sizeof init, "{\"type\":\"server/init\",\"payload\":{\"server_id\":\"%s\",\"version\":1}}", server_id);
    memcpy(prologue + pl_len, init, (size_t)il); pl_len += (size_t)il;
    if (ssgroup_psk(server_id, psk)) return -1;
    /* psk_id = b64url(SHA-256("sendspin-psk-id-v1" || PSK)) */
    uint8_t idin[18 + 32], idh[32]; char pid[48];
    memcpy(idin, "sendspin-psk-id-v1", 18); memcpy(idin + 18, psk, 32); sha256(idin, sizeof idin, idh); b64_encode(idh, 32, pid, 1, 0);
    char p1[128]; int p1l = snprintf(p1, sizeof p1, "{\"psk_id\":\"%s\",\"psk_category\":\"lt\"}", pid);
    noise_kk_initiator_init(&hs, prologue, pl_len, key_priv, key_pub, cpub);
    size_t m1l = noise_kk_write_msg1(&hs, p1, (size_t)p1l, m1);
    b64_encode(m1, m1l, b64, 1, 0);
    int hl = snprintf(hs1, sizeof hs1, "{\"type\":\"noise/handshake\",\"payload\":{\"data\":\"%s\"}}", b64);
    if (ws_send(&c->ws, WS_TEXT, init, (size_t)il) || ws_send(&c->ws, WS_TEXT, hs1, (size_t)hl)) return -1;
    if (ws_recv(&c->ws, &op, &d, &n) != 1 || op != WS_TEXT || !jstr((char *)d, "data", data, sizeof data)) return -1;
    long m2l = b64_decode(data, strlen(data), m2, sizeof m2);
    if (m2l < 0 || noise_kk_read_msg2(&hs, psk, m2, (size_t)m2l, pl, &c->tx, &c->rx) < 0) { crypto_wipe(psk, 32); return -1; }
    crypto_wipe(psk, 32);
    return 0;
}

static void on_json(struct conn *c, const char *j, long long t_recv)
{
    char type[48] = "", cmd[32]; long long a;
    jstr(j, "type", type, sizeof type);
    if (!strcmp(type, "client/time") && jnum(j, "client_transmitted", &a)) {
        send_json(c, "{\"type\":\"server/time\",\"payload\":{\"client_transmitted\":%lld,\"server_received\":%lld,\"server_transmitted\":%lld}}",
                  a, t_recv, now_us());
    } else if (!strcmp(type, "client/hello")) {
        c->ctl = strstr(j, "controller@v1") != NULL;
        send_json(c, "{\"type\":\"server/activate\",\"payload\":{\"activities\":[\"playback\"],\"active_roles\":[\"player@v1\"%s]}}",
                  c->ctl ? ",\"controller@v1\"" : "");
        send_json(c, "{\"type\":\"group/update\",\"payload\":{\"playback_state\":\"playing\",\"group_id\":\"hassmic-spotify\",\"group_name\":\"Spotify\"}}");
        if (c->ctl)
            send_json(c, "{\"type\":\"server/state\",\"payload\":{\"controller\":{\"supported_commands\":[\"play\",\"pause\",\"next\",\"previous\"],"
                      "\"volume\":%d,\"muted\":false}}}", atomic_load(&volume) < 0 ? 50 : atomic_load(&volume));
    } else if (!strcmp(type, "client/state")) {
        c->state_seen = 1;
    } else if (!strcmp(type, "client/command") && jstr(j, "command", cmd, sizeof cmd)) {
        /* a player's button or "stop": Spotify's (core_lock: spotify.c's hooks run with it held) */
        pthread_mutex_lock(&core_lock);
        if (!strcmp(cmd, "pause") || !strcmp(cmd, "stop")) spotify_pause();
        else if (!strcmp(cmd, "play")) spotify_resume();
        else if (!strcmp(cmd, "next")) spotify_skip(1);
        else if (!strcmp(cmd, "previous")) spotify_skip(-1);
        pthread_mutex_unlock(&core_lock);
        fprintf(stderr, "ssgroup: %s from %s\n", cmd, c->self ? "this Echo" : c->ip);
    } else if (!strcmp(type, "client/goodbye")) {
        atomic_store(&c->stop, 1);
    }
}

/* the audio: stream/start per stretch of music, its chunks as they come, stream/end when it stops; volume changes */
static void *writer(void *arg)
{
    struct conn *c = arg; static __thread uint8_t pkt[1 + 8 + 4 + CHUNK * 4];
    int my_gen = -1, streaming = 0, sent_vol = -1; long long next = 0, last_ping = now_ms();
    while (!atomic_load(&c->stop)) {
        pthread_mutex_lock(&gl);
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_nsec += 200000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        while (!atomic_load(&c->stop) && (!c->state_seen || ((!playing || next >= seq) && streaming == playing && sent_vol == atomic_load(&volume))))
            if (pthread_cond_timedwait(&gc, &gl, &until) == ETIMEDOUT) break;
        int pl = playing, g = gen, v = atomic_load(&volume); long long s = seq;
        pthread_mutex_unlock(&gl);
        if (atomic_load(&c->stop)) break;
        if (now_ms() - last_ping > 20000) { pthread_mutex_lock(&c->sl); ws_send(&c->ws, WS_PING, "", 0); pthread_mutex_unlock(&c->sl); last_ping = now_ms(); }
        if (!c->state_seen) continue;
        if (v >= 0 && v != sent_vol) {
            if (send_json(c, "{\"type\":\"server/command\",\"payload\":{\"player\":{\"command\":\"volume\",\"volume\":%d}}}", v)) break;
            sent_vol = v;
        }
        if (!pl && streaming) {
            if (send_json(c, "{\"type\":\"stream/end\",\"payload\":{\"server_transmitted\":%lld,\"roles\":[\"player\"]}}", now_us())) break;
            streaming = 0;
            continue;
        }
        if (!pl) continue;
        if (g != my_gen) {
            if (send_json(c, "{\"type\":\"stream/start\",\"payload\":{\"server_transmitted\":%lld,\"player\":{\"codec\":\"pcm\",\"sample_rate\":%d,\"channels\":2,\"bit_depth\":16}}}",
                          now_us(), OUT_RATE)) break;
            my_gen = g; streaming = 1;
            /* from the oldest chunk of this stretch still in the ring and still ahead of time */
            next = s > RING ? s - RING : 0;
        }
        while (next < s && !atomic_load(&c->stop)) {
            pthread_mutex_lock(&gl);
            if (next < seq - RING) next = seq - RING;            /* overwritten while we waited */
            struct chunk *ch = &ring[next % RING];
            int ok = ch->gen == my_gen && ch->ts > now_us() + 50000;
            long long ts = ch->ts;
            if (ok) { pkt[0] = 4; for (int i = 0; i < 8; i++) pkt[1 + i] = (uint8_t)(ts >> (56 - 8 * i)); memset(pkt + 9, 0xff, 4); memcpy(pkt + 13, ch->pcm, CHUNK * 4); }
            pthread_mutex_unlock(&gl);
            next++;
            if (ok && send_plain(c, pkt, sizeof pkt)) { atomic_store(&c->stop, 1); break; }
        }
    }
    atomic_store(&c->stop, 1);
    shutdown(c->ws.fd, SHUT_RDWR);
    return NULL;
}

static void *conn_thread(void *arg)
{
    struct conn *c = arg; pthread_t w; int op; uint8_t *d, *pt = malloc(70000); size_t n;
    char host[24]; snprintf(host, sizeof host, "%s", c->ip);
    char *colon = strrchr(host, ':'); int port = colon ? atoi(colon + 1) : atomic_load(&ss_port);
    if (colon) *colon = 0;
    if (!pt || ws_connect(&c->ws, host, port, "/sendspin")) goto out;
    struct timeval tv = { 75, 0 }, stv = { 5, 0 };
    setsockopt(c->ws.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(c->ws.fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    if (handshake(c)) { fprintf(stderr, "ssgroup: %s: handshake failed (not in this network, or an older build)\n", c->self ? "this Echo" : c->ip); ws_close(&c->ws); goto out; }
    fprintf(stderr, "ssgroup: %s joined\n", c->self ? "this Echo" : c->ip);
    if (send_json(c, "{\"type\":\"server/hello\",\"payload\":{\"name\":\"Spotify on %s\"}}", core_name)) { ws_close(&c->ws); goto out; }
    pthread_create(&w, NULL, writer, c);
    while (!atomic_load(&c->stop)) {
        int r = ws_recv(&c->ws, &op, &d, &n);
        if (r != 1) break;
        long long t = now_us();
        if (op != WS_BINARY) continue;
        long m = noise_decrypt(&c->rx, d, n, pt);
        if (m < 1) break;
        if (pt[0] == 0) { pt[m] = 0; on_json(c, (char *)pt + 1, t); }
    }
    atomic_store(&c->stop, 1);
    pthread_mutex_lock(&gl); pthread_cond_broadcast(&gc); pthread_mutex_unlock(&gl);
    pthread_join(w, NULL);
    ws_close(&c->ws);
    fprintf(stderr, "ssgroup: %s left\n", c->self ? "this Echo" : c->ip);
out:
    free(pt);
    atomic_store(&c->alive, 0);
    return NULL;
}

/* ---------------------------------------------------------------- who is in the group */

/* "host:port" of every player: ours first, then the members' from arb_members_json ([{"node":..,"ip":..},...]) on
 * the same port as ours (every hassmic's default).  HASSMIC_SSGROUP_PEERS="host:port,..." (tests: Echos on one PC,
 * ours first) instead */
static int targets(char ips[][24], int max)
{
    static char js[4096]; int n = 0; const char *e = getenv("HASSMIC_SSGROUP_PEERS");
    if (e) {
        for (const char *p = e; *p && n < max; ) {
            size_t l = strcspn(p, ",");
            if (l > 0 && l < 24) { memcpy(ips[n], p, l); ips[n][l] = 0; n++; }
            p += l; if (*p) p++;
        }
        return n;
    }
    snprintf(ips[n++], 24, "127.0.0.1:%d", atomic_load(&ss_port));
    arb_members_json(js, sizeof js);
    for (const char *p = js; n < max && (p = strstr(p, "\"ip\":\"")); ) {
        p += 6; size_t l = strcspn(p, "\"");
        if (l > 0 && l < 16) { snprintf(ips[n], 24, "%.*s:%d", (int)l, p, atomic_load(&ss_port)); n++; }
        p += l;
    }
    return n;
}

static void key_load(void)
{
    const char *d = getenv("HASSMIC_STATE"); char p[300]; FILE *f;
    snprintf(p, sizeof p, "%s/ssgroup.key", d ? d : "/data/local/hassmic/state");
    if ((f = fopen(p, "rb"))) { size_t n = fread(key_priv, 1, 32, f); fclose(f); if (n == 32) goto have; }
    ws_random(key_priv, 32);
    if (statefile_write(p, 0600, key_priv, 32)) fprintf(stderr, "ssgroup: cannot write %s\n", p);
have:
    crypto_x25519_public_key(key_pub, key_priv);
    b64_encode(key_pub, 32, server_id, 1, 0);
}

static void *manager(void *arg)
{
    (void)arg; char ips[MAX_CONN][24]; long long tried[MAX_CONN] = { 0 }; char tried_ip[MAX_CONN][24] = { { 0 } };
    for (;;) {
        pthread_mutex_lock(&gl);
        int want = atomic_load(&enabled) && (playing || now_ms() - last_pcm_ms < LINGER_MS);
        pthread_mutex_unlock(&gl);
        uint8_t k[32]; int net = !arb_derive("sendspin group v1", k); crypto_wipe(k, sizeof k);
        atomic_store(&net_ok, net);
        int n = want && net ? targets(ips, MAX_CONN) : 0;
        pthread_mutex_lock(&cl);
        for (int i = 0; i < MAX_CONN; i++) {             /* gone, or no longer wanted */
            struct conn *c = conns[i];
            if (!c) continue;
            int keep = 0;
            for (int t = 0; t < n; t++) if (!strcmp(ips[t], c->ip)) keep = 1;
            if (!keep && atomic_load(&c->alive)) { atomic_store(&c->stop, 1); shutdown(c->ws.fd, SHUT_RDWR); }
            if (!atomic_load(&c->alive)) { pthread_mutex_destroy(&c->sl); free(c); conns[i] = NULL; }
        }
        for (int t = 0; t < n; t++) {                     /* new ones, a try every 10 s each */
            int have = 0, slot = -1, ti = -1;
            for (int i = 0; i < MAX_CONN; i++) { if (conns[i] && !strcmp(conns[i]->ip, ips[t])) have = 1; if (!conns[i] && slot < 0) slot = i; }
            for (int i = 0; i < MAX_CONN; i++) if (!strcmp(tried_ip[i], ips[t])) ti = i;
            if (ti < 0) for (int i = 0; i < MAX_CONN; i++) if (!tried_ip[i][0] || now_ms() - tried[i] > 600000) { ti = i; snprintf(tried_ip[i], 24, "%s", ips[t]); tried[i] = 0; break; }
            if (have || slot < 0 || ti < 0 || now_ms() - tried[ti] < 10000) continue;
            tried[ti] = now_ms();
            struct conn *c = calloc(1, sizeof *c); pthread_t th;
            if (!c) break;
            snprintf(c->ip, sizeof c->ip, "%s", ips[t]); c->self = t == 0; pthread_mutex_init(&c->sl, NULL);
            atomic_store(&c->alive, 1);
            if (pthread_create(&th, NULL, conn_thread, c)) { pthread_mutex_destroy(&c->sl); free(c); continue; }
            pthread_detach(th); conns[slot] = c;
        }
        pthread_mutex_unlock(&cl);
        usleep(1000000);
    }
    return NULL;
}

void ssgroup_start(int port)
{
    pthread_t t;
    atomic_store(&ss_port, port ? port : 28928);
    key_load();
    if (!pthread_create(&t, NULL, manager, NULL)) pthread_detach(t);
}
