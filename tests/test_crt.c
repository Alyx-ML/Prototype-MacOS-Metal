/*
 * Unit tests of the MSVCR80 formatted I/O and stream functions.
 *
 * The tests call the runtime import functions the same way as the guest
 * code: the arguments are on the guest stack and the result is in eax. The
 * expected values are the documented results of the Microsoft C runtime 8.0
 * (Visual C++ 2005).
 *
 * tests/harness.c has the shared helpers.
 */
#include "harness.h"
#include "rt_file.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* The import functions under test (rt_imports.h is not included because
   llasm_cpu.h has no include guard and rt.h includes it already). */
#define IMPORT(n) void rt_##n(_cpu *cpu);
IMPORT(sprintf) IMPORT(_snprintf) IMPORT(_snprintf_s) IMPORT(_vsnprintf) IMPORT(wsprintfA)
IMPORT(sscanf) IMPORT(sscanf_s) IMPORT(swscanf) IMPORT(fprintf)
IMPORT(fopen) IMPORT(fopen_s) IMPORT(fclose) IMPORT(fputs) IMPORT(fputc) IMPORT(fgets) IMPORT(getc)
IMPORT(ungetc) IMPORT(ftell) IMPORT(fseek) IMPORT(rewind) IMPORT(fread) IMPORT(fwrite) IMPORT(feof)
IMPORT(ferror) IMPORT(_stat64)
#undef IMPORT

/* sprintf with one conversion and a 32-bit argument. */
static void pf_i(const char *fmt, uint32_t v, const char *want)
{
    uint32_t buf = gbuf(512), r;
    argv_t a = { .n = 0 };
    a32(&a, buf);
    a32(&a, gs(fmt));
    a32(&a, v);
    r = callv(rt_sprintf, &a);
    check_str(fmt, buf, want);
    check_u32(fmt, r, (uint32_t)strlen(want));
}

static void pf_d(const char *fmt, double v, const char *want)
{
    uint32_t buf = gbuf(512);
    argv_t a = { .n = 0 };
    a32(&a, buf);
    a32(&a, gs(fmt));
    af(&a, v);
    callv(rt_sprintf, &a);
    check_str(fmt, buf, want);
}

