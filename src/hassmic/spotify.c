/* Spotify Connect: hassmic-spotify (src/spotify/, on librespot) runs beside hassmic while the setting "spotify" is on.
 *
 * Process.  Started by double fork, as the log reader is (clock.c): Amazon's DHA module waits for all of hassmic's
 * children, and a speaker that never ends would hold it.  So it is init's child; it ends by itself when its stdin or
 * its audio pipe closes (hassmic ended or restarted).  Its effective group is hassmic's real one (3990, runas -r in
 * main.sh): lockdown.sh lets that group's sockets out, Amazon's daemons stay local, so the speaker reaches Spotify's
 * servers as hassmic reaches Home Assistant's media.  Inherited descriptors are closed: a listening socket of ours left
 * open in it would keep a port from the next hassmic.  It is restarted when it ends (5 s, doubling to 60 s while it
 * keeps ending within a minute).
 *
 * Discovery.  The zeroconf HTTP server is the daemon's, on SPOTIFY_PORT; the mDNS record is a service file of ours in
 * avahi's folder, as hassmic.service (avahi notices files written there), present only while the speaker runs.
 *
 * Audio.  44.1 kHz s16 stereo from a pipe (16 KiB, ~90 ms: what a pause or skip leaves to play), onto a stream of the
 * backend's own (spot_open: the mixer mixes it like the music stream; music_open is Sendspin's alone), ducked under the
 * assistant like Sendspin and the phone's Bluetooth audio, fed to the ring's music effects.  The output opens with the
 * first PCM and closes 300 ms after the last: that is what core_music hears, so the newest source wins as with the others.
 *
 * Volume.  The Echo's music volume is the speaker's: librespot attenuates nothing (src/spotify/src/main.rs EchoMixer),
 * the app's slider sets core_set_volume, the keys and Home Assistant send it back (equal values end the echo).
 *
 * Voice.  core_transcript hands the words over before Home Assistant's agent sees them (spotcmd.c says which); a command
 * cancels the run quietly as "stop" does.  The music starting is the answer; a refusal (no login, nothing found,
 * Spotify's API failing) is a falling two-tone (core_error_sound), and the page says why. */
#define _GNU_SOURCE
#include "spotify.h"
#include "spotcmd.h"
#include "audio.h"
#include "core.h"
#include "duck.h"
#include "musiclight.h"
#include "resample.h"
#include "settings.h"
#include "statefile.h"
#include "ssgroup.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RATE 44100
#define FRAME 4                         /* s16 stereo */

const char *const spotify_bitrates[3] = { "96", "160", "320" };
/* Which client plays (src/soloist/README.md): Spotify's own Soloist, where the build brings its runtime (glibc and its
 * libpulse next to hassmic); librespot refuses some accounts every track (its issue #1649).  Soloist needs a developer
 * API key from the owner (spotify_set_key) and is downloaded from Spotify by hassmic-spotify */
const char *const spotify_engines[2] = { "soloist", "librespot" };
#define SOLOIST_ZC_PORT 28936           /* its zeroconf listener, put in the firewall's range by its libpulse's bind() */
#define SOLOIST_WS_PORT 28937           /* its WebSocket API, on 127.0.0.1 only */

static _Atomic int enabled, bitrate = 1, normalise = 1, voice = 1, engine;
static _Atomic int restart_now;         /* a setting the daemon starts with changed: end it, the loop starts it again */

static pthread_mutex_t sl = PTHREAD_MUTEX_INITIALIZER;     /* what follows; never held while taking core_lock */
static int cmd_fd = -1;                 /* the daemon's stdin */
static pid_t pid;
static int running, ready, logged_in, playing, paused_by_button, last_volume = -1;
static long long paused_by_button_at, started_at;
static char user[128], track[3][160], result[160], name[80], last_error[160];
static long long result_at;

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static const char *state_dir(void) { const char *d = getenv("HASSMIC_STATE"); return d ? d : "/data/local/hassmic/state"; }

static void dir_path(char *out, size_t cap) { snprintf(out, cap, "%s/spotify", state_dir()); }

