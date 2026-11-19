/* Spoken commands for Spotify, in German and English: what Home Assistant's speech to text wrote, read word by word.
 * Only sentences that name Spotify start or search anything ("Spiel Spotify", "Spiel Queen auf Spotify", "Play the
 * album Thriller on Spotify"); "nächstes Lied", "pause" without the name are taken only while Spotify plays on this
 * Echo, so Home Assistant keeps answering them for its own players.  Everything else goes to Home Assistant as before.
 * The words of a search are kept as said (case, umlauts): Spotify's search reads them. */
#include "spotcmd.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAXW 24

/* lower case for comparing: ASCII and the German capitals (speech to text capitalises the first word: "Spiel") */
static void lower(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 2 < cap; p++) {
        if (p[0] == 0xC3 && (p[1] == 0x84 || p[1] == 0x96 || p[1] == 0x9C)) { out[n++] = (char)0xC3; out[n++] = (char)(p[1] + 0x20); p++; }
        else out[n++] = (char)tolower(*p);
    }
    out[n] = 0;
}

static int in(const char *w, const char *const *list)
{
    for (; *list; list++) if (!strcmp(w, *list)) return 1;
    return 0;
}

static const char *const verbs[] = { "spiel", "spiele", "spielen", "spielt", "play", "starte", "start", "starten", "mach",
                                     "mache", "öffne", "open", "put", "hör", "höre", "hören", "listen", "lass", "leg", 0 };
static const char *const fillers[] = { "mal", "bitte", "doch", "mir", "uns", "please", "me", "us", "some", "jetzt", "now",
                                       "to", "etwas", "something", "was", 0 };
static const char *const preps[] = { "auf", "bei", "über", "ueber", "uber", "von", "mit", "in", "on", "from", "with", "via", "using", 0 };
static const char *const tails[] = { "ab", "an", "spielen", "abspielen", "starten", "laufen", "bitte", "please", "now", "jetzt", "aus", 0 };
static const char *const articles[] = { "die", "der", "das", "den", "dem", "the", "a", "an", "ein", "eine", "einen", "meine", "my", 0 };

static const char *const k_playlist[] = { "playlist", "playliste", "wiedergabeliste", 0 };
static const char *const k_album[] = { "album", 0 };
static const char *const k_artist[] = { "künstler", "kuenstler", "kunstler", "band", "interpret", "interpreten", "artist", "sänger", "sängerin", 0 };
static const char *const k_track[] = { "song", "lied", "titel", "track", 0 };

/* the rest of the sentence, as words (lower-cased beside them) */
struct words { int n; char w[MAXW][64], l[MAXW][64]; };

static void drop(struct words *s, int i)
{
    memmove(s->w[i], s->w[i + 1], (size_t)(s->n - i - 1) * sizeof s->w[0]);
    memmove(s->l[i], s->l[i + 1], (size_t)(s->n - i - 1) * sizeof s->l[0]);
    s->n--;
}

static int is(const struct words *s, int i, const char *w) { return i >= 0 && i < s->n && !strcmp(s->l[i], w); }

/* the words in s[from..] alone are exactly one of these phrases */
static int only(const struct words *s, const char *const *phrases)
{
    char joined[256] = ""; size_t n = 0;
    for (int i = 0; i < s->n; i++) n += (size_t)snprintf(joined + n, n < sizeof joined ? sizeof joined - n : 0, "%s%s", i ? " " : "", s->l[i]);
    return in(joined, phrases);
}

static const char *const say_next[] = { "nächstes lied", "nächster song", "nächster titel", "nächstes", "nächster", "naechstes lied",
                                        "next", "next song", "next track", "skip", "überspringen", "weiter springen", "song überspringen",
                                        "lied überspringen", "überspringe das lied", "skip this song", "skip song", 0 };
static const char *const say_prev[] = { "vorheriges lied", "voriges lied", "vorheriger song", "vorheriger titel", "letztes lied",
                                        "zurück", "previous", "previous song", "previous track", "go back", "last song", 0 };
static const char *const say_pause[] = { "pause", "pausieren", "pausiere", "anhalten", "halt an", "pausier", 0 };
static const char *const say_resume[] = { "weiter", "fortsetzen", "weiterspielen", "setz fort", "resume", "continue", "unpause", 0 };
static const char *const say_liked[] = { "lieblingssongs", "lieblingslieder", "lieblingsmusik", "lieblingstitel", "songs", "lieder",
                                         "musik", "lieblings songs", "lieblings lieder", "gespeicherten songs", "liked songs",
                                         "favorites", "favourites", "favorite songs", "favourite songs", "music", "saved songs", 0 };
static const char *const say_any[] = { "musik", "music", "irgendwas", "anything", "something", "etwas", "was", "spotify", 0 };

