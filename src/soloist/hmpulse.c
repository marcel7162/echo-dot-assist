/* libpulse.so.0 for Spotify Soloist on the Echo: the 50 functions of PulseAudio's asynchronous client API Soloist
 * loads (dlopen, after PipeWire is not found), without a PulseAudio server.  The Echo has none, and Soloist (Spotify's
 * own Connect client, src/soloist/README.md) plays nowhere else.  Its float32 PCM goes out as s16le stereo at the
 * stream's rate, paced by the clock, to:
 *   HMPULSE_FD=<n>              an inherited descriptor (hassmic's pipe, as hassmic-spotify's PCM: spotify.c)
 *   HMPULSE_TCP=<ip>:<port>     a TCP connection, made again when it drops (the Echo's mixplay -l: a test by hand)
 * The volume Soloist sets on its sink input is applied here unless HMPULSE_VOLUME=report, which leaves the PCM at full
 * scale and writes "volume <0..100>" lines to HMPULSE_EVENTS_FD (hassmic owns the Echo's volume).
 *
 * Also bind(): Soloist's zeroconf server takes any free port (34449 on a PC), and the Echo's firewall admits TCP on
 * 16384-32767 only (lockdown.sh).  With HMPULSE_ZC_PORT a TCP socket bound to port 0 gets that port instead (the first
 * one; a second falls back to the kernel's).  This library is preloaded (LD_PRELOAD) for that, so Soloist's dlopen of
 * libpulse.so.0 finds it loaded already.
 *
 * Built with the glibc armhf toolchain against PulseAudio's own headers (Makefile), so the structs Soloist reads
 * (pa_timing_info, pa_sink_input_info, pa_cvolume) are laid out as libpulse's. */
#define _GNU_SOURCE
#undef _FORTIFY_SOURCE          /* open() and fopen() are ours below (machine-id): no fortify wrappers over them */
#include <pulse/pulseaudio.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default")))

static void logf_(const char *fmt, ...)
{
    if (!getenv("HMPULSE_DEBUG")) return;
    va_list a; va_start(a, fmt);
    fprintf(stderr, "hmpulse: "); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a);
}

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

/* ---------------------------------------------------------------- the threaded mainloop and its task queue */

struct task { void (*fn)(void *); void *arg; struct task *next; };

struct pa_threaded_mainloop {
    pthread_mutex_t lock;               /* the application's lock: callbacks run with it held, as libpulse's */
    pthread_cond_t cond, accept;        /* pa_threaded_mainloop_wait/signal */
    pthread_mutex_t qlock;              /* the task queue only, never held while a task runs */
    pthread_cond_t qcond;
    struct task *head, *tail;
    pthread_t thread;
    int running, quit, in_thread;
    pa_mainloop_api api;
};

static void post(pa_threaded_mainloop *m, void (*fn)(void *), void *arg)
{
    struct task *t = calloc(1, sizeof *t);
    if (!t) return;
    t->fn = fn; t->arg = arg;
    pthread_mutex_lock(&m->qlock);
    if (m->tail) m->tail->next = t; else m->head = t;
    m->tail = t;
    pthread_cond_signal(&m->qcond);
    pthread_mutex_unlock(&m->qlock);
}

static void *loop_thread(void *arg)
{
    pa_threaded_mainloop *m = arg;
    for (;;) {
        pthread_mutex_lock(&m->qlock);
        while (!m->head && !m->quit) pthread_cond_wait(&m->qcond, &m->qlock);
        if (m->quit && !m->head) { pthread_mutex_unlock(&m->qlock); break; }
        struct task *t = m->head;
        m->head = t->next; if (!m->head) m->tail = NULL;
        pthread_mutex_unlock(&m->qlock);
        pthread_mutex_lock(&m->lock);
        t->fn(t->arg);
        pthread_cond_broadcast(&m->cond);         /* whoever waits for an operation or a state looks again */
        pthread_mutex_unlock(&m->lock);
        free(t);
    }
    return NULL;
}

