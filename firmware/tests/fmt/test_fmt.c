/* test_fmt.c - report formatting, number parsing and the dump CRC, on the PC.
 */
#include <stdio.h>
#include <string.h>

#include "fmt.h"

static int failures;

static void expect_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) { failures++; printf("  FAIL %s: \"%s\", expected \"%s\"\n", what, got, want); }
}

static void fix(double v, int d, const char *want)
{
    char b[64];
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    fmt_fix(&f, v, d);
    expect_str(b, want, "fmt_fix");
}

static void micro(const char *s, int rc, unsigned long want)
{
    uint32_t v = 0;
    int got = parse_micro(s, &v);
    if (got != rc || (rc == 0 && v != want)) {
        failures++;
        printf("  FAIL parse_micro(\"%s\") = %d / %lu, expected %d / %lu\n", s, got, (unsigned long)v, rc, want);
    }
}

int main(void)
{
    char b[128];
    fmt_t f;

    printf("Formatting, parsing and CRC-32\n");
    fix(2128.8751234, 6, "2128.875123");
    fix(-0.0004, 3, "0.000");
    fix(-12.25, 1, "-12.3");
    fix(0.5, 0, "1");
    fix(50000.0123456, 3, "50000.012");
    fix(1.0 / 0.0, 2, "nan");

    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "FIT");
    fmt_kv_u32(&f, "id", 7);
    fmt_kv_i32(&f, "status", -3);
    fmt_kv_fix(&f, "f_Hz", 2128.875, 3);
    fmt_str(&f, " crc32=");
    fmt_hex32(&f, 0x0badf00du);
    expect_str(b, "FIT id=7 status=-3 f_Hz=2128.875 crc32=0badf00d", "key=value line");

    fmt_init(&f, b, sizeof b);
    fmt_u32_pad(&f, 7, 2);
    fmt_str(&f, ":");
    fmt_u32_pad(&f, 2026, 4);
    fmt_str(&f, ":");
    fmt_u32_pad(&f, 41230, 6);
    fmt_str(&f, ":");
    fmt_u32_pad(&f, 123, 2);
    expect_str(b, "07:2026:041230:123", "zero padding");

    fmt_init(&f, b, 8);
    fmt_str(&f, "overflowing");
    expect_str(b, "overflo", "truncation");

    micro("3", 0, 3000000);
    micro("2.5", 0, 2500000);
    micro("0.05", 0, 50000);
    micro("4.0000009", 0, 4000000);
    micro("", -1, 0);
    micro("abc", -1, 0);
    micro("1.2.3", -1, 0);
    micro("5000", -1, 0);
    micro(".5", 0, 500000);

    /* The standard CRC-32 check value, and the empty string */
    unsigned long crc = crc32_update(0, "123456789", 9);
    if (crc != 0xCBF43926ul) { failures++; printf("  FAIL crc32(\"123456789\") = %08lx\n", crc); }
    if (crc32_update(0, "", 0) != 0u) { failures++; printf("  FAIL crc32(\"\")\n"); }
    /* in two pieces = in one */
    if (crc32_update(crc32_update(0, "12345", 5), "6789", 4) != 0xCBF43926u) { failures++; printf("  FAIL crc32 chained\n"); }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
