/*
 * Spotify on every Echo: with the setting on, this Echo runs a second Spotify speaker, "Alle Echos" in the Spotify app
 * (hassmic-spotify again: a name, folder, ports 28938/28939 and a device id of its own; spotify_spawn_group), and is
 * a Sendspin server for the Echos of its arbitration network the owner picked (state/ssgroup.out lists those left
 * out), itself included; each plays that speaker's music through its own Sendspin player (sendspin.c), in step: the
 * players already follow a server's clock to the millisecond (Kalman filter, a rate of their own; sendspin.c).  The
 * Echo's own speaker stays as it was: picked in the app, it plays on this Echo alone.
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
 *   - client/command from a player's controller (its action button, multi presses, "<wake word>, stop"): to the
 *     group's speaker.
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
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
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

/* ---------------------------------------------------------------- the group's speaker */

static pthread_mutex_t spk = PTHREAD_MUTEX_INITIALIZER;
static int spk_in = -1, spk_running, spk_logged, spk_playing; static pid_t spk_pid;
static char spk_user[128], spk_title[200], spk_artist[200], spk_error[64], spk_name[80] = "Alle Echos";
static atomic_int spk_restart;

static const char *sdir(void) { const char *d = getenv("HASSMIC_STATE"); return d ? d : "/data/local/hassmic/state"; }

static void spk_send(const char *l)
{
    pthread_mutex_lock(&spk);
    if (spk_in >= 0) { char b[300]; int n = snprintf(b, sizeof b, "%s\n", l); if (write(spk_in, b, (size_t)n) < 0) {} }
    pthread_mutex_unlock(&spk);
}

static void spk_event(char *l)
{
    if (!strncmp(l, "pid ", 4)) { pthread_mutex_lock(&spk); spk_pid = (pid_t)atoi(l + 4); pthread_mutex_unlock(&spk); return; }
    if (strncmp(l, "track ", 6)) fprintf(stderr, "ssgroup: speaker < %s\n", l);
    pthread_mutex_lock(&spk);
    if (!strcmp(l, "ready") && !spotify_group_engine_soloist()) { pthread_mutex_unlock(&spk); spotify_group_mdns(1, spk_name); return; }
    if (!strncmp(l, "login ", 6)) { spk_logged = strcmp(l + 6, "-") != 0; snprintf(spk_user, sizeof spk_user, "%s", spk_logged ? l + 6 : ""); if (spk_logged) spk_error[0] = 0; }
    else if (!strncmp(l, "state ", 6)) spk_playing = !strcmp(l + 6, "playing");
    else if (!strncmp(l, "track ", 6)) {
        char *t = l + 6, *a = strchr(t, '\t'); if (a) *a++ = 0;
        char *e = a ? strchr(a, '\t') : NULL; if (e) *e = 0;
        snprintf(spk_title, sizeof spk_title, "%s", t); snprintf(spk_artist, sizeof spk_artist, "%s", a ? a : "");
    }
    else if (!strncmp(l, "error ", 6)) snprintf(spk_error, sizeof spk_error, "%s", l + 6);
    pthread_mutex_unlock(&spk);
    if (!strncmp(l, "volume ", 7)) ssgroup_volume(atoi(l + 7));       /* the app's slider: every Echo of the group */
}

static void *spk_audio(void *arg)
{
    int fd = (int)(long)arg; static int16_t buf[4096]; size_t have = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 300);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { ssgroup_idle(); continue; }      /* 300 ms without PCM: paused, stopped or between sessions */
        ssize_t n = read(fd, (char *)buf + have, sizeof buf - have);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        have += (size_t)n;
        size_t frames = have / 4;
        if (!frames) continue;
        ssgroup_pcm(buf, frames);
        have -= frames * 4;
        if (have) memmove(buf, (char *)buf + frames * 4, have);
    }
    ssgroup_idle();
    close(fd);
    return NULL;
}

static void name_load(void)
{
    char p[300]; FILE *f; snprintf(p, sizeof p, "%s/spotify/group_name", sdir());
    if ((f = fopen(p, "r"))) { char n[80]; if (fgets(n, sizeof n, f)) { n[strcspn(n, "\r\n")] = 0; if (n[0]) { pthread_mutex_lock(&spk); snprintf(spk_name, sizeof spk_name, "%s", n); pthread_mutex_unlock(&spk); } } fclose(f); }
}