int spotcmd_parse(const char *text, const char *wake, int playing, char *out, size_t cap)
{
    struct words s; s.n = 0;
    char buf[512]; snprintf(buf, sizeof buf, "%s", text);
    for (char *t = strtok(buf, " \t"); t && s.n < MAXW; t = strtok(NULL, " \t")) {
        size_t k = strlen(t);
        while (k && strchr(".,!?;:\"'«»“”„", t[k - 1])) t[--k] = 0;     /* ASCII punctuation at the end; the quotes' bytes too */
        while (*t && strchr(",\"'¡¿«„“", *t)) t++;
        if (!*t || strlen(t) >= sizeof s.w[0]) continue;
        snprintf(s.w[s.n], sizeof s.w[0], "%s", t); lower(t, s.l[s.n], sizeof s.l[0]); s.n++;
    }
    if (wake && *wake) {                                            /* "Alexa, spiel Spotify": the wake word sent too */
        char lw[64]; lower(wake, lw, sizeof lw);
        if (s.n && !strcmp(s.l[0], lw)) drop(&s, 0);
    }
    int at = -1;
    for (int i = 0; i < s.n; i++) if (!strcmp(s.l[i], "spotify") || !strcmp(s.l[i], "spotifys") || !strcmp(s.l[i], "spotify's")) at = i;
    if (at < 0) {
        /* without the name: a few words to the music that plays here, nothing more */
        if (!playing) return 0;
        const char *c = only(&s, say_next) ? "next" : only(&s, say_prev) ? "prev" : only(&s, say_pause) ? "pause" : NULL;
        if (!c) return 0;
        snprintf(out, cap, "%s", c);
        return 1;
    }
    drop(&s, at);
    if (in(s.l[at - 1 < 0 ? 0 : at - 1], preps) && at > 0) drop(&s, at - 1);       /* "auf Spotify", "on Spotify" */
    /* the verb and fillers in front, the separable verb's part and fillers at the end ("spiel ... ab") */
    while (s.n && (in(s.l[0], verbs) || in(s.l[0], fillers) || in(s.l[0], preps))) drop(&s, 0);
    while (s.n && in(s.l[s.n - 1], tails)) drop(&s, s.n - 1);
    if (!s.n || only(&s, say_any)) { snprintf(out, cap, "play"); return 1; }
    if (only(&s, say_next)) { snprintf(out, cap, "next"); return 1; }
    if (only(&s, say_prev)) { snprintf(out, cap, "prev"); return 1; }
    if (only(&s, say_pause)) { snprintf(out, cap, "pause"); return 1; }
    if (only(&s, say_resume)) { snprintf(out, cap, "play"); return 1; }
    /* "meine Lieblingssongs", "my liked songs" */
    if (is(&s, 0, "meine") || is(&s, 0, "my") || is(&s, 0, "meinen") || is(&s, 0, "meiner")) {
        struct words r = s; drop(&r, 0);
        if (only(&r, say_liked)) { snprintf(out, cap, "liked"); return 1; }
    }
    if (only(&s, say_liked) && (is(&s, 0, "lieblingssongs") || is(&s, 0, "lieblingslieder") || is(&s, 0, "lieblingsmusik") || is(&s, 0, "liked"))) {
        snprintf(out, cap, "liked"); return 1;
    }
    /* a kind said: "die Playlist X", "das Album X", "den Künstler X", "Musik von X", "Songs von X", "the song X" */
    const char *kind = "any";
    if (s.n > 1 && in(s.l[0], articles)) {
        if (in(s.l[1], k_playlist) || in(s.l[1], k_album) || in(s.l[1], k_artist) || in(s.l[1], k_track)) drop(&s, 0);
    }
    if (s.n > 1) {
        if (in(s.l[0], k_playlist)) { kind = "playlist"; drop(&s, 0); }
        else if (in(s.l[0], k_album)) { kind = "album"; drop(&s, 0); }
        else if (in(s.l[0], k_artist)) { kind = "artist"; drop(&s, 0); }
        else if (in(s.l[0], k_track)) { kind = "track"; drop(&s, 0); }
        else if (s.n > 2 && (in(s.l[0], (const char *const[]){ "musik", "music", "songs", "lieder", "etwas", "something", "titel", "hits", 0 }))
                 && in(s.l[1], (const char *const[]){ "von", "by", "of", 0 })) { kind = "artist"; drop(&s, 0); drop(&s, 0); }
    }
    while (s.n && in(s.l[s.n - 1], tails)) drop(&s, s.n - 1);
    if (!s.n) { snprintf(out, cap, "play"); return 1; }
    size_t n = (size_t)snprintf(out, cap, "search %s", kind);
    for (int i = 0; i < s.n && n < cap; i++) {
        /* "Thriller von Michael Jackson": the "von" goes, Spotify reads title and artist together */
        if (strcmp(kind, "artist") && i > 0 && i < s.n - 1 && (is(&s, i, "von") || is(&s, i, "by"))) continue;
        n += (size_t)snprintf(out + n, cap - n, " %s", s.w[i]);
    }
    return n < cap;
}