API pa_threaded_mainloop *pa_threaded_mainloop_new(void)
{
    pa_threaded_mainloop *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->lock, &a);
    pthread_cond_init(&m->cond, NULL); pthread_cond_init(&m->accept, NULL);
    pthread_mutex_init(&m->qlock, NULL); pthread_cond_init(&m->qcond, NULL);
    m->api.userdata = m;
    return m;
}
API int pa_threaded_mainloop_start(pa_threaded_mainloop *m)
{
    if (m->running) return 0;
    m->quit = 0;
    if (pthread_create(&m->thread, NULL, loop_thread, m)) return -1;
    m->running = 1;
    return 0;
}
API void pa_threaded_mainloop_stop(pa_threaded_mainloop *m)
{
    if (!m->running) return;
    pthread_mutex_lock(&m->qlock); m->quit = 1; pthread_cond_signal(&m->qcond); pthread_mutex_unlock(&m->qlock);
    if (!pthread_equal(pthread_self(), m->thread)) pthread_join(m->thread, NULL);
    m->running = 0;
}
API void pa_threaded_mainloop_free(pa_threaded_mainloop *m) { if (!m) return; pa_threaded_mainloop_stop(m); free(m); }
API void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) { pthread_mutex_lock(&m->lock); }
API void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) { pthread_mutex_unlock(&m->lock); }
/* the lock is recursive (callbacks call back in); waiting needs it held once, as with libpulse */
API void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) { pthread_cond_wait(&m->cond, &m->lock); }
API void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept) { (void)wait_for_accept; pthread_cond_broadcast(&m->cond); }
API pa_mainloop_api *pa_threaded_mainloop_get_api(pa_threaded_mainloop *m) { return &m->api; }

/* ---------------------------------------------------------------- operations */

struct pa_operation { int refs; pa_operation_state_t state; pa_threaded_mainloop *m; };

static pa_operation *op_new(pa_threaded_mainloop *m)
{
    pa_operation *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->refs = 2;                        /* the caller's and the task's */
    o->state = PA_OPERATION_RUNNING; o->m = m;
    return o;
}
API void pa_operation_unref(pa_operation *o) { if (o && __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0) free(o); }
API pa_operation_state_t pa_operation_get_state(const pa_operation *o) { return o ? o->state : PA_OPERATION_DONE; }
static void op_done(pa_operation *o) { if (o) { o->state = PA_OPERATION_DONE; pa_operation_unref(o); } }

/* ---------------------------------------------------------------- context */

struct pa_context {
    int refs;
    pa_threaded_mainloop *m;
    pa_context_state_t state;
    pa_context_notify_cb_t state_cb; void *state_ud;
    pa_context_subscribe_cb_t sub_cb; void *sub_ud;
    pa_cvolume volume;                  /* the one sink input's (Soloist's stream) */
    struct pa_stream *stream;
};

static pa_threaded_mainloop *loop_of(pa_mainloop_api *api) { return api ? api->userdata : NULL; }

API pa_context *pa_context_new(pa_mainloop_api *api, const char *name)
{
    (void)name;
    pa_context *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->refs = 1; c->m = loop_of(api); c->state = PA_CONTEXT_UNCONNECTED;
    pa_cvolume_set(&c->volume, 2, PA_VOLUME_NORM);
    return c;
}
API void pa_context_unref(pa_context *c) { if (c && --c->refs == 0) free(c); }
API pa_context_state_t pa_context_get_state(const pa_context *c) { return c->state; }
API int pa_context_errno(const pa_context *c) { (void)c; return PA_OK; }
API void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *ud) { c->state_cb = cb; c->state_ud = ud; }
API void pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb, void *ud) { c->sub_cb = cb; c->sub_ud = ud; }