static const char *bin(void)
{
    static char p[PATH_MAX];
    const char *e = getenv("HASSMIC_SPOTIFY");
    if (e) return e;
    if (!p[0]) {                        /* next to hassmic itself: /system/hassmic, or an update's folder */
        ssize_t n = readlink("/proc/self/exe", p, sizeof p - 32);
        if (n <= 0) return "";
        p[n] = 0;
        char *s = strrchr(p, '/');
        strcpy(s ? s + 1 : p, "hassmic-spotify");
    }
    return p;
}

int spotify_present(void) { const char *b = bin(); return b[0] && access(b, X_OK) == 0; }

/* Soloist's runtime next to the speaker's binary (the bundle's flat folder) */
static void runtime_dir(char *out, size_t cap)
{
    snprintf(out, cap, "%s", bin());
    char *s = strrchr(out, '/');
    if (s) *s = 0; else snprintf(out, cap, ".");
}
int spotify_soloist_present(void)
{
    char d[PATH_MAX], p[PATH_MAX + 32];
    if (getenv("HASSMIC_SOLOIST_RUNTIME")) return 1;          /* tests */
    runtime_dir(d, sizeof d); snprintf(p, sizeof p, "%s/libpulse.so.0", d);
    return access(p, R_OK) == 0;
}
static int use_soloist(void) { return atomic_load(&engine) == 0 && spotify_soloist_present(); }

/* ---------------------------------------------------------------- settings */

int spotify_enable(int set) { if (set >= 0 && atomic_exchange(&enabled, set != 0) != (set != 0)) atomic_store(&restart_now, 1); return atomic_load(&enabled); }
int spotify_bitrate(int set) { if (set >= 0 && set < 3 && atomic_exchange(&bitrate, set) != set) atomic_store(&restart_now, 1); return atomic_load(&bitrate); }
int spotify_normalise(int set) { if (set >= 0 && atomic_exchange(&normalise, set != 0) != (set != 0)) atomic_store(&restart_now, 1); return atomic_load(&normalise); }
int spotify_voice(int set) { if (set >= 0) atomic_store(&voice, set != 0); return atomic_load(&voice); }
int spotify_engine(int set) { if (set >= 0 && set < 2 && atomic_exchange(&engine, set) != set) atomic_store(&restart_now, 1); return atomic_load(&engine); }

static void key_path(char *out, size_t cap) { snprintf(out, cap, "%s/spotify/soloist.key", state_dir()); }
static int has_key(void) { char p[300]; key_path(p, sizeof p); struct stat st; return !stat(p, &st) && st.st_size > 0; }
int spotify_set_key(const char *k)
{
    char p[300], d[300];
    size_t n = strlen(k);
    if (n > 200) return -1;
    for (const unsigned char *q = (const unsigned char *)k; *q; q++) if (*q <= ' ' || *q > '~') return -1;
    dir_path(d, sizeof d); mkdir(d, 0700); key_path(p, sizeof p);
    if (n ? statefile_write(p, 0600, k, n) : (unlink(p) && errno != ENOENT ? -1 : 0)) return -1;
    atomic_store(&restart_now, 1);
    fprintf(stderr, "spotify: Soloist API key %s\n", n ? "saved" : "removed");
    return 0;
}

/* The owner's Web API app (developer.spotify.com: Client ID and Secret) for searches with Soloist, which has no search
 * of its own: "id\nsecret" in state/spotify/webapi (0600), hassmic-spotify gets its path */
static void webapi_path(char *out, size_t cap) { snprintf(out, cap, "%s/spotify/webapi", state_dir()); }
static int has_webapi(void) { char p[300]; webapi_path(p, sizeof p); struct stat st; return !stat(p, &st) && st.st_size > 0; }
int spotify_set_webapi(const char *id, const char *secret)
{
    char p[300], d[300], t[300];
    size_t a = strlen(id), b = strlen(secret);
    if (a > 100 || b > 100 || !a != !b) return -1;
    for (const char *s = id; *s; s++) if (!isalnum((unsigned char)*s)) return -1;
    for (const char *s = secret; *s; s++) if (!isalnum((unsigned char)*s)) return -1;
    dir_path(d, sizeof d); mkdir(d, 0700); webapi_path(p, sizeof p);
    int n = snprintf(t, sizeof t, "%s\n%s\n", id, secret);
    int r = a ? statefile_write(p, 0600, t, (size_t)n) : (unlink(p) && errno != ENOENT ? -1 : 0);
    for (volatile char *v = t; v < t + sizeof t; v++) *v = 0;
    if (r) return -1;
    atomic_store(&restart_now, 1);
    fprintf(stderr, "spotify: Web API app for searches %s\n", a ? "saved" : "removed");
    return 0;
}

