/* src/soloist/hmpulse.c (Soloist's libpulse on the Echo) the way Soloist uses it: a threaded mainloop, a context and a
 * float32 stereo stream at 44.1 kHz, written as pa_stream_writable_size allows, its timing read, its volume set.
 * Checked: the context and the stream come up READY, the PCM arrives on HMPULSE_FD as s16le stereo at about real-time
 * pace (paced by the clock, not the pipe), the started callback fires, a cork stops it, the volume is reported
 * ("volume 50") and not applied with HMPULSE_VOLUME=report, and bind() gives a TCP socket on port 0 HMPULSE_ZC_PORT. */
#include <pulse/pulseaudio.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static pa_threaded_mainloop *m;
static int started, vol_ok;
static void ctx_cb(pa_context *c, void *u) { (void)c; (void)u; pa_threaded_mainloop_signal(m, 0); }
static void st_cb(pa_stream *s, void *u) { (void)s; (void)u; pa_threaded_mainloop_signal(m, 0); }
static void started_cb(pa_stream *s, void *u) { (void)s; (void)u; started++; }
static void vol_cb(pa_context *c, int ok, void *u) { (void)c; (void)u; vol_ok = ok; }

int main(void)
{
    int pcm[2], ev[2];
    if (pipe(pcm) || pipe(ev)) return 1;
    fcntl(pcm[0], F_SETFL, O_NONBLOCK); fcntl(ev[0], F_SETFL, O_NONBLOCK);
    char b[16];
    snprintf(b, sizeof b, "%d", pcm[1]); setenv("HMPULSE_FD", b, 1);
    snprintf(b, sizeof b, "%d", ev[1]); setenv("HMPULSE_EVENTS_FD", b, 1);
    setenv("HMPULSE_VOLUME", "report", 1);
    setenv("HMPULSE_ZC_PORT", "28977", 1);

    m = pa_threaded_mainloop_new();
    pa_context *c = pa_context_new(pa_threaded_mainloop_get_api(m), "Soloist");
    pa_context_set_state_callback(c, ctx_cb, NULL);
    pa_threaded_mainloop_lock(m);
    pa_threaded_mainloop_start(m);
    pa_context_connect(c, NULL, 0, NULL);
    check(PA_CONTEXT_IS_GOOD(pa_context_get_state(c)), "right after connect: a good state (CONNECTING), as Soloist checks");
    while (pa_context_get_state(c) != PA_CONTEXT_READY) pa_threaded_mainloop_wait(m);
    check(1, "context READY");
    pa_sample_spec ss = { PA_SAMPLE_FLOAT32LE, 44100, 2 };
    pa_proplist *p = pa_proplist_new(); pa_proplist_sets(p, "media.role", "music");
    pa_stream *s = pa_stream_new_with_proplist(c, "Spotify", &ss, NULL, p);
    pa_proplist_free(p);
    pa_stream_set_state_callback(s, st_cb, NULL);
    pa_stream_set_started_callback(s, started_cb, NULL);
    pa_buffer_attr attr = { (uint32_t)-1, (uint32_t)pa_usec_to_bytes(500000, &ss), (uint32_t)-1, (uint32_t)-1, (uint32_t)-1 };
    pa_stream_connect_playback(s, NULL, &attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
    check(PA_STREAM_IS_GOOD(pa_stream_get_state(s)), "right after connect_playback: a good state (CREATING)");
    while (pa_stream_get_state(s) != PA_STREAM_READY) pa_threaded_mainloop_wait(m);
    check(1, "stream READY");
    check(pa_stream_writable_size(s) == pa_usec_to_bytes(500000, &ss), "writable: the buffer asked for (500 ms)");
    pa_threaded_mainloop_unlock(m);

    /* 2 s of a 440 Hz tone at -6 dBFS, written as room comes */
    long long frames = 0, total = 88200; size_t got = 0; double t0 = now(); int paced_checked = 0;
    while (now() - t0 < 2.6) {
        pa_threaded_mainloop_lock(m);
        size_t w = pa_stream_writable_size(s) / 8;
        if (frames < total && w) {
            if (w > (size_t)(total - frames)) w = (size_t)(total - frames);
            float *buf = malloc(w * 8);
            for (size_t i = 0; i < w; i++) buf[2 * i] = buf[2 * i + 1] = 0.5f * (float)sin(2 * M_PI * 440 * (double)(frames + (long long)i) / 44100);
            pa_stream_write(s, buf, w * 8, free, 0, PA_SEEK_RELATIVE);
            frames += (long long)w;
        }
        pa_threaded_mainloop_unlock(m);
        char r[65536]; ssize_t n;
        while ((n = read(pcm[0], r, sizeof r)) > 0) {
            if (!got && n >= 400) {                  /* a sample past the fade: full scale, not the volume's */
                int16_t v[100]; memcpy(v, r, sizeof v); int peak = 0;
                for (int i = 0; i < 100; i++) if (abs(v[i]) > peak) peak = abs(v[i]);
                check(peak > 1000 && peak <= 16384, "s16le out, at the tone's level");
            }
            got += (size_t)n;
        }
        usleep(5000);
        if (now() - t0 > 1.0 && !paced_checked) {
            size_t at1 = got; paced_checked = 1;
            check(at1 > 44100 * 4 * 0.85 && at1 < 44100 * 4 * 1.15, "paced: about 1 s of s16 stereo after 1 s (the clock, not the pipe)");
        }
    }
    check(got == 88200 * 4, "all of it played, as s16le stereo (4 bytes a frame)");
    check(started >= 1, "the started callback fired");

    pa_threaded_mainloop_lock(m);
    const pa_timing_info *ti = pa_stream_get_timing_info(s);
    check(ti && ti->write_index == 88200 * 8 && ti->read_index == 88200 * 8, "timing: written and read indexes in Soloist's bytes");
    pa_cvolume v; pa_cvolume_set(&v, 2, PA_VOLUME_NORM / 2);
    pa_operation *o = pa_context_set_sink_input_volume(c, pa_stream_get_index(s), &v, vol_cb, NULL);
    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) pa_threaded_mainloop_wait(m);
    pa_operation_unref(o);
    check(vol_ok, "volume set: the operation done, its callback told success");
    o = pa_stream_cork(s, 1, NULL, NULL);
    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) pa_threaded_mainloop_wait(m);
    pa_operation_unref(o);
    check(pa_stream_is_corked(s), "corked");
    pa_threaded_mainloop_unlock(m);
    char e[64] = ""; ssize_t n = read(ev[0], e, sizeof e - 1); if (n > 0) e[n] = 0;
    check(!strcmp(e, "volume 50\n"), "the volume reported to hassmic, not applied");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    bind(fd, (struct sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a; getsockname(fd, (struct sockaddr *)&a, &l);
    check(ntohs(a.sin_port) == 28977, "zeroconf's listener on port 0: HMPULSE_ZC_PORT instead");
    int fd2 = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a2 = { .sin_family = AF_INET };
    bind(fd2, (struct sockaddr *)&a2, sizeof a2); l = sizeof a2; getsockname(fd2, (struct sockaddr *)&a2, &l);
    check(ntohs(a2.sin_port) != 28977 && ntohs(a2.sin_port) != 0, "a second one: the kernel's port, as before");

    pa_stream_disconnect(s); pa_stream_unref(s);
    pa_context_disconnect(c); pa_context_unref(c);
    pa_threaded_mainloop_stop(m); pa_threaded_mainloop_free(m);
    printf(fails ? "%d FAILED\n" : "hmpulse: all passed\n", fails);
    return fails != 0;
}