static void ctx_set(pa_context *c, pa_context_state_t s) { c->state = s; if (c->state_cb) c->state_cb(c, c->state_ud); }
static void ctx_connect_task(void *arg)
{
    pa_context *c = arg;
    ctx_set(c, PA_CONTEXT_AUTHORIZING); ctx_set(c, PA_CONTEXT_SETTING_NAME); ctx_set(c, PA_CONTEXT_READY);
}
/* CONNECTING at once, as libpulse: Soloist looks at the state right after the call and takes one that is not
 * PA_CONTEXT_IS_GOOD (UNCONNECTED is not) for a failure: "Unable to initialize AudioDriver, using dummy" (2026-11-19) */
API int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api)
{
    (void)server; (void)flags; (void)api;
    logf_("context connect");
    ctx_set(c, PA_CONTEXT_CONNECTING);
    post(c->m, ctx_connect_task, c);
    return 0;
}
static void ctx_term_task(void *arg) { ctx_set(arg, PA_CONTEXT_TERMINATED); }
API void pa_context_disconnect(pa_context *c) { post(c->m, ctx_term_task, c); }

struct succ { pa_operation *o; pa_context *c; pa_context_success_cb_t cb; void *ud; };
static void succ_task(void *arg)
{
    struct succ *s = arg;
    if (s->cb) s->cb(s->c, 1, s->ud);
    op_done(s->o); free(s);
}
static pa_operation *succ_post(pa_context *c, pa_context_success_cb_t cb, void *ud)
{
    struct succ *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = op_new(c->m); s->c = c; s->cb = cb; s->ud = ud;
    post(c->m, succ_task, s);
    return s->o;
}
API pa_operation *pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, pa_context_success_cb_t cb, void *ud) { (void)m; return succ_post(c, cb, ud); }

