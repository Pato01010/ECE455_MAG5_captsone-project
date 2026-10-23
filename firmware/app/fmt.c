/* fmt.c - report formatting, number parsing, CRC-32.
 */
#include "fmt.h"

#include <math.h>
#include <stdbool.h>

void fmt_init(fmt_t *f, char *buf, uint32_t cap)
{
    f->buf = buf;
    f->cap = cap;
    f->len = 0;
    if (cap) buf[0] = '\0';
}

static void put(fmt_t *f, char c)
{
    if (f->len + 1u < f->cap) {
        f->buf[f->len++] = c;
        f->buf[f->len] = '\0';
    }
}

void fmt_str(fmt_t *f, const char *s)
{
    while (*s) put(f, *s++);
}

static void put_u64(fmt_t *f, uint64_t v, int min_digits)
{
    char d[24];
    int n = 0;
    do { d[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v && n < 24);
    while (n < min_digits && n < 24) d[n++] = '0';
    while (n) put(f, d[--n]);
}

void fmt_u32(fmt_t *f, uint32_t v) { put_u64(f, v, 1); }
void fmt_u32_pad(fmt_t *f, uint32_t v, int digits) { put_u64(f, v, digits); }

void fmt_i32(fmt_t *f, int32_t v)
{
    if (v < 0) { put(f, '-'); put_u64(f, (uint64_t)(-(int64_t)v), 1); }
    else put_u64(f, (uint64_t)v, 1);
}

void fmt_hex32(fmt_t *f, uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) put(f, hex[(v >> i) & 15u]);
}

void fmt_fix(fmt_t *f, double v, int decimals)
{
    uint64_t scale = 1;
    if (!isfinite(v)) { fmt_str(f, "nan"); return; }
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    for (int i = 0; i < decimals; i++) scale *= 10u;
    bool negative = v < 0.0;
    if (negative) v = -v;
    if (v * (double)scale > 9.0e18) { fmt_str(f, "big"); return; }
    uint64_t x = (uint64_t)(v * (double)scale + 0.5);
    if (negative && x != 0u) put(f, '-');
    put_u64(f, x / scale, 1);
    if (decimals) {
        put(f, '.');
        put_u64(f, x % scale, decimals);
    }
}

void fmt_kv_str(fmt_t *f, const char *key, const char *v) { put(f, ' '); fmt_str(f, key); put(f, '='); fmt_str(f, v); }
void fmt_kv_u32(fmt_t *f, const char *key, uint32_t v) { put(f, ' '); fmt_str(f, key); put(f, '='); fmt_u32(f, v); }
void fmt_kv_i32(fmt_t *f, const char *key, int32_t v) { put(f, ' '); fmt_str(f, key); put(f, '='); fmt_i32(f, v); }
void fmt_kv_fix(fmt_t *f, const char *key, double v, int decimals)
{
    put(f, ' ');
    fmt_str(f, key);
    put(f, '=');
    fmt_fix(f, v, decimals);
}

int parse_u32(const char *s, uint32_t *out)
{
    uint64_t v = 0;
    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFu) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int parse_micro(const char *s, uint32_t *out)
{
    uint64_t whole = 0, frac = 0;
    int frac_digits = 0, digits = 0;
    if (!s || !*s) return -1;
    for (; *s && *s != '.'; s++) {
        if (*s < '0' || *s > '9') return -1;
        whole = whole * 10u + (uint64_t)(*s - '0');
        if (whole > 4000u) return -1;
        digits++;
    }
    if (*s == '.') {
        for (s++; *s; s++) {
            if (*s < '0' || *s > '9') return -1;
            if (frac_digits < 6) { frac = frac * 10u + (uint64_t)(*s - '0'); frac_digits++; }
            digits++;
        }
    }
    if (!digits) return -1;
    while (frac_digits < 6) { frac *= 10u; frac_digits++; }
    *out = (uint32_t)(whole * 1000000u + frac);
    return 0;
}

uint32_t crc32_update(uint32_t crc, const void *data, uint32_t n)
{
    static const uint32_t nib[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ nib[crc & 15u];
        crc = (crc >> 4) ^ nib[crc & 15u];
    }
    return ~crc;
}