static void name_load(void)
{
    char p[300]; dir_path(p, sizeof p); strcat(p, "/name");
    FILE *f = fopen(p, "r");
    name[0] = 0;
    if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\r\n")] = 0; fclose(f); }
}

static void shown_name(char *out, size_t cap)
{
    pthread_mutex_lock(&sl);
    snprintf(out, cap, "%s", name[0] ? name : core_name);
    pthread_mutex_unlock(&sl);
}

int spotify_set_name(const char *n)
{
    size_t len = strlen(n);
    if (len >= sizeof name) return -1;
    for (const unsigned char *p = (const unsigned char *)n; *p; p++) if (*p < 0x20 || *p == '<' || *p == '>' || *p == '&') return -1;
    char p[300]; dir_path(p, sizeof p); mkdir(p, 0700); strcat(p, "/name");
    if (len ? statefile_write(p, 0600, n, len) : (unlink(p) && errno != ENOENT ? -1 : 0)) return -1;
    pthread_mutex_lock(&sl); snprintf(name, sizeof name, "%s", n); pthread_mutex_unlock(&sl);
    atomic_store(&restart_now, 1);
    fprintf(stderr, "spotify: shown as \"%s\"\n", len ? n : core_name);
    return 0;
}

/* ---------------------------------------------------------------- commands to the daemon */

static int send_locked(const char *line)
{
    if (cmd_fd < 0) return -1;
    size_t n = strlen(line);
    char buf[600];
    if (n + 2 > sizeof buf) return -1;
    memcpy(buf, line, n); buf[n++] = '\n';
    return write(cmd_fd, buf, n) == (ssize_t)n ? 0 : -1;   /* non-blocking: a line is far below the pipe's size */
}

static int spot_send(const char *line) { pthread_mutex_lock(&sl); int r = send_locked(line); pthread_mutex_unlock(&sl); return r; }

void spotify_logout(void)
{
    char p[300]; dir_path(p, sizeof p); strcat(p, "/credentials.json");
    pthread_mutex_lock(&sl);
    if (send_locked("logout")) unlink(p);           /* not running: the file goes here */
    logged_in = 0; user[0] = 0; playing = 0;
    pthread_mutex_unlock(&sl);
    fprintf(stderr, "spotify: logged out\n");
}

void spotify_volume_changed(int v)
{
    char l[32];
    pthread_mutex_lock(&sl);
    if (running && v != last_volume) { last_volume = v; snprintf(l, sizeof l, "volume %d", v); send_locked(l); }
    pthread_mutex_unlock(&sl);
}

void spotify_pause(void)
{
    pthread_mutex_lock(&sl);
    if (playing) { send_locked("pause"); fprintf(stderr, "spotify: paused, another source plays\n"); }
    pthread_mutex_unlock(&sl);
}

int spotify_stop(void)
{
    pthread_mutex_lock(&sl);
    int was = playing && !send_locked("pause");
    pthread_mutex_unlock(&sl);
    if (was) fprintf(stderr, "spotify: paused (stop)\n");
    return was;
}

int spotify_button(void)
{
    int used = 0;
    pthread_mutex_lock(&sl);
    if (playing) { if (!send_locked("pause")) { paused_by_button = 1; paused_by_button_at = mono_ms(); used = 1; } }
    else if (paused_by_button && mono_ms() - paused_by_button_at < 1800000LL && running) { send_locked("resume"); paused_by_button = 0; used = 1; }
    pthread_mutex_unlock(&sl);
    if (used) fprintf(stderr, "spotify: button -> %s\n", paused_by_button ? "pause" : "play");
    return used;
}