/* the sink input's volume: Soloist's own volume control (the Spotify app's slider) */
static int report_volume(void) { const char *v = getenv("HMPULSE_VOLUME"); return v && !strcmp(v, "report"); }
static void tell(const char *fmt, ...)
{
    const char *e = getenv("HMPULSE_EVENTS_FD");
    if (!e) return;
    char l[64]; va_list a; va_start(a, fmt); int n = vsnprintf(l, sizeof l, fmt, a); va_end(a);
    if (n > 0 && n < (int)sizeof l) { ssize_t w = write(atoi(e), l, (size_t)n); (void)w; }
}
API pa_operation *pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *v, pa_context_success_cb_t cb, void *ud)
{
    (void)idx;
    if (v && pa_cvolume_valid(v)) {
        c->volume = *v;
        pa_volume_t avg = pa_cvolume_avg(v);
        int pct = (int)((avg * 100ULL + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
        logf_("volume %d%%", pct);
        if (report_volume()) tell("volume %d\n", pct > 100 ? 100 : pct);
    }
    return succ_post(c, cb, ud);
}

struct sii { pa_operation *o; pa_context *c; pa_sink_input_info_cb_t cb; void *ud; uint32_t idx; };
static void sii_task(void *arg)
{
    struct sii *s = arg;
    pa_sink_input_info i; memset(&i, 0, sizeof i);
    i.index = s->idx; i.name = "Soloist"; i.owner_module = PA_INVALID_INDEX; i.client = 0; i.sink = 0;
    i.sample_spec.format = PA_SAMPLE_FLOAT32LE; i.sample_spec.rate = 44100; i.sample_spec.channels = 2;
    pa_channel_map_init_stereo(&i.channel_map);
    i.volume = s->c->volume; i.has_volume = 1; i.volume_writable = 1;
    i.driver = "hmpulse"; i.resample_method = "";
    if (s->cb) { s->cb(s->c, &i, 0, s->ud); s->cb(s->c, NULL, 1, s->ud); }
    op_done(s->o); free(s);
}
API pa_operation *pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *ud)
{
    struct sii *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = op_new(c->m); s->c = c; s->cb = cb; s->ud = ud; s->idx = idx;
    post(c->m, sii_task, s);
    return s->o;
}

/* ---------------------------------------------------------------- the stream */

struct pa_stream {
    int refs;
    pa_context *c;
    pa_sample_spec ss;
    pa_stream_state_t state;
    pa_stream_notify_cb_t state_cb, started_cb, underflow_cb; void *state_ud, *started_ud, *underflow_ud;
    pthread_mutex_t dl;                 /* what follows; the sender's and the application's */
    unsigned char *ring; size_t cap, head, fill;
    size_t tlength;                     /* the buffer Soloist may fill: what pa_stream_writable_size leaves it */
    int corked, started, underflowed, quit;
    int64_t written, played;            /* bytes in Soloist's format: the timing info's indexes */
    pa_timing_info timing;
    pthread_t sender; int sending;
};

static size_t frame_size(const pa_sample_spec *ss) { return pa_sample_size(ss) * ss->channels; }

API pa_stream *pa_stream_new_with_proplist(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map, pa_proplist *p)
{
    (void)name; (void)map; (void)p;
    if (!ss || (ss->format != PA_SAMPLE_FLOAT32LE && ss->format != PA_SAMPLE_S16LE) || !ss->channels || !ss->rate) {
        logf_("refused a stream of format %d", ss ? ss->format : -1);
        return NULL;
    }
    pa_stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->refs = 1; s->c = c; s->ss = *ss; s->state = PA_STREAM_UNCONNECTED;
    pthread_mutex_init(&s->dl, NULL);
    s->cap = pa_usec_to_bytes(4000000, ss);              /* 4 s: Soloist asked for 1 s (its "sink buffer") */
    s->ring = malloc(s->cap);
    if (!s->ring) { free(s); return NULL; }
    s->tlength = pa_usec_to_bytes(1000000, ss);
    c->stream = s;
    logf_("stream %s %u Hz %u ch", ss->format == PA_SAMPLE_FLOAT32LE ? "float32" : "s16", ss->rate, ss->channels);
    return s;
}
API pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map)
{
    return pa_stream_new_with_proplist(c, name, ss, map, NULL);
}
API pa_stream_state_t pa_stream_get_state(const pa_stream *s) { return s->state; }
API uint32_t pa_stream_get_index(const pa_stream *s) { (void)s; return 1; }
API int pa_stream_is_corked(const pa_stream *s) { return s->corked; }
API void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->state_cb = cb; s->state_ud = ud; }
API void pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->started_cb = cb; s->started_ud = ud; }
API void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->underflow_cb = cb; s->underflow_ud = ud; }

static void st_set(pa_stream *s, pa_stream_state_t st) { s->state = st; if (s->state_cb) s->state_cb(s, s->state_ud); }
static void started_task(void *arg) { pa_stream *s = arg; if (s->started_cb && s->state == PA_STREAM_READY) s->started_cb(s, s->started_ud); }
static void underflow_task(void *arg) { pa_stream *s = arg; if (s->underflow_cb && s->state == PA_STREAM_READY) s->underflow_cb(s, s->underflow_ud); }

/* ---- the output */

static int out_fd = -1;

static int out_open(void)
{
    const char *e;
    if (out_fd >= 0) return out_fd;
    if ((e = getenv("HMPULSE_FD"))) return out_fd = atoi(e);
    if ((e = getenv("HMPULSE_TCP"))) {
        char host[64]; const char *colon = strrchr(e, ':');
        if (!colon || colon - e >= (int)sizeof host) return -1;
        memcpy(host, e, (size_t)(colon - e)); host[colon - e] = 0;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(colon + 1)) };
        if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        int small = 16384; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);   /* little in flight: a pause stops soon */
        if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
        logf_("connected to %s", e);
        return out_fd = fd;
    }
    return -1;
}