static void *spk_supervisor(void *arg)
{
    (void)arg; int backoff = 5;
    name_load();
    for (;;) {
        atomic_store(&spk_restart, 0);
        if (!atomic_load(&enabled) || !spotify_enabled()) { while (!atomic_load(&spk_restart) && (!atomic_load(&enabled) || !spotify_enabled())) usleep(300000); continue; }
        int in_w, out_r, aud_r; char nm[80];
        pthread_mutex_lock(&spk); snprintf(nm, sizeof nm, "%s", spk_name); pthread_mutex_unlock(&spk);
        if (spotify_spawn_group(nm, &in_w, &out_r, &aud_r)) { fprintf(stderr, "ssgroup: the group's speaker would not start\n"); sleep(10); continue; }
        fprintf(stderr, "ssgroup: the group's speaker \"%s\" started\n", nm);
        long long started = now_ms();
        pthread_t at; pthread_create(&at, NULL, spk_audio, (void *)(long)aud_r);
        pthread_mutex_lock(&spk); spk_in = in_w; spk_running = 1; spk_logged = spk_playing = 0; spk_error[0] = 0; pthread_mutex_unlock(&spk);
        char line[2048]; size_t have = 0; int quit = 0; long long quit_at = 0;
        for (;;) {
            if (!quit && (atomic_load(&spk_restart) || !atomic_load(&enabled) || !spotify_enabled())) { spk_send("quit"); quit = 1; quit_at = now_ms(); }
            if (quit && now_ms() - quit_at > 5000) { pthread_mutex_lock(&spk); pid_t k = spk_pid; pthread_mutex_unlock(&spk); if (k > 0) kill(k, SIGKILL); quit_at = now_ms(); }
            struct pollfd p = { out_r, POLLIN, 0 };
            int r = poll(&p, 1, 200);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            ssize_t n = read(out_r, line + have, sizeof line - 1 - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            have += (size_t)n; line[have] = 0;
            char *s = line, *e;
            while ((e = strchr(s, '\n'))) { *e = 0; spk_event(s); s = e + 1; }
            have -= (size_t)(s - line); memmove(line, s, have);
            if (have == sizeof line - 1) have = 0;
        }
        close(out_r);
        pthread_mutex_lock(&spk); close(spk_in); spk_in = -1; spk_running = spk_playing = 0; pid_t was = spk_pid; spk_pid = 0; pthread_mutex_unlock(&spk);
        if (was > 0) kill(was, SIGTERM);
        pthread_join(at, NULL);
        spotify_group_mdns(0, "");
        fprintf(stderr, "ssgroup: the group's speaker ended\n");
        if (!quit && now_ms() - started < 60000) { for (int i = 0; i < backoff * 5 && !atomic_load(&spk_restart); i++) usleep(200000); backoff = backoff * 2 > 60 ? 60 : backoff * 2; }
        else backoff = 5;
    }
    return NULL;
}

int ssgroup_set_name(const char *n)
{
    size_t l = strlen(n);
    if (l > 79) return -1;
    for (const char *c = n; *c; c++) if ((unsigned char)*c < ' ' || *c == '<' || *c == '>' || *c == '&') return -1;
    char p[300]; snprintf(p, sizeof p, "%s/spotify", sdir()); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/spotify/group_name", sdir());
    if (statefile_write(p, 0600, n, l)) return -1;
    pthread_mutex_lock(&spk); snprintf(spk_name, sizeof spk_name, "%s", l ? n : "Alle Echos"); pthread_mutex_unlock(&spk);
    atomic_store(&spk_restart, 1);
    return 0;
}

void ssgroup_logout(void)
{
    spk_send("logout");
    pthread_mutex_lock(&spk); spk_logged = 0; spk_user[0] = 0; pthread_mutex_unlock(&spk);
}

/* Echos left out of the group, a node name a line (state/ssgroup.out): new members join by themselves */
static pthread_mutex_t outl = PTHREAD_MUTEX_INITIALIZER;
static char out_nodes[16][64]; static int n_out, out_loaded;
static void out_load(void)
{
    char p[300]; FILE *f; snprintf(p, sizeof p, "%s/ssgroup.out", sdir());
    n_out = 0;
    if ((f = fopen(p, "r"))) { char l[64]; while (n_out < 16 && fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0]) snprintf(out_nodes[n_out++], 64, "%s", l); } fclose(f); }
    out_loaded = 1;
}
static int is_out(const char *node)
{
    pthread_mutex_lock(&outl); if (!out_loaded) out_load();
    int r = 0; for (int i = 0; i < n_out; i++) if (!strcmp(out_nodes[i], node)) r = 1;
    pthread_mutex_unlock(&outl); return r;
}
int ssgroup_member(const char *node, int in)
{
    if (!node[0] || strlen(node) > 63 || strchr(node, '\n')) return -1;
    pthread_mutex_lock(&outl); if (!out_loaded) out_load();
    int k = 0; for (int i = 0; i < n_out; i++) if (strcmp(out_nodes[i], node)) memmove(out_nodes[k++], out_nodes[i], 64);
    n_out = k;
    if (!in && n_out < 16) snprintf(out_nodes[n_out++], 64, "%s", node);
    char p[300], b[16 * 65]; size_t o = 0; snprintf(p, sizeof p, "%s/ssgroup.out", sdir());
    for (int i = 0; i < n_out; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s\n", out_nodes[i]);
    int r = statefile_write(p, 0600, b, o);
    pthread_mutex_unlock(&outl);
    return r;
}
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
        /* a player's button or "stop": the group's speaker's */
        if (!strcmp(cmd, "pause") || !strcmp(cmd, "stop")) spk_send("pause");
        else if (!strcmp(cmd, "play")) spk_send("resume");
        else if (!strcmp(cmd, "next")) spk_send("next");
        else if (!strcmp(cmd, "previous")) spk_send("prev");
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

/* Every player of the network: node name and "host:port", ours first, then the members' from arb_members_json
 * ([{"node":..,"ip":..},...]) on our Sendspin port (every hassmic's default).  HASSMIC_SSGROUP_PEERS="node@host:port,..."
 * (tests: Echos on one PC, ours first) instead */
static int members(char ips[][24], char nodes[][64], int max)
{
    static char js[4096]; int n = 0; const char *e = getenv("HASSMIC_SSGROUP_PEERS");
    if (e) {
        for (const char *p = e; *p && n < max; ) {
            size_t l = strcspn(p, ","); char it[96];
            if (l > 0 && l < sizeof it) {
                memcpy(it, p, l); it[l] = 0;
                char *at = strchr(it, '@');
                snprintf(nodes[n], 64, "%s", at ? (*at = 0, it) : n ? "peer" : core_node_name());
                snprintf(ips[n], 24, "%s", at ? at + 1 : it); n++;
            }
            p += l; if (*p) p++;
        }
        return n;
    }
    snprintf(nodes[n], 64, "%s", core_node_name()); snprintf(ips[n++], 24, "127.0.0.1:%d", atomic_load(&ss_port));
    arb_members_json(js, sizeof js);
    for (const char *p = js; n < max && (p = strstr(p, "{\"node\":\"")); ) {
        p += 9; size_t l = strcspn(p, "\"");
        snprintf(nodes[n], 64, "%.*s", (int)(l < 63 ? l : 63), p);
        const char *q = strstr(p, "\"ip\":\""); if (!q) break;
        q += 6; size_t m = strcspn(q, "\"");
        if (m > 0 && m < 16) { snprintf(ips[n], 24, "%.*s:%d", (int)m, q, atomic_load(&ss_port)); n++; }
        p = q + m;
    }
    return n;
}

/* the players to serve: the members not left out */
static int targets(char ips[][24], int max)
{
    char all[MAX_CONN][24], nodes[MAX_CONN][64]; int n = members(all, nodes, MAX_CONN), k = 0;
    for (int i = 0; i < n && k < max; i++) if (!is_out(nodes[i])) snprintf(ips[k++], 24, "%s", all[i]);
    return k;
}

static void jesc(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(o + k, cap - k, "\\u%04x", c);
        else o[k++] = (char)c;
    }
    o[k] = 0;
}