void spotify_resume(void)
{
    pthread_mutex_lock(&sl);
    if (running && !playing) send_locked("resume");
    paused_by_button = 0;
    pthread_mutex_unlock(&sl);
}

int spotify_skip(int dir)
{
    pthread_mutex_lock(&sl);
    int used = playing && !send_locked(dir > 0 ? "next" : "prev");
    pthread_mutex_unlock(&sl);
    return used;
}

int spotify_command(const char *text, const char *wake)
{
    char cmd[300];
    if (!atomic_load(&enabled) || !atomic_load(&voice)) return 0;
    pthread_mutex_lock(&sl);
    int now = playing, up = running && ready;
    pthread_mutex_unlock(&sl);
    if (!spotcmd_parse(text, wake, now, cmd, sizeof cmd)) return 0;
    fprintf(stderr, "spotify: said \"%s\" -> %s\n", text, cmd);
    if (!up || spot_send(cmd)) {
        pthread_mutex_lock(&sl);
        snprintf(result, sizeof result, "fail notrunning"); result_at = mono_ms();
        pthread_mutex_unlock(&sl);
        fprintf(stderr, "spotify: not running, \"%s\" refused\n", cmd);
        core_error_sound();
    }
    return 1;
}

/* ---------------------------------------------------------------- the page */

static size_t jesc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

size_t spotify_status_json(char *out, size_t cap)
{
    char u[260], t[3][330], r[330], nm[170], dn[170], e[330]; static char g[3000];
    ssgroup_status_json(g, sizeof g);
    pthread_mutex_lock(&sl);
    jesc(u, sizeof u, user); for (int i = 0; i < 3; i++) jesc(t[i], sizeof t[i], track[i]);
    jesc(r, sizeof r, result); jesc(nm, sizeof nm, name); jesc(dn, sizeof dn, core_name); jesc(e, sizeof e, last_error);
    int run = running, rd = ready, li = logged_in, pl = playing; long long age = result_at ? (mono_ms() - result_at) / 1000 : -1;
    pthread_mutex_unlock(&sl);
    return (size_t)snprintf(out, cap,
        "{\"present\":%d,\"enabled\":%d,\"running\":%d,\"ready\":%d,\"logged_in\":%d,\"user\":\"%s\",\"playing\":%d,"
        "\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"result\":\"%s\",\"result_age\":%lld,\"name\":\"%s\","
        "\"default_name\":\"%s\",\"port\":%d,\"error\":\"%s\",\"engine\":\"%s\",\"soloist\":%d,\"has_key\":%d,\"has_webapi\":%d,\"group\":%s}",
        spotify_present(), atomic_load(&enabled), run, rd, li, u, pl, t[0], t[1], t[2], r, age, nm, dn,
        use_soloist() ? SOLOIST_ZC_PORT : SPOTIFY_PORT, e, use_soloist() ? "soloist" : "librespot", spotify_soloist_present(), has_key(), has_webapi(), g);
}

/* ---------------------------------------------------------------- mDNS */

static const char *mdns_path(void) { const char *p = getenv("HASSMIC_SPOTIFY_MDNS"); return p ? p : "/data/misc/avahi/services/hassmic-spotify.service"; }

static void mdns_write(int on)
{
    if (!on) { if (unlink(mdns_path()) == 0) fprintf(stderr, "spotify: mDNS record withdrawn\n"); return; }
    char n[80]; shown_name(n, sizeof n);
    struct statefile s;
    if (statefile_open(&s, mdns_path(), 0644)) { fprintf(stderr, "spotify: cannot write %s\n", mdns_path()); return; }
    fchmod(fileno(s.f), 0644);                          /* avahi reads it: past hassmic's umask */
    /* the name is plain text (spotify_set_name refuses <>&); %h would be avahi's host name */
    fprintf(s.f, "<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<service-group>\n  <name>%s</name>\n  <service><type>_spotify-connect._tcp</type><port>%d</port>\n"
            "    <txt-record>VERSION=1.0</txt-record><txt-record>CPath=/</txt-record><txt-record>Stack=SP</txt-record>\n"
            "  </service>\n</service-group>\n", n, SPOTIFY_PORT);
    if (statefile_commit(&s)) fprintf(stderr, "spotify: cannot write %s\n", mdns_path());
}

