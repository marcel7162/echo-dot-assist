/* Spotify on every Echo (ssgroup.c): the Echo that runs Spotify serves its music as a Sendspin server to the Echos of
 * its arbitration network, itself included, and they play it in step through their Sendspin players (sendspin.c). */
#ifndef SSGROUP_H
#define SSGROUP_H
#include <stddef.h>
#include <stdint.h>

void ssgroup_start(int sendspin_port);  /* main(): the manager thread; the players' port (ours: core_sendspin_port) */
int  ssgroup_enable(int set);           /* setting spotify_group (-1 reads); any thread */
int  ssgroup_routing(void);             /* 1: Spotify's PCM goes to the group, not to this Echo's own stream */

/* spotify.c's audio thread: PCM at 44.1 kHz stereo s16 as it comes; idle after it stopped (pause, between sessions) */
void ssgroup_pcm(const int16_t *pcm, size_t frames);
void ssgroup_idle(void);
void ssgroup_volume(int v);             /* the Spotify app's volume: every Echo of the group follows */

/* sendspin.c: the key a member's player takes from this network's servers (KKpsk2's PSK, category "lt"), derived from
 * the network key and the server's id; 0 = there, -1 = no network.  ssgroup_is_self: the server is this Echo's own */
int  ssgroup_psk(const char *server_id, uint8_t out[32]);
int  ssgroup_is_self(const char *server_id);
#endif
