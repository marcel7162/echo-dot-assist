/* Spoken commands for Spotify (spotify.c): speech to text's words -> a line for hassmic-spotify (src/spotify/src/main.rs).
 * Pure, for tests/unit/spotcmd_test.c. */
#ifndef SPOTCMD_H
#define SPOTCMD_H
#include <stddef.h>

/* text: the transcript, the wake word already off or named in wake ("Alexa", "" none).  playing: Spotify plays here now,
 * so "nächstes Lied", "pause" without the word Spotify are for it.  1 and the command in out ("play", "liked", "pause",
 * "resume", "next", "prev", "search <any|track|artist|album|playlist> <words>"), 0: not one for Spotify */
int spotcmd_parse(const char *text, const char *wake, int playing, char *out, size_t cap);
#endif