static void out_write(const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        int fd = out_open();
        if (fd < 0) { usleep(200000); return; }        /* nobody to play it: dropped, as a sink with no device */
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            if (getenv("HMPULSE_FD")) { logf_("hassmic's pipe closed: ending"); _exit(0); }
            close(fd); out_fd = -1; return;            /* mixplay ended: the next chunk connects again */
        }
        b += w; n -= (size_t)w;
    }
}

/* frames from Soloist's format to s16le stereo, the volume applied unless hassmic owns it */
static size_t convert(const pa_stream *s, const unsigned char *in, size_t frames, int16_t *out)
{
    unsigned ch = s->ss.channels;
    float g = 1;
    if (!report_volume()) { pa_volume_t v = pa_cvolume_avg(&s->c->volume); g = (float)v / PA_VOLUME_NORM; g = g * g * g; }   /* cubic, as PulseAudio's software volume */
    for (size_t i = 0; i < frames; i++) {
        float l, r;
        if (s->ss.format == PA_SAMPLE_FLOAT32LE) {
            const float *f = (const float *)(in + i * ch * 4);
            l = f[0]; r = ch > 1 ? f[1] : f[0];
        } else {
            const int16_t *f = (const int16_t *)(in + i * ch * 2);
            l = f[0] / 32768.0f; r = (ch > 1 ? f[1] : f[0]) / 32768.0f;
        }
        l *= g; r *= g;
        l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
        out[2 * i] = (int16_t)lrintf(l * 32767); out[2 * i + 1] = (int16_t)lrintf(r * 32767);
    }
    return frames;
}

/* The sender: plays the ring out at the stream's rate, 10 ms at a time, on the clock.  Whatever reads the output
 * (hassmic's pipe, mixplay) blocks at playback speed as well; the clock keeps Soloist's buffer, not the pipe's, the
 * one that fills, so a pause or a skip is heard at once.  An empty ring while playing is an underflow */
static void *sender_thread(void *arg)
{
    pa_stream *s = arg;
    size_t fs = frame_size(&s->ss);
    int16_t *out = malloc((s->ss.rate / 50 + 1) * 4);
    unsigned char *chunk = malloc((s->ss.rate / 50 + 1) * fs);
    long long t0 = now_us(), sent = 0;              /* frames since t0 */
    if (!out || !chunk) return NULL;
    for (;;) {
        usleep(10000);
        pthread_mutex_lock(&s->dl);
        if (s->quit) { pthread_mutex_unlock(&s->dl); break; }
        if (s->corked) { t0 = now_us(); sent = 0; pthread_mutex_unlock(&s->dl); continue; }
        long long due = (now_us() - t0) * s->ss.rate / 1000000 - sent;
        if (due > (long long)s->ss.rate / 50) due = s->ss.rate / 50;
        size_t have = s->fill / fs, n = (size_t)(due > 0 ? due : 0);
        if (n > have) n = have;
        int fire_start = 0, fire_under = 0;
        if (!have) {
            if (s->started && !s->underflowed) { s->underflowed = 1; fire_under = 1; }
            t0 = now_us(); sent = 0;                    /* the clock starts again with the next data */
        }
        for (size_t k = 0; k < n * fs; k++) chunk[k] = s->ring[(s->head + k) % s->cap];
        s->head = (s->head + n * fs) % s->cap; s->fill -= n * fs; s->played += (int64_t)(n * fs);
        if (n && (!s->started || s->underflowed)) { fire_start = !s->started || s->underflowed; s->started = 1; s->underflowed = 0; }
        pthread_mutex_unlock(&s->dl);
        if (fire_start) post(s->c->m, started_task, s);
        if (fire_under) post(s->c->m, underflow_task, s);
        if (n) { convert(s, chunk, n, out); out_write(out, n * 4); sent += (long long)n; }
    }
    free(out); free(chunk);
    return NULL;
}