void spotify_group_mdns(int on, const char *name)
{
    char p[320]; const char *e = getenv("HASSMIC_SPOTIFY_MDNS");
    if (e) snprintf(p, sizeof p, "%s.group", e); else snprintf(p, sizeof p, "/data/misc/avahi/services/hassmic-spotify-group.service");
    if (!on) { unlink(p); return; }
    struct statefile s;
    if (statefile_open(&s, p, 0644)) return;
    fchmod(fileno(s.f), 0644);
    fprintf(s.f, "<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<service-group>\n  <name>%s</name>\n  <service><type>_spotify-connect._tcp</type><port>%d</port>\n"
            "    <txt-record>VERSION=1.0</txt-record><txt-record>CPath=/</txt-record><txt-record>Stack=SP</txt-record>\n"
            "  </service>\n</service-group>\n", name, SPOTIFY_GROUP_PORT);
    statefile_commit(&s);
}

/* ---------------------------------------------------------------- audio */

static void *audio_thread(void *arg)
{
    int fd = (int)(long)arg, open_ = 0;
    static int16_t buf[4096];
    size_t have = 0; float duck_g = 1;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 300);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) {                                   /* 300 ms without PCM: paused, stopped or between sessions */
            if (open_) { spot_close(); open_ = 0; core_music(MUSIC_SPOTIFY, 0); fprintf(stderr, "spotify: output closed\n"); }
            continue;
        }
        ssize_t n = read(fd, (char *)buf + have, sizeof buf - have);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        have += (size_t)n;
        size_t frames = have / FRAME;
        if (!frames) continue;
        if (!open_) {
            if (spot_open(RATE, 2)) { fprintf(stderr, "spotify: cannot open the output\n"); usleep(200000); have = 0; continue; }
            open_ = 1; core_music(MUSIC_SPOTIFY, 1); fprintf(stderr, "spotify: output open\n");
        }
        music_duck(buf, frames, RATE, &duck_g, duck_active() ? settings_duck_gain() : 1);   /* under the voice assistant, ours or another Echo's (duck.c) */
        musiclight_feed(buf, frames, 2, RATE, spot_queued_us());
        spot_write(buf, frames * FRAME);
        have -= frames * FRAME;
        if (have) memmove(buf, (char *)buf + frames * FRAME, have);
    }
    if (open_) { spot_close(); core_music(MUSIC_SPOTIFY, 0); }
    close(fd);
    return NULL;
}

/* ---------------------------------------------------------------- the daemon */

/* hex of SHA-1 over the Echo's node name: Spotify's device id, the same however the speaker is named, so the app keeps
 * one entry for it.  hash.c's sha1 */