static double bits_double(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* ---------------------------------------------------------------- printf */

static void test_printf(void)
{
    uint32_t buf, r;
    argv_t a;

    pf_i("%d", 42, "42");
    pf_i("%5d|", 42, "   42|");
    pf_i("%-5d|", 42, "42   |");
    pf_i("%05d", 42, "00042");
    pf_i("%05d", (uint32_t)-42, "-0042");
    pf_i("%+d", 5, "+5");
    pf_i("% d", 5, " 5");
    pf_i("%.3d", 7, "007");
    pf_i("%08.3d", 7, "     007");
    pf_i("%.0d", 0, "");
    pf_i("%u", 0xFFFFFFFFu, "4294967295");
    pf_i("%x", 255, "ff");
    pf_i("%X", 255, "FF");
    pf_i("%#x", 255, "0xff");
    pf_i("%#x", 0, "0");
    pf_i("%o", 255, "377");
    pf_i("%#o", 255, "0377");
    pf_i("%hd", 0x12345678u, "22136");
    pf_i("%p", 0x1234, "00001234");
    pf_i("%c", 'A', "A");
    pf_i("%3c|", 'A', "  A|");
    pf_i("%s", 0, "(null)");
    pf_i("%5.1s|", gs("abc"), "    a|");
    pf_i("%-6s|", gs("abc"), "abc   |");
    pf_i("%ls", gws("wide"), "wide");
    pf_i("%S", gws("wide"), "wide");
    pf_i("100%%", 0, "100%");

    buf = gbuf(64);
    a = (argv_t){ .n = 0 };
    a32(&a, buf);
    a32(&a, gs("%I64d %I64u %lld %I64x"));
    a64(&a, (uint64_t)-1);
    a64(&a, UINT64_C(9223372036854775808));
    a64(&a, (uint64_t)-5);
    a64(&a, UINT64_C(0x123456789ABC));
    callv(rt_sprintf, &a);
    check_str("64-bit integers", buf, "-1 9223372036854775808 -5 123456789abc");

    buf = gbuf(64);
    a = (argv_t){ .n = 0 };
    a32(&a, buf);
    a32(&a, gs("%*d|%-*d|%.*f"));
    a32(&a, 4);
    a32(&a, 7);
    a32(&a, (uint32_t)-4);
    a32(&a, 7);
    a32(&a, 2);
    af(&a, 3.14159);
    callv(rt_sprintf, &a);
    check_str("star width and precision", buf, "   7|7   |3.14");

    /* Floating point: 3-digit exponents, 17 significant digits. */
    pf_d("%f", 1.5, "1.500000");
    pf_d("%e", 1234.5, "1.234500e+003");
    pf_d("%E", 0.000123, "1.230000E-004");
    pf_d("%e", 0.0, "0.000000e+000");
    pf_d("%.3e", 9.9996, "1.000e+001");
    pf_d("%g", 1e6, "1e+006");
    pf_d("%g", 100000.0, "100000");
    pf_d("%g", 0.0001, "0.0001");
    pf_d("%g", 0.00001, "1e-005");
    pf_d("%g", 0.0, "0");
    pf_d("%g", 1.5, "1.5");
    pf_d("%#.3g", 1.0, "1.00");
    pf_d("%.15g", 0.1, "0.1");
    pf_d("%.17g", 0.1, "0.10000000000000001");
    pf_d("%.20f", 0.1, "0.10000000000000001000");
    pf_d("%.2f", 2.675, "2.67");
    pf_d("%.0f", 0.5, "1");
    pf_d("%.0f", 2.5, "3");
    pf_d("%.2f", 0.006, "0.01");
    pf_d("%.2f", 0.0006, "0.00");
    pf_d("%.3f", 0.0123, "0.012");
    pf_d("%10.4f|", 3.14159265, "    3.1416|");
    pf_d("%-10.2e|", 12345.678, "1.23e+004 |");
    pf_d("%+.1f", 2.0, "+2.0");
    pf_d("%010.2f", -3.5, "-000003.50");
    pf_d("%f", 1e20, "100000000000000000000.000000");
    pf_d("%f", -0.0, "-0.000000");

    /* Infinity and NaN in the Microsoft form. */
    pf_d("%f", HUGE_VAL, "1.#INF00");
    pf_d("%f", -HUGE_VAL, "-1.#INF00");
    pf_d("%f", bits_double(UINT64_C(0xFFF8000000000000)), "-1.#IND00");
    pf_d("%f", bits_double(UINT64_C(0x7FF8000000000000)), "1.#QNAN0");
    pf_d("%f", bits_double(UINT64_C(0x7FF0000000000001)), "1.#SNAN0");
    pf_d("%.2f", HUGE_VAL, "1.#J");
    pf_d("%.3f", HUGE_VAL, "1.#IO");
    pf_d("%e", HUGE_VAL, "1.#INF00e+000");
    pf_d("%g", HUGE_VAL, "1.#INF");
    pf_d("%10f|", HUGE_VAL, "  1.#INF00|");

    /* _snprintf: no terminator when the output fills the buffer. */
    buf = gbuf(16);
    memset(G2H(buf), 'x', 16);
    a = (argv_t){ .n = 0 };
    a32(&a, buf);
    a32(&a, 5);
    a32(&a, gs("%s"));
    a32(&a, gs("hello"));
    r = callv(rt__snprintf, &a);
    check_u32("_snprintf exact fit", r, 5);
    check_bytes("_snprintf exact fit", G2H(buf), 6, "hellox", 6);
    a.w[3] = gs("hello!");
    r = callv(rt__snprintf, &a);
    check_u32("_snprintf overflow", r, 0xFFFFFFFFu);
    check_bytes("_snprintf overflow", G2H(buf), 6, "hellox", 6);
    a.w[3] = gs("hi");
    r = callv(rt__snprintf, &a);
    check_u32("_snprintf short", r, 2);
    check_str("_snprintf short", buf, "hi");

    /* _snprintf_s with _TRUNCATE and with a count below the size. */
    buf = gbuf(16);
    a = (argv_t){ .n = 0 };
    a32(&a, buf);
    a32(&a, 8);
    a32(&a, 0xFFFFFFFFu);
    a32(&a, gs("%s"));
    a32(&a, gs("abcdefghij"));
    r = callv(rt__snprintf_s, &a);
    check_u32("_snprintf_s _TRUNCATE", r, 0xFFFFFFFFu);
    check_str("_snprintf_s _TRUNCATE", buf, "abcdefg");
    a.w[2] = 3;
    r = callv(rt__snprintf_s, &a);
    check_u32("_snprintf_s count 3", r, 0xFFFFFFFFu);
    check_str("_snprintf_s count 3", buf, "abc");
    a.w[2] = 0xFFFFFFFFu;
    a.w[4] = gs("abc");
    r = callv(rt__snprintf_s, &a);
    check_u32("_snprintf_s fits", r, 3);
    check_str("_snprintf_s fits", buf, "abc");

    /* _vsnprintf with a guest va_list. */
    {
        uint32_t ap = gbuf(16);
        wr32(ap, 12);
        wr32(ap + 4, gs("x"));
        buf = gbuf(32);
        a = (argv_t){ .n = 0 };
        a32(&a, buf);
        a32(&a, 32);
        a32(&a, gs("%d-%s"));
        a32(&a, ap);
        r = callv(rt__vsnprintf, &a);
        check_u32("_vsnprintf", r, 4);
        check_str("_vsnprintf", buf, "12-x");
    }

    /* wsprintfA: no floating point; unknown conversions are copied. */
    buf = gbuf(1100);
    a = (argv_t){ .n = 0 };
    a32(&a, buf);
    a32(&a, gs("%d %s %5u %lx %S %f"));
    a32(&a, (uint32_t)-3);
    a32(&a, gs("str"));
    a32(&a, 7);
    a32(&a, 0xABCu);
    a32(&a, gws("wide"));
    r = callv(rt_wsprintfA, &a);
    check_str("wsprintfA", buf, "-3 str     7 abc wide f");
    check_u32("wsprintfA length", r, 23);
}

/* ---------------------------------------------------------------- scanf */

static void test_scanf(void)
{
    uint32_t i1 = gbuf(8), i2 = gbuf(8), f1 = gbuf(8), s1 = gbuf(64), s2 = gbuf(64), r;
    argv_t a;
    float fv;
    double dv;

    a = (argv_t){ .n = 0 };
    a32(&a, gs("  12 abc 3.5"));
    a32(&a, gs("%d %s %f"));
    a32(&a, i1);
    a32(&a, s1);
    a32(&a, f1);
    r = callv(rt_sscanf, &a);
    check_u32("sscanf count", r, 3);
    check_u32("sscanf %d", rd32(i1), 12);
    check_str("sscanf %s", s1, "abc");
    memcpy(&fv, G2H(f1), 4);
    check_u32("sscanf %f", fv == 3.5f, 1);

    a = (argv_t){ .n = 0 };
    a32(&a, gs("0x1F 017 ff -9"));
    a32(&a, gs("%i %i %x %u"));
    a32(&a, i1);
    a32(&a, i2);
    a32(&a, f1);
    a32(&a, s1);
    r = callv(rt_sscanf, &a);
    check_u32("sscanf %i count", r, 4);
    check_u32("sscanf %i hex", rd32(i1), 31);
    check_u32("sscanf %i octal", rd32(i2), 15);
    check_u32("sscanf %x", rd32(f1), 255);
    check_u32("sscanf %u negative", rd32(s1), (uint32_t)-9);

    a = (argv_t){ .n = 0 };
    a32(&a, gs(""));
    a32(&a, gs("%d"));
    a32(&a, i1);
    check_u32("sscanf empty input", callv(rt_sscanf, &a), 0xFFFFFFFFu);

    a = (argv_t){ .n = 0 };
    a32(&a, gs("abc"));
    a32(&a, gs("%d"));
    a32(&a, i1);
    check_u32("sscanf matching failure", callv(rt_sscanf, &a), 0);

    a = (argv_t){ .n = 0 };
    a32(&a, gs("key=value;rest"));
    a32(&a, gs("%[^=]=%[^;]"));
    a32(&a, s1);
    a32(&a, s2);
    check_u32("sscanf %[ count", callv(rt_sscanf, &a), 2);
    check_str("sscanf %[ key", s1, "key");
    check_str("sscanf %[ value", s2, "value");

    a = (argv_t){ .n = 0 };
    a32(&a, gs("1e5x"));
    a32(&a, gs("%lf%c"));
    a32(&a, f1);
    a32(&a, s1);
    check_u32("sscanf %lf%c count", callv(rt_sscanf, &a), 2);
    memcpy(&dv, G2H(f1), 8);
    check_u32("sscanf %lf", dv == 1e5, 1);
    check_u32("sscanf %c", rd8(s1), 'x');

    a = (argv_t){ .n = 0 };
    a32(&a, gs("12345"));
    a32(&a, gs("%3d%d%n"));
    a32(&a, i1);
    a32(&a, i2);
    a32(&a, f1);
    check_u32("sscanf width count", callv(rt_sscanf, &a), 2);
    check_u32("sscanf width 1", rd32(i1), 123);
    check_u32("sscanf width 2", rd32(i2), 45);
    check_u32("sscanf %n", rd32(f1), 5);

    a = (argv_t){ .n = 0 };
    a32(&a, gs("10 %"));
    a32(&a, gs("%d %%"));
    a32(&a, i1);
    check_u32("sscanf %%", callv(rt_sscanf, &a), 1);

    /* sscanf_s: the buffer size follows %s; a small buffer fails. */
    a = (argv_t){ .n = 0 };
    a32(&a, gs("abc def"));
    a32(&a, gs("%s %s"));
    a32(&a, s1);
    a32(&a, 64);
    a32(&a, s2);
    a32(&a, 2);
    check_u32("sscanf_s small buffer", callv(rt_sscanf_s, &a), 1);
    check_str("sscanf_s first", s1, "abc");
    check_str("sscanf_s second", s2, "");

    /* swscanf: %s stores wide characters. */
    a = (argv_t){ .n = 0 };
    a32(&a, gws("42 hi"));
    a32(&a, gws("%d %s"));
    a32(&a, i1);
    a32(&a, s1);
    check_u32("swscanf count", callv(rt_swscanf, &a), 2);
    check_u32("swscanf %d", rd32(i1), 42);
    check_u32("swscanf %s 0", rd16(s1), 'h');
    check_u32("swscanf %s 1", rd16(s1 + 2), 'i');
    check_u32("swscanf %s end", rd16(s1 + 4), 0);
}

static size_t read_host(const char *path, uint8_t *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t r;
    if (f == NULL) return (size_t)-1;
    r = fread(out, 1, n, f);
    fclose(f);
    return r;
}

static void test_streams(const char *user_dir)
{
    uint32_t name = gs("C:\\Users\\Player\\Documents\\t.txt");
    uint32_t f, buf = gbuf(64), st = gbuf(64);
    uint8_t host[64];
    char hp[1024];
    size_t n;

    snprintf(hp, sizeof(hp), "%s/Documents/t.txt", user_dir);

    /* Text mode output: "\n" becomes "\r\n". */
    f = call2(rt_fopen, name, gs("w"));
    check_u32("fopen w", f != 0, 1);
    check_u32("fputs", call2(rt_fputs, gs("a\nb\n"), f), 0);
    {
        argv_t a = { .n = 0 };
        a32(&a, f);
        a32(&a, gs("%d!"));
        a32(&a, 9);
        check_u32("fprintf", callv(rt_fprintf, &a), 2);
    }
    check_u32("fputc", call2(rt_fputc, 'z', f), 'z');
    check_u32("fclose", call1(rt_fclose, f), 0);
    n = read_host(hp, host, sizeof(host));
    check_bytes("text mode file bytes", host, n, "a\r\nb\r\n9!z", 9);

    /* Text mode input. */
    f = call2(rt_fopen, name, gs("r"));
    check_u32("fopen r", f != 0, 1);
    check_u32("fgets 1", call3(rt_fgets, buf, 64, f), buf);
    check_str("fgets 1 text", buf, "a\n");
    check_u32("ftell after fgets", call1(rt_ftell, f), 3);
    check_u32("fgets 2", call3(rt_fgets, buf, 64, f), buf);
    check_str("fgets 2 text", buf, "b\n");
    check_u32("getc", call1(rt_getc, f), '9');
    check_u32("ungetc", call2(rt_ungetc, 'Q', f), 'Q');
    check_u32("ftell after ungetc", call1(rt_ftell, f), 6);
    check_u32("getc after ungetc", call1(rt_getc, f), 'Q');
    check_u32("fgets 3", call3(rt_fgets, buf, 64, f), buf);
    check_str("fgets 3 text", buf, "!z");
    check_u32("feof", call1(rt_feof, f), 0x10);
    check_u32("fgets at end", call3(rt_fgets, buf, 64, f), 0);
    check_u32("write on read stream", call2(rt_fputc, 'x', f), 0xFFFFFFFFu);
    check_u32("ferror", call1(rt_ferror, f), 0x20);
    call1(rt_rewind, f);
    check_u32("ferror after rewind", call1(rt_ferror, f), 0);
    check_u32("fseek", call3(rt_fseek, f, 2, 0), 0);
    check_u32("fread text", call4(rt_fread, buf, 1, 64, f), 6);
    check_bytes("fread text bytes", G2H(buf), 6, "\nb\n9!z", 6);
    check_u32("fclose r", call1(rt_fclose, f), 0);

    /* Binary mode. */
    f = call2(rt_fopen, name, gs("rb"));
    check_u32("fread binary", call4(rt_fread, buf, 3, 10, f), 3);
    check_bytes("fread binary bytes", G2H(buf), 9, "a\r\nb\r\n9!z", 9);
    check_u32("fseek end", call3(rt_fseek, f, -1, 2), 0);
    check_u32("ftell end", call1(rt_ftell, f), 8);
    check_u32("fclose rb", call1(rt_fclose, f), 0);

    /* Ctrl+Z ends a text file. */
    {
        uint32_t z = gs("C:\\Users\\Player\\Documents\\z.txt");
        f = call2(rt_fopen, z, gs("wb"));
        call4(rt_fwrite, gs("ab\x1A" "cd"), 1, 5, f);
        call1(rt_fclose, f);
        f = call2(rt_fopen, z, gs("rt"));
        check_u32("fread to Ctrl+Z", call4(rt_fread, buf, 1, 64, f), 2);
        check_u32("feof at Ctrl+Z", call1(rt_feof, f), 0x10);
        call1(rt_fclose, f);
    }

    /* Update mode: write, seek, read. */
    f = call2(rt_fopen, name, gs("r+b"));
    check_u32("fwrite update", call4(rt_fwrite, gs("XY"), 1, 2, f), 2);
    check_u32("read after write fails", call1(rt_getc, f), 0xFFFFFFFFu);
    call1(rt_rewind, f);
    check_u32("getc after rewind", call1(rt_getc, f), 'X');
    check_u32("fclose r+b", call1(rt_fclose, f), 0);

    /* fopen_s of a missing file: ENOENT. */
    {
        uint32_t pf = gbuf(4);
        check_u32("fopen_s missing", call3(rt_fopen_s, pf, gs("C:\\Prototype\\none.txt"), gs("r")), 2);
        check_u32("fopen_s missing pointer", rd32(pf), 0);
    }

    /* _stat64. */
    check_u32("_stat64 file", call2(rt__stat64, name, st), 0);
    check_u32("_stat64 mode", rd16(st + 6), 0x81B6);
    check_u32("_stat64 size", (uint32_t)rd64(st + 24), 9);
    check_u32("_stat64 drive", rd32(st), 2);
    check_u32("_stat64 directory", call2(rt__stat64, gs("C:\\Users\\Player\\Documents"), st), 0);
    check_u32("_stat64 directory mode", rd16(st + 6), 0x41FF);
    check_u32("_stat64 missing", call2(rt__stat64, gs("C:\\Prototype\\none.txt"), st), 0xFFFFFFFFu);
    check_u32("_stat64 wildcard", call2(rt__stat64, gs("C:\\Prototype\\*.txt"), st), 0xFFFFFFFFu);
    check_u32("_stat64 root", call2(rt__stat64, gs("C:\\"), st), 0);
    check_u32("_stat64 root mode", rd16(st + 6), 0x41FF);
}

int main(int argc, char **argv)
{
    char base[1024], game[1100], user[1100], pd[1100];
    const char *tmp = getenv("TMPDIR");
    if (argc != 1 || argv[0] == NULL) return 2;
    snprintf(base, sizeof(base), "%s/rt_test_crt.XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (mkdtemp(base) == NULL)
    {
        perror("mkdtemp");
        return 2;
    }
    snprintf(game, sizeof(game), "%s/game", base);
    snprintf(user, sizeof(user), "%s/user", base);
    snprintf(pd, sizeof(pd), "%s/programdata", base);
    if (mkdir(game, 0755) != 0)
    {
        perror("mkdir");
        return 2;
    }

    harness_init();
    rt_fs_init(game, user, pd);

    test_printf();
    test_scanf();
    test_streams(user);

    return harness_report();
}