static void st_ready_task(void *arg) { st_set(arg, PA_STREAM_READY); }
API int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *attr, pa_stream_flags_t flags, const pa_cvolume *volume, pa_stream *sync)
{
    (void)dev; (void)sync;
    if (attr && attr->tlength != (uint32_t)-1 && attr->tlength) s->tlength = attr->tlength < s->cap ? attr->tlength : s->cap;
    if (flags & PA_STREAM_START_CORKED) s->corked = 1;
    if (volume && pa_cvolume_valid(volume)) s->c->volume = *volume;
    if (!pthread_create(&s->sender, NULL, sender_thread, s)) s->sending = 1;
    logf_("playback connected, buffer %zu bytes%s", s->tlength, s->corked ? ", corked" : "");
    st_set(s, PA_STREAM_CREATING);                     /* at once, as the context's CONNECTING */
    post(s->c->m, st_ready_task, s);
    return 0;
}

static void st_term_task(void *arg) { st_set(arg, PA_STREAM_TERMINATED); }
API int pa_stream_disconnect(pa_stream *s)
{
    pthread_mutex_lock(&s->dl); s->quit = 1; pthread_mutex_unlock(&s->dl);
    if (s->sending) { pthread_join(s->sender, NULL); s->sending = 0; }
    post(s->c->m, st_term_task, s);
    return 0;
}
API void pa_stream_unref(pa_stream *s)
{
    if (!s || --s->refs) return;
    if (s->sending) pa_stream_disconnect(s);
    if (s->c && s->c->stream == s) s->c->stream = NULL;
    free(s->ring); free(s);
}

API size_t pa_stream_writable_size(const pa_stream *s)
{
    pthread_mutex_lock((pthread_mutex_t *)&s->dl);
    size_t w = s->fill < s->tlength ? s->tlength - s->fill : 0;
    pthread_mutex_unlock((pthread_mutex_t *)&s->dl);
    return w - w % frame_size(&s->ss);
}

API int pa_stream_write(pa_stream *s, const void *data, size_t nbytes, pa_free_cb_t free_cb, int64_t offset, pa_seek_mode_t seek)
{
    (void)offset; (void)seek;                           /* Soloist writes on, relative */
    pthread_mutex_lock(&s->dl);
    size_t room = s->cap - s->fill, n = nbytes < room ? nbytes : room;
    for (size_t k = 0; k < n; k++) s->ring[(s->head + s->fill + k) % s->cap] = ((const unsigned char *)data)[k];
    s->fill += n; s->written += (int64_t)n;
    pthread_mutex_unlock(&s->dl);
    if (free_cb) free_cb((void *)data);
    return 0;
}

struct sop { pa_operation *o; pa_stream *s; pa_stream_success_cb_t cb; void *ud; };
static void sop_task(void *arg)
{
    struct sop *x = arg;
    if (x->cb) x->cb(x->s, 1, x->ud);
    op_done(x->o); free(x);
}
static pa_operation *sop_post(pa_stream *s, pa_stream_success_cb_t cb, void *ud)
{
    struct sop *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->o = op_new(s->c->m); x->s = s; x->cb = cb; x->ud = ud;
    post(s->c->m, sop_task, x);
    return x->o;
}
API pa_operation *pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *ud)
{
    pthread_mutex_lock(&s->dl); s->corked = !!b; if (b) s->started = 0; pthread_mutex_unlock(&s->dl);
    logf_(b ? "corked" : "uncorked");
    return sop_post(s, cb, ud);
}
API pa_operation *pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *ud)
{
    pthread_mutex_lock(&s->dl); s->played += (int64_t)s->fill; s->fill = 0; pthread_mutex_unlock(&s->dl);
    return sop_post(s, cb, ud);
}
API pa_operation *pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *ud) { return sop_post(s, cb, ud); }

/* What is in flight past the ring: the output's own buffer (the mixer's, hassmic's queue): a guess, as a remote
 * sink's latency is to libpulse */
