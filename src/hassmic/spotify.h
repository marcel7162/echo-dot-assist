/* Spotify Connect (spotify.c): hassmic-spotify (src/spotify/, librespot) run beside hassmic while the setting is on,
 * its PCM played on a stream of its own, its volume the Echo's music volume, spoken commands ("Spiel Spotify",
 * "Spiel Queen auf Spotify") taken before Home Assistant answers them. */
#ifndef SPOTIFY_H
#define SPOTIFY_H
#include <stddef.h>

#define SPOTIFY_PORT 28933              /* zeroconf (TCP): the firewall's inbound range, beside 28931 (page) and 28932 (Drop In) */

int  spotify_present(void);             /* the daemon is there to run (next to hassmic, or $HASSMIC_SPOTIFY): any thread */
void spotify_start(void);               /* main(): the supervisor thread, after settings_load */

/* Settings (settings.c; -1 reads).  A change of bitrate or normalisation restarts the daemon */
int  spotify_enable(int set);
int  spotify_bitrate(int set);          /* index into spotify_bitrates */
int  spotify_normalise(int set);
int  spotify_voice(int set);            /* spoken commands */
extern const char *const spotify_bitrates[3];   /* "96", "160", "320" (kbit/s) */

/* Lock held (core_lock): the core's hooks */
void spotify_volume_changed(int v);     /* the Echo's music volume moved (keys, Home Assistant): the app's slider follows */
void spotify_pause(void);               /* another music source started (core_music): newest wins */
int  spotify_stop(void);                /* "<wake word>, stop": 1 if it paused Spotify */
int  spotify_button(void);              /* action button: pause, or play again what the button paused; 1 = used */
int  spotify_skip(int dir);             /* action button twice (+1) / three times (-1); 1 = sent */
/* speech to text's words: 1 if they were a command for Spotify (sent, or refused with the error sound); the run is the
 * caller's to cancel.  wake: the wake phrase ("Alexa"), so "Alexa, spiel Spotify" with the word sent too reads right */
int  spotify_command(const char *text, const char *wake);

/* The settings page (web.c): its state as JSON, and the speaker's own name (state/spotify/name; "" = the Echo's name,
 * core_name) and logout.  Any thread */
size_t spotify_status_json(char *out, size_t cap);
int  spotify_set_name(const char *name);        /* 0, -1: too long or not text */
void spotify_logout(void);
#endif
