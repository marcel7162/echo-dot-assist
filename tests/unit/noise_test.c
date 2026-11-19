/* Responder driven over stdin/stdout in hex lines by noise_ref.py (Python noiseprotocol = what aiosendspin uses). */
#include <stdio.h>
#include <string.h>
#include "noise.h"
static size_t unhex(const char *s, uint8_t *o) { size_t n = 0; unsigned v; while (sscanf(s + 2 * n, "%2x", &v) == 1) o[n++] = v; return n; }
static void puthex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); fflush(stdout); }
/* KKpsk2 initiator (ssgroup.c) against our responder, which noise_ref.py holds to the reference: the same handshake
 * hash on both sides, transport both ways, a wrong PSK refused */
static int kk_loop(void)
{
    uint8_t ip[32], iP[32], rp[32], rP[32], psk[32], bad[32], m1[128], m2[128], pl[128], ct[64], pt[64];
    struct noise_hs i, r, i2, r2; struct noise_cs itx, irx, rtx, rrx, x1, x2;
    noise_keypair(ip, iP); noise_keypair(rp, rP); memset(psk, 7, 32); memset(bad, 8, 32);
    noise_kk_initiator_init(&i, "pro", 3, ip, iP, rP); noise_kk_responder_init(&r, "pro", 3, rp, rP, iP);
    size_t n1 = noise_kk_write_msg1(&i, "{\"psk_id\":\"x\"}", 14, m1);
    if (noise_kk_read_msg1(&r, m1, n1, pl) != 14 || memcmp(pl, "{\"psk_id\":\"x\"}", 14)) { puts("kk-loop: msg1"); return 1; }
    size_t n2 = noise_kk_write_msg2(&r, psk, "{}", 2, m2, &rtx, &rrx);
    if (noise_kk_read_msg2(&i, psk, m2, n2, pl, &itx, &irx) != 2 || memcmp(i.hash_out, r.hash_out, 32)) { puts("kk-loop: msg2"); return 1; }
    size_t n = noise_encrypt(&itx, "to client", 9, ct);
    if (noise_decrypt(&rrx, ct, n, pt) != 9 || memcmp(pt, "to client", 9)) { puts("kk-loop: initiator -> responder"); return 1; }
    n = noise_encrypt(&rtx, "to server", 9, ct);
    if (noise_decrypt(&irx, ct, n, pt) != 9 || memcmp(pt, "to server", 9)) { puts("kk-loop: responder -> initiator"); return 1; }
    noise_kk_initiator_init(&i2, "pro", 3, ip, iP, rP); noise_kk_responder_init(&r2, "pro", 3, rp, rP, iP);
    n1 = noise_kk_write_msg1(&i2, "{}", 2, m1); noise_kk_read_msg1(&r2, m1, n1, pl);
    n2 = noise_kk_write_msg2(&r2, psk, "{}", 2, m2, &x1, &x2);
    if (noise_kk_read_msg2(&i2, bad, m2, n2, pl, &x1, &x2) != -1) { puts("kk-loop: a wrong PSK taken"); return 1; }
    puts("kk-loop: ok");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "kk-loop")) return kk_loop();
    static char line[8192]; static uint8_t a[4096], b[4096], prologue[256], spriv[32], spub[32], ipub[32], psk[32];
    struct noise_hs hs; struct noise_cs tx, rx; size_t pl;
    fgets(line, sizeof line, stdin); pl = unhex(line, prologue);
    fgets(line, sizeof line, stdin); unhex(line, spriv);
    fgets(line, sizeof line, stdin); unhex(line, ipub);
    fgets(line, sizeof line, stdin); unhex(line, psk);
    { extern void crypto_x25519_public_key(uint8_t *, const uint8_t *); crypto_x25519_public_key(spub, spriv); }
    noise_kk_responder_init(&hs, prologue, pl, spriv, spub, ipub);
    fgets(line, sizeof line, stdin); size_t n = unhex(line, a);
    long p = noise_kk_read_msg1(&hs, a, n, b);
    if (p < 0) { puts("ERR msg1"); return 1; }
    puthex(b, p);                                           /* payload of message 1 */
    n = noise_kk_write_msg2(&hs, psk, "{}", 2, a, &tx, &rx); puthex(a, n);
    puthex(hs.hash_out, 32);
    for (int i = 0; i < 3; i++) {                           /* transport: decrypt, reply with the text reversed */
        fgets(line, sizeof line, stdin); n = unhex(line, a); p = noise_decrypt(&rx, a, n, b);
        if (p < 0) { puts("ERR transport"); return 1; }
        for (long j = 0; j < p / 2; j++) { uint8_t t = b[j]; b[j] = b[p - 1 - j]; b[p - 1 - j] = t; }
        n = noise_encrypt(&tx, b, p, a); puthex(a, n);
    }
    return 0;
}