#define SINK_USEC 60000
API const pa_timing_info *pa_stream_get_timing_info(pa_stream *s)
{
    pthread_mutex_lock(&s->dl);
    memset(&s->timing, 0, sizeof s->timing);
    gettimeofday(&s->timing.timestamp, NULL);
    s->timing.synchronized_clocks = 1;
    s->timing.sink_usec = SINK_USEC;
    s->timing.playing = s->started && !s->corked && s->fill > 0;
    s->timing.write_index = s->written;
    s->timing.read_index = s->played;
    s->timing.configured_sink_usec = SINK_USEC;
    pthread_mutex_unlock(&s->dl);
    return &s->timing;
}

/* ---------------------------------------------------------------- the small ones */

API pa_proplist *pa_proplist_new(void) { return calloc(1, 1); }
API void pa_proplist_free(pa_proplist *p) { free(p); }
API int pa_proplist_sets(pa_proplist *p, const char *k, const char *v) { (void)p; (void)k; (void)v; return 0; }
API const char *pa_strerror(int error) { return error ? "hmpulse: not supported" : "OK"; }

API size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec)
{
    size_t fs = frame_size(spec);
    return (size_t)(t * spec->rate / PA_USEC_PER_SEC) * fs;
}
API struct timeval *pa_gettimeofday(struct timeval *tv) { gettimeofday(tv, NULL); return tv; }
API pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b)
{
    long long d = ((long long)a->tv_sec - b->tv_sec) * 1000000LL + (a->tv_usec - b->tv_usec);
    return (pa_usec_t)(d < 0 ? -d : d);
}
API pa_volume_t pa_cvolume_avg(const pa_cvolume *a)
{
    uint64_t sum = 0;
    if (!a->channels) return PA_VOLUME_MUTED;
    for (unsigned i = 0; i < a->channels; i++) sum += a->values[i];
    return (pa_volume_t)(sum / a->channels);
}
API pa_cvolume *pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v)
{
    if (channels > PA_CHANNELS_MAX) channels = PA_CHANNELS_MAX;
    a->channels = (uint8_t)channels;
    for (unsigned i = 0; i < channels; i++) a->values[i] = v;
    return a;
}
/* used by the above, not by Soloist: local versions, so nothing outside this file is asked for */
API int pa_cvolume_valid(const pa_cvolume *v) { return v && v->channels > 0 && v->channels <= PA_CHANNELS_MAX; }
API size_t pa_sample_size(const pa_sample_spec *spec) { return spec->format == PA_SAMPLE_FLOAT32LE ? 4 : 2; }
API pa_channel_map *pa_channel_map_init_stereo(pa_channel_map *m)
{
    memset(m, 0, sizeof *m); m->channels = 2; m->map[0] = PA_CHANNEL_POSITION_LEFT; m->map[1] = PA_CHANNEL_POSITION_RIGHT;
    return m;
}

/* ---------------------------------------------------------------- the end of hassmic is Soloist's end

 * Soloist does not end when hassmic does (it reads no stdin); left running it would hold the zeroconf and WebSocket
 * ports from the next hassmic (hassmic-spotify did, 2026-11-19).  With HMPULSE_FD (hassmic runs it), a thread waits
 * on that pipe's write end: the reader gone, poll says POLLERR, and the process ends.  Preloaded, so from the start */
#include <poll.h>
static void *watch_thread(void *arg)
{
    int fd = (int)(long)arg;
    for (;;) {
        struct pollfd p = { fd, 0, 0 };                 /* no events asked: only POLLERR/POLLHUP come back */
        int r = poll(&p, 1, -1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || p.revents & (POLLERR | POLLHUP | POLLNVAL)) { logf_("hassmic is gone: ending"); _exit(0); }
    }
    return NULL;
}
__attribute__((constructor)) static void hmpulse_init(void)
{
    const char *e = getenv("HMPULSE_FD");
    pthread_t t;
    if (e && !pthread_create(&t, NULL, watch_thread, (void *)(long)atoi(e))) pthread_detach(t);
}

