/* fmt.h - building report lines without printf (no heap, no float printf support needed).
 */
#ifndef FMT_H
#define FMT_H

#include <stdint.h>

typedef struct {
    char *buf;
    uint32_t len, cap;
} fmt_t;

void fmt_init(fmt_t *f, char *buf, uint32_t cap);
void fmt_str(fmt_t *f, const char *s);
void fmt_u32(fmt_t *f, uint32_t v);
void fmt_u32_pad(fmt_t *f, uint32_t v, int digits);  /* zero-padded to at least digits */
void fmt_i32(fmt_t *f, int32_t v);
void fmt_hex32(fmt_t *f, uint32_t v);
void fmt_fix(fmt_t *f, double v, int decimals);     /* fixed point, rounded; "nan" if not finite */

/* " key=value" */
void fmt_kv_str(fmt_t *f, const char *key, const char *v);
void fmt_kv_u32(fmt_t *f, const char *key, uint32_t v);
void fmt_kv_i32(fmt_t *f, const char *key, int32_t v);
void fmt_kv_fix(fmt_t *f, const char *key, double v, int decimals);

/* Parse a non-negative decimal like "3", "2.5" or "0.05" scaled by 10^6 (seconds -> microseconds).
 * Returns 0 on success. */
int parse_micro(const char *s, uint32_t *out);
int parse_u32(const char *s, uint32_t *out);

uint32_t crc32_update(uint32_t crc, const void *data, uint32_t n);  /* start from 0; zlib-compatible */

#endif
