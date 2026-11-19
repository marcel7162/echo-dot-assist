/* Spotify on every Echo (ssgroup.c): a second Spotify speaker on this Echo ("Alle Echos") whose music this Echo serves
 * as a Sendspin server to the Echos of its arbitration network, itself included; they play it in step through their
 * Sendspin players (sendspin.c). */
#ifndef SSGROUP_H
#define SSGROUP_H
#include <stddef.h>
#include <stdint.h>

void ssgroup_start(int sendspin_port);  /* main(): the manager thread; the players' port (ours: core_sendspin_port) */
int  ssgroup_enable(int set);           /* setting spotify_group (-1 reads); any thread */
/* The group's speaker's PCM at 44.1 kHz stereo s16 as it comes; idle after it stopped (pause, between sessions) */
void ssgroup_pcm(const int16_t *pcm, size_t frames);
void ssgroup_idle(void);
void ssgroup_volume(int v);             /* the Spotify app's volume: every Echo of the group follows */

/* The settings page (spotify.c's status JSON, web.c): its state and members; the speaker's name in the app ("" =
 * "Alle Echos"), its logout; an Echo of the network in or out of the group (node name).  Any thread */
size_t ssgroup_status_json(char *out, size_t cap);
int  ssgroup_set_name(const char *name);
void ssgroup_logout(void);
int  ssgroup_member(const char *node, int in);

/* sendspin.c: the key a member's player takes from this network's servers (KKpsk2's PSK, category "lt"), derived from
 * the network key and the server's id; 0 = there, -1 = no network.  ssgroup_is_self: the server is this Echo's own */
int  ssgroup_psk(const char *server_id, uint8_t out[32]);
int  ssgroup_is_self(const char *server_id);
#endif