/* ---------------------------------------------------------------- bind(): zeroconf's port into the firewall's range */

API int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real)(int, const struct sockaddr *, socklen_t);
    static int taken[2];                /* once per address family: IPv4 and IPv6 each get the port */
    if (!real) real = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "bind");
    const char *p = getenv("HMPULSE_ZC_PORT");
    if (p && addr && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6) && !taken[addr->sa_family == AF_INET6]) {
        int type = 0; socklen_t tl = sizeof type;
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
        struct sockaddr_storage a; memcpy(&a, addr, len < sizeof a ? len : sizeof a);
        uint16_t *port = addr->sa_family == AF_INET ? &((struct sockaddr_in *)&a)->sin_port : &((struct sockaddr_in6 *)&a)->sin6_port;
        if (type == SOCK_STREAM && *port == 0) {
            *port = htons((uint16_t)atoi(p));
            if (real(fd, (struct sockaddr *)&a, len) == 0) { taken[addr->sa_family == AF_INET6] = 1; logf_("zeroconf on TCP %s", p); return 0; }
        }
    }
    return real(fd, addr, len);
}

/* ---------------------------------------------------------------- machine-id: a second speaker on one Echo */

/* Soloist's Spotify device id is /etc/machine-id's (else /var/lib/dbus/machine-id's): the same for every Soloist on
 * one Echo, so the group's speaker ("Alle Echos", ssgroup.c) beside the Echo's own was one device to the app.
 * HMPULSE_MACHINE_ID names a file of its own to read instead. */
static const char *mid_path(const char *p)
{
    const char *e = getenv("HMPULSE_MACHINE_ID");
    return e && p && (!strcmp(p, "/etc/machine-id") || !strcmp(p, "/var/lib/dbus/machine-id")) ? e : p;
}

/* Each under its exported name by asm label: with 64-bit file offsets (Ubuntu's armhf glibc) the headers turn open
 * into open64 and fopen into fopen64, and C names for both would be one symbol twice */
#define MID_OPEN(cname, sym)                                                                          \
    API int cname(const char *p, int flags, ...) __asm__(sym);                                         \
    API int cname(const char *p, int flags, ...)                                                       \
    {                                                                                                  \
        static int (*real)(const char *, int, ...);                                                    \
        if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, sym);                     \
        va_list ap; va_start(ap, flags); mode_t m = (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(ap, int) : 0; va_end(ap); \
        return real(mid_path(p), flags, m);                                                            \
    }
#define MID_OPENAT(cname, sym)                                                                        \
    API int cname(int d, const char *p, int flags, ...) __asm__(sym);                                  \
    API int cname(int d, const char *p, int flags, ...)                                                \
    {                                                                                                  \
        static int (*real)(int, const char *, int, ...);                                               \
        if (!real) real = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, sym);                \
        va_list ap; va_start(ap, flags); mode_t m = (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(ap, int) : 0; va_end(ap); \
        return real(d, mid_path(p), flags, m);                                                         \
    }
#define MID_FOPEN(cname, sym)                                                                         \
    API FILE *cname(const char *p, const char *mode) __asm__(sym);                                     \
    API FILE *cname(const char *p, const char *mode)                                                   \
    {                                                                                                  \
        static FILE *(*real)(const char *, const char *);                                              \
        if (!real) real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, sym);               \
        return real(mid_path(p), mode);                                                                \
    }
MID_OPEN(hm_open, "open")
MID_OPEN(hm_open64, "open64")
MID_OPENAT(hm_openat, "openat")
MID_OPENAT(hm_openat64, "openat64")
MID_FOPEN(hm_fopen, "fopen")
MID_FOPEN(hm_fopen64, "fopen64")
