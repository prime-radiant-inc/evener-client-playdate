#include "chirp.h"
#include "pairing_protocol.h"
#include "qrcodegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void randombytes(unsigned char *bytes, unsigned long long count) { (void)bytes; (void)count; abort(); }
static void unhex(uint8_t *out, const char *text, unsigned length) {
    if (strlen(text) != length * 2) exit(2);
    for (unsigned i = 0; i < length; i++) {
        unsigned n; if (sscanf(text + i * 2, "%2x", &n) != 1) exit(2);
        out[i] = n;
    }
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    uint8_t secret[32], session[8];
    unhex(secret, argv[2], 32); unhex(session, argv[3], 8);
    if (!strcmp(argv[1], "qr") || !strcmp(argv[1], "qr-image")) {
        char qr[PAIRING_QR_LENGTH]; pairing_key(secret, session, qr);
        if (!strcmp(argv[1], "qr")) { puts(qr); return 0; }
        uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(12)], code[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
        if (!qrcodegen_encodeText(qr, scratch, code, qrcodegen_Ecc_MEDIUM, 1, 12, qrcodegen_Mask_AUTO, true)) return 1;
        int size = qrcodegen_getSize(code);
        printf("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"400\" viewBox=\"0 0 %d %d\"><rect width=\"100%%\" height=\"100%%\" fill=\"white\"/>", size + 8, size + 8);
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) if (qrcodegen_getModule(code, x, y))
            printf("<rect x=\"%d\" y=\"%d\" width=\"1\" height=\"1\"/>", x + 4, y + 4);
        puts("</svg>"); return 0;
    }
    static ChirpDecoder decoder;
    chirp_reset(&decoder);
    if (!strcmp(argv[1], "open") || !strcmp(argv[1], "open-invitation")) {
        decoder.length = (unsigned)fread(decoder.bytes, 1, sizeof(decoder.bytes), stdin);
    } else {
        int16_t samples[997]; size_t n;
        while ((n = fread(samples, sizeof(*samples), 997, stdin))) chirp_feed(&decoder, samples, n);
        if (!chirp_complete(&decoder)) {
            fprintf(stderr, "Incomplete: %u%% (%u valid, %u rejected)\n", chirp_progress(&decoder), decoder.valid_frames, decoder.rejected_frames);
            return 1;
        }
    }
    if (!strcmp(argv[1], "open-invitation") || !strcmp(argv[1], "decode-invitation")) {
        PairingInvitation invitation; char public_hex[65]; unsigned comparison;
        if (!pairing_invitation(decoder.bytes, decoder.length, &invitation)) return 1;
        pairing_claim_key(&invitation, secret, public_hex, &comparison);
        PairingSettings *s = &invitation.destination;
        printf("%s %u %s %u %u %06u %s ", s->host, s->port, s->path, s->tailnet, s->tls, comparison, public_hex);
        for (unsigned i = 0; i < 24; i++) printf("%02x", invitation.secret[i]);
        puts(""); return 0;
    }
    PairingSettings settings; unsigned comparison;
    if (!pairing_open(decoder.bytes, decoder.length, secret, session, &settings, &comparison)) {
        fprintf(stderr, "Envelope rejected\n"); return 1;
    }
    printf("%s %u %s %u %u %06u %08x\n", settings.host, settings.port, settings.path,
        settings.tailnet, settings.tls, comparison, pairing_crc((const uint8_t *)settings.token, strlen(settings.token)));
    return 0;
}
