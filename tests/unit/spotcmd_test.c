/* spotcmd.c: sentences as Home Assistant's speech to text writes them (capitalised first word, a full stop, the wake
 * word sometimes in front) -> the line for hassmic-spotify, or nothing (Home Assistant answers it as before). */
#include "spotcmd.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void t(const char *text, int playing, const char *want)
{
    char out[256] = "";
    int got = spotcmd_parse(text, "Alexa", playing, out, sizeof out);
    int ok = want ? got && !strcmp(out, want) : !got;
    printf("%s %-46s -> %s%s%s\n", ok ? "ok  " : "FAIL", text, got ? out : "(Home Assistant)", ok ? "" : ", wanted ", ok ? "" : want ? want : "(Home Assistant)");
    if (!ok) fails++;
}

int main(void)
{
    /* resume or start */
    t("Spiel Spotify.", 0, "play");
    t("Spiele Spotify", 0, "play");
    t("Alexa, spiel Spotify.", 0, "play");
    t("Spotify spielen", 0, "play");
    t("Spotify", 0, "play");
    t("Spiel Musik auf Spotify", 0, "play");
    t("Spiel mal was auf Spotify", 0, "play");
    t("Play Spotify.", 0, "play");
    t("Play some music on Spotify", 0, "play");
    t("Spotify weiter", 0, "play");
    t("Starte Spotify", 0, "play");
    /* liked songs */
    t("Spiel meine Lieblingssongs auf Spotify", 0, "liked");
    t("Spiel meine Lieblingslieder auf Spotify.", 0, "liked");
    t("Play my liked songs on Spotify", 0, "liked");
    t("Spiel meine Musik auf Spotify", 0, "liked");
    /* searches, the words as said */
    t("Spiel Queen auf Spotify.", 0, "search any Queen");
    t("Spiel Bohemian Rhapsody auf Spotify", 0, "search any Bohemian Rhapsody");
    t("Spiel Thriller von Michael Jackson auf Spotify", 0, "search any Thriller Michael Jackson");
    t("Spiel auf Spotify die Ärzte", 0, "search any die Ärzte");
    t("Spiel die Playlist Chillout Lounge auf Spotify", 0, "search playlist Chillout Lounge");
    t("Spiel das Album Abbey Road auf Spotify ab", 0, "search album Abbey Road");
    t("Spiel den Künstler Herbert Grönemeyer auf Spotify", 0, "search artist Herbert Grönemeyer");
    t("Spiel Musik von AC/DC auf Spotify", 0, "search artist AC/DC");
    t("Spiel das Lied Atemlos auf Spotify", 0, "search track Atemlos");
    t("Play the album Thriller on Spotify", 0, "search album Thriller");
    t("Play Bohemian Rhapsody by Queen on Spotify", 0, "search any Bohemian Rhapsody Queen");
    t("Play songs by Taylor Swift on Spotify.", 0, "search artist Taylor Swift");
    t("Spotify spiel Wincent Weiss", 0, "search any Wincent Weiss");
    /* controls named with Spotify */
    t("Spotify pausieren", 1, "pause");
    t("Nächstes Lied auf Spotify", 1, "next");
    /* without the name: only while Spotify plays here */
    t("Nächstes Lied.", 1, "next");
    t("Nächstes Lied.", 0, NULL);
    t("Skip", 1, "next");
    t("Vorheriges Lied", 1, "prev");
    t("Pause", 1, "pause");
    t("Pause", 0, NULL);
    /* not Spotify's */
    t("Wie spät ist es?", 1, NULL);
    t("Spiel Musik im Wohnzimmer", 0, NULL);
    t("Schalte das Licht ein", 1, NULL);
    t("", 0, NULL);
    printf(fails ? "%d FAILED\n" : "spotcmd: all passed\n", fails);
    return fails != 0;
}