#include "hash.h"
static void device_id(char out[41])
{
    uint8_t h[20]; const char *n = core_node_name();
    sha1(n, strlen(n), h);
    for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static int exec_speaker(char *const argv[], int *in_w, int *out_r, int *aud_r);
static int spawn(int *in_w, int *out_r, int *aud_r)
{
    char dir[300], did[41], vol[8], br[8], nm[80];
    dir_path(dir, sizeof dir); mkdir(dir, 0700);
    device_id(did); shown_name(nm, sizeof nm);
    pthread_mutex_lock(&core_lock); int v = core_volume(); pthread_mutex_unlock(&core_lock);
    snprintf(vol, sizeof vol, "%d", v); snprintf(br, sizeof br, "%s", spotify_bitrates[atomic_load(&bitrate)]);
    int sol = use_soloist();
    char port[8], wsp[8], rt[PATH_MAX], key[300], wa[300];
    snprintf(port, sizeof port, "%d", sol ? SOLOIST_ZC_PORT : SPOTIFY_PORT); snprintf(wsp, sizeof wsp, "%d", SOLOIST_WS_PORT);
    runtime_dir(rt, sizeof rt); key_path(key, sizeof key); webapi_path(wa, sizeof wa);
    if (sol) { strncat(dir, "/soloist", sizeof dir - strlen(dir) - 1); mkdir(dir, 0700); }
    const char *b = bin();
    char *argv[] = { (char *)b, "--name", nm, "--dir", dir, "--port", port, "--device-id", did, "--bitrate", br,
                     "--normalise", atomic_load(&normalise) ? "1" : "0", "--volume", vol, "--audio-fd", "3",
                     "--engine", sol ? "soloist" : "librespot", "--runtime", getenv("HASSMIC_SOLOIST_RUNTIME") ? getenv("HASSMIC_SOLOIST_RUNTIME") : rt,
                     "--soloist-key-file", key, "--ws-port", wsp, "--webapi-file", wa, NULL };
    return exec_speaker(argv, in_w, out_r, aud_r);
}

/* The group's speaker (ssgroup.c): the same program, a name, folder, ports and device id of its own (machine-id for
 * Soloist: hmpulse.c), its music for the group's server instead of this Echo's stream */
int spotify_spawn_group(const char *name, int *in_w, int *out_r, int *aud_r)
{
    char dir[300], did[41], mid[33], vol[8], br[8], nm[80], rt[PATH_MAX], key[300], wa[300], port[8], wsp[8];
    uint8_t h[20]; char seed[120];
    if (!spotify_present()) return -1;
    dir_path(dir, sizeof dir); mkdir(dir, 0700); strncat(dir, "/group", sizeof dir - strlen(dir) - 1); mkdir(dir, 0700);
    snprintf(seed, sizeof seed, "%s group", core_node_name()); sha1(seed, strlen(seed), h);
    for (int i = 0; i < 20; i++) sprintf(did + 2 * i, "%02x", h[i]);
    memcpy(mid, did, 32); mid[32] = 0;
    snprintf(nm, sizeof nm, "%s", name);
    pthread_mutex_lock(&core_lock); int v = core_volume(); pthread_mutex_unlock(&core_lock);
    snprintf(vol, sizeof vol, "%d", v); snprintf(br, sizeof br, "%s", spotify_bitrates[atomic_load(&bitrate)]);
    int sol = use_soloist();
    snprintf(port, sizeof port, "%d", SPOTIFY_GROUP_PORT); snprintf(wsp, sizeof wsp, "%d", SPOTIFY_GROUP_WS_PORT);
    runtime_dir(rt, sizeof rt); key_path(key, sizeof key); webapi_path(wa, sizeof wa);
    const char *b = bin();
    char *argv[] = { (char *)b, "--name", nm, "--dir", dir, "--port", port, "--device-id", did, "--bitrate", br,
                     "--normalise", atomic_load(&normalise) ? "1" : "0", "--volume", vol, "--audio-fd", "3",
                     "--engine", sol ? "soloist" : "librespot", "--runtime", getenv("HASSMIC_SOLOIST_RUNTIME") ? getenv("HASSMIC_SOLOIST_RUNTIME") : rt,
                     "--soloist-key-file", key, "--ws-port", wsp, "--webapi-file", wa, "--machine-id", mid, NULL };
    return exec_speaker(argv, in_w, out_r, aud_r);
}
int spotify_group_engine_soloist(void) { return use_soloist(); }
int spotify_enabled(void) { return atomic_load(&enabled); }

/* hassmic-spotify on three pipes (stdin, stdout, PCM on fd 3), its grandchild's pid as the first line on stdout */
static int exec_speaker(char *const argv[], int *in_w, int *out_r, int *aud_r)
{
    int pin[2], pout[2], paud[2];
    if (pipe2(pin, O_CLOEXEC)) return -1;
    if (pipe2(pout, O_CLOEXEC)) { close(pin[0]); close(pin[1]); return -1; }
    if (pipe2(paud, O_CLOEXEC)) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return -1; }
    fcntl(paud[0], F_SETPIPE_SZ, 16384);                /* ~90 ms of PCM: what a pause or a skip leaves behind */
    pid_t c = fork();
    if (c == 0) {
        /* only async-signal-safe calls from here to exec: hassmic has threads */
        pid_t g = fork();
        if (g != 0) {                                   /* the grandchild's pid to the reader, then out */
            char l[24]; int k = 0; unsigned long x = (unsigned long)g; char d[20]; int m = 0;
            if (g > 0) { l[k++] = 'p'; l[k++] = 'i'; l[k++] = 'd'; l[k++] = ' '; do d[m++] = (char)('0' + x % 10); while ((x /= 10)); while (m) l[k++] = d[--m]; l[k++] = '\n'; if (write(pout[1], l, (size_t)k) < 0) _exit(1); }
            _exit(g < 0);
        }
        sigset_t all; sigemptyset(&all); sigprocmask(SIG_SETMASK, &all, NULL);
        for (int s = 1; s < 32; s++) if (s != SIGKILL && s != SIGSTOP) signal(s, SIG_DFL);
        setsid();
        dup2(pin[0], 0); dup2(pout[1], 1); dup2(paud[1], 3);
        for (int fd = 4; fd < 1024; fd++) close(fd);    /* sockets of ours without CLOEXEC: never into the speaker */
        gid_t r, e, s;
        if (!getresgid(&r, &e, &s) && r != e && setresgid(r, r, r)) _exit(126);   /* the network group (lockdown.sh) */
        execv(argv[0], argv);
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(paud[1]);
    if (c < 0) { close(pin[1]); close(pout[0]); close(paud[0]); return -1; }
    while (waitpid(c, NULL, 0) < 0 && errno == EINTR) {}
    fcntl(pin[1], F_SETFL, O_NONBLOCK);
    *in_w = pin[1]; *out_r = pout[0]; *aud_r = paud[0];
    return 0;
}

/* One event line (src/spotify/src/main.rs lists them) */
static void event(char *l)
{
    if (!strncmp(l, "pid ", 4)) { pthread_mutex_lock(&sl); pid = (pid_t)atoi(l + 4); pthread_mutex_unlock(&sl); return; }
    fprintf(stderr, "spotify: < %s\n", !strncmp(l, "track ", 6) ? "track" : l);
    /* Soloist announces itself (its own mDNS, beside avahi), and its zeroconf port is another */
    if (!strcmp(l, "ready")) { pthread_mutex_lock(&sl); ready = 1; pthread_mutex_unlock(&sl); if (!use_soloist()) mdns_write(1); return; }
    if (!strncmp(l, "login ", 6)) {
        pthread_mutex_lock(&sl);
        logged_in = strcmp(l + 6, "-") != 0; snprintf(user, sizeof user, "%s", logged_in ? l + 6 : "");
        if (!logged_in) playing = 0; else last_error[0] = 0;        /* a session is up: what went wrong before is over */
        pthread_mutex_unlock(&sl);
        return;
    }
    if (!strncmp(l, "state ", 6)) {
        int on = !strcmp(l + 6, "playing");
        pthread_mutex_lock(&sl); playing = on; if (on) paused_by_button = 0; pthread_mutex_unlock(&sl);
        return;
    }
    if (!strncmp(l, "track ", 6)) {
        char *f[5] = { l + 6 }; int k = 1;
        for (char *p = l + 6; *p && k < 5; p++) if (*p == '\t') { *p = 0; f[k++] = p + 1; }
        pthread_mutex_lock(&sl);
        for (int i = 0; i < 3; i++) snprintf(track[i], sizeof track[i], "%s", i < k ? f[i] : "");
        pthread_mutex_unlock(&sl);
        fprintf(stderr, "spotify: now %s - %s\n", track[1], track[0]);
        return;
    }
    if (!strncmp(l, "volume ", 7)) {                    /* the app's slider: the Echo's music volume */
        int v = atoi(l + 7);
        if (v < 0 || v > 100) return;
        pthread_mutex_lock(&sl); last_volume = v; pthread_mutex_unlock(&sl);
        pthread_mutex_lock(&core_lock);
        if (core_volume() != v) core_set_volume(v);
        pthread_mutex_unlock(&core_lock);
        return;
    }
    if (!strncmp(l, "error ", 6)) {                     /* why no session comes up: clock, unreachable, refused (the page says it) */
        pthread_mutex_lock(&sl); snprintf(last_error, sizeof last_error, "%s", l + 6); pthread_mutex_unlock(&sl);
        return;
    }
    if (!strncmp(l, "result ", 7)) {
        const char *r = strchr(l + 7, ' ');
        pthread_mutex_lock(&sl); snprintf(result, sizeof result, "%s", r ? r + 1 : ""); result_at = mono_ms(); pthread_mutex_unlock(&sl);
        if (r && !strncmp(r + 1, "fail", 4)) core_error_sound();
        return;
    }
}

static void *supervisor(void *arg)
{
    (void)arg;
    int backoff = 5;
    name_load();
    for (;;) {
        atomic_store(&restart_now, 0);
        if (!atomic_load(&enabled) || !spotify_present()) {
            if (atomic_load(&enabled)) {
                pthread_mutex_lock(&sl); snprintf(last_error, sizeof last_error, "%s is missing", bin()); pthread_mutex_unlock(&sl);
            }
            while (!atomic_load(&restart_now)) usleep(200000);
            continue;
        }
        int in_w, out_r, aud_r;
        if (spawn(&in_w, &out_r, &aud_r)) { fprintf(stderr, "spotify: cannot start %s: %s\n", bin(), strerror(errno)); sleep(10); continue; }
        fprintf(stderr, "spotify: started %s (%s)\n", bin(), use_soloist() ? "Soloist" : "librespot");
        pthread_t at; pthread_create(&at, NULL, audio_thread, (void *)(long)aud_r);
        pthread_mutex_lock(&sl);
        cmd_fd = in_w; running = 1; ready = 0; logged_in = 0; playing = 0; started_at = mono_ms(); last_volume = -1; last_error[0] = 0;
        pthread_mutex_unlock(&sl);
        char line[2048]; size_t have = 0; int quit_sent = 0; long long quit_at = 0;
        for (;;) {
            struct pollfd p = { out_r, POLLIN, 0 };
            if (atomic_load(&restart_now) && !quit_sent) { spot_send("quit"); quit_sent = 1; quit_at = mono_ms(); }
            if (quit_sent && mono_ms() - quit_at > 5000) {      /* a speaker that does not end when told: ended */
                pthread_mutex_lock(&sl); pid_t k = pid; pthread_mutex_unlock(&sl);
                if (k > 0) { fprintf(stderr, "spotify: did not end, killed\n"); kill(k, SIGKILL); }
                quit_at = mono_ms();
            }
            int r = poll(&p, 1, 200);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            ssize_t n = read(out_r, line + have, sizeof line - 1 - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            have += (size_t)n; line[have] = 0;
            char *s = line, *e;
            while ((e = strchr(s, '\n'))) { *e = 0; if (e > s && e[-1] == '\r') e[-1] = 0; event(s); s = e + 1; }
            have -= (size_t)(s - line);
            memmove(line, s, have);
            if (have == sizeof line - 1) have = 0;          /* a line longer than any event: dropped */
        }
        close(out_r);
        pthread_mutex_lock(&sl);
        close(cmd_fd); cmd_fd = -1; running = 0; ready = 0; playing = 0;
        pid_t was = pid; pid = 0;
        long long ran = mono_ms() - started_at;
        pthread_mutex_unlock(&sl);
        if (was > 0) kill(was, SIGTERM);                /* stdout closed but still there: no leftover speaker */
        pthread_join(at, NULL);
        mdns_write(0);
        fprintf(stderr, "spotify: ended after %lld s\n", ran / 1000);
        if (atomic_load(&enabled) && ran < 60000 && !quit_sent) {
            pthread_mutex_lock(&sl); snprintf(last_error, sizeof last_error, "ended after %lld s", ran / 1000); pthread_mutex_unlock(&sl);
            for (int i = 0; i < backoff * 5 && !atomic_load(&restart_now); i++) usleep(200000);
            backoff = backoff * 2 > 60 ? 60 : backoff * 2;
        } else backoff = 5;
    }
    return NULL;
}

void spotify_start(void)
{
    pthread_t t;
    mdns_write(0);                                      /* one a crash left behind */
    if (pthread_create(&t, NULL, supervisor, NULL)) { fprintf(stderr, "spotify: no supervisor thread\n"); return; }
    pthread_detach(t);
}
