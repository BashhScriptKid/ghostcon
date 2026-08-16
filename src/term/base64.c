#include "ghostcon/term/base64.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t
ghostcon_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t need = ((len + 2) / 3) * 4;
    if (out_len < need + 1)
        return 0;

    size_t oi = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[oi++] = B64_ALPHABET[(n >> 18) & 0x3F];
        out[oi++] = B64_ALPHABET[(n >> 12) & 0x3F];
        out[oi++] = B64_ALPHABET[(n >> 6) & 0x3F];
        out[oi++] = B64_ALPHABET[n & 0x3F];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[oi++] = B64_ALPHABET[(n >> 18) & 0x3F];
        out[oi++] = B64_ALPHABET[(n >> 12) & 0x3F];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[oi++] = B64_ALPHABET[(n >> 18) & 0x3F];
        out[oi++] = B64_ALPHABET[(n >> 12) & 0x3F];
        out[oi++] = B64_ALPHABET[(n >> 6) & 0x3F];
        out[oi++] = '=';
    }
    out[oi] = '\0';
    return oi;
}

static int
b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1; /* invalid/whitespace/padding -- caller skips */
}

size_t
ghostcon_base64_decode(const char *in, uint8_t *out, size_t out_len)
{
    size_t oi = 0;
    uint32_t acc = 0;
    int nbits = 0;

    for (const char *p = in; *p && oi < out_len; p++) {
        int v = b64_val(*p);
        if (v < 0)
            continue; /* skip invalid/padding/whitespace */
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[oi++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }
    return oi;
}