size_t ssgroup_status_json(char *out, size_t cap)
{
    char u[260], t[400], a[400], e[130], nm[170], ips[MAX_CONN][24], nodes[MAX_CONN][64];
    int n = members(ips, nodes, MAX_CONN);
    pthread_mutex_lock(&spk);
    jesc(u, sizeof u, spk_user); jesc(t, sizeof t, spk_title); jesc(a, sizeof a, spk_artist); jesc(e, sizeof e, spk_error); jesc(nm, sizeof nm, spk_name);
    int run = spk_running, li = spk_logged, pl = spk_playing;
    pthread_mutex_unlock(&spk);
    size_t o = (size_t)snprintf(out, cap, "{\"on\":%d,\"network\":%d,\"running\":%d,\"logged_in\":%d,\"user\":\"%s\",\"playing\":%d,\"title\":\"%s\","
                                "\"artist\":\"%s\",\"error\":\"%s\",\"name\":\"%s\",\"port\":%d,\"members\":[",
                                atomic_load(&enabled), atomic_load(&net_ok), run, li, u, pl, t, a, e, nm, SPOTIFY_GROUP_PORT);
    for (int i = 0; i < n && o < cap; i++) {
        int joined = 0;
        pthread_mutex_lock(&cl);
        for (int k = 0; k < MAX_CONN; k++) if (conns[k] && atomic_load(&conns[k]->alive) && conns[k]->state_seen && !strcmp(conns[k]->ip, ips[i])) joined = 1;
        pthread_mutex_unlock(&cl);
        char nd[140]; jesc(nd, sizeof nd, nodes[i]);
        o += (size_t)snprintf(out + o, cap - o, "%s{\"node\":\"%s\",\"self\":%d,\"in\":%d,\"joined\":%d}", i ? "," : "", nd, i == 0, !is_out(nodes[i]), joined);
    }
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]}");
    return o < cap ? o : cap - 1;
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
    pthread_t t, u;
    atomic_store(&ss_port, port ? port : 28928);
    key_load();
    if (!pthread_create(&t, NULL, manager, NULL)) pthread_detach(t);
    if (!pthread_create(&u, NULL, spk_supervisor, NULL)) pthread_detach(u);
}
