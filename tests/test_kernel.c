/*
 * Unit tests of the KERNEL32 virtual memory, system information, locale,
 * code page and FormatMessageA functions (runtime/src/rt_kernel_misc.c).
 * The expected values are the documented Windows XP results.
 */
#include "harness.h"

#include <stdio.h>

void rt_VirtualAlloc(_cpu *cpu);
void rt_VirtualFree(_cpu *cpu);
void rt_GetProcessHeap(_cpu *cpu);
void rt_HeapFree(_cpu *cpu);
void rt_GetVersionExA(_cpu *cpu);
void rt_GlobalMemoryStatus(_cpu *cpu);
void rt_GetLocaleInfoA(_cpu *cpu);
void rt_MultiByteToWideChar(_cpu *cpu);
void rt_FormatMessageA(_cpu *cpu);
void rt_lstrlenA(_cpu *cpu);
void rt_lstrlenW(_cpu *cpu);
void rt_GetLastError(_cpu *cpu);

static uint32_t mb2wc(uint32_t cp, uint32_t flags, uint32_t src, int32_t srclen, uint32_t dst, int32_t dstlen)
{
    argv_t a = { .n = 0 };
    a32(&a, cp);
    a32(&a, flags);
    a32(&a, src);
    a32(&a, (uint32_t)srclen);
    a32(&a, dst);
    a32(&a, (uint32_t)dstlen);
    return callv(rt_MultiByteToWideChar, &a);
}

static uint32_t last_error(void)
{
    argv_t a = { .n = 0 };
    return callv(rt_GetLastError, &a);
}

static void test_virtual_memory(void)
{
    uint32_t base, p;
    base = call4(rt_VirtualAlloc, 0, 0x30000, 0x2000, 4); /* MEM_RESERVE */
    check_u32("reserve", base != 0 && (base & 0xFFFF) == 0, 1);
    check_u32("reserved memory is not committed", rt_va_is_committed(base, 1), 0);
    p = call4(rt_VirtualAlloc, base + 0x1234, 0x10, 0x1000, 4); /* MEM_COMMIT */
    check_u32("commit returns the page", p, base + 0x1000);
    check_u32("committed page", rt_va_is_committed(base + 0x1000, 0x1000), 1);
    wr32(base + 0x1234, 0x11223344u);
    check_u32("committed memory is writable", rd32(base + 0x1234), 0x11223344u);
    check_u32("decommit", call3(rt_VirtualFree, base + 0x1000, 0x1000, 0x4000), 1);
    check_u32("decommitted page", rt_va_is_committed(base + 0x1000, 1), 0);
    check_u32("release with size fails", call3(rt_VirtualFree, base, 0x1000, 0x8000), 0);
    check_u32("release with size error", last_error(), 87);
    check_u32("release", call3(rt_VirtualFree, base, 0, 0x8000), 1);
    check_u32("second release fails", call3(rt_VirtualFree, base, 0, 0x8000), 0);
    p = call4(rt_VirtualAlloc, 0, 100, 0x1000, 4); /* MEM_COMMIT with NULL reserves too */
    check_u32("commit with NULL address", p != 0 && rt_va_is_committed(p, 100), 1);
    check_u32("release of commit", call3(rt_VirtualFree, p, 0, 0x8000), 1);
    check_u32("size 0 fails", call4(rt_VirtualAlloc, 0, 0, 0x3000, 4), 0);

    {
        argv_t a = { .n = 0 };
        uint32_t heap = callv(rt_GetProcessHeap, &a), blk = rt_heap_alloc(64);
        check_u32("process heap in PEB", rd32(rt_peb + 0x18), heap);
        check_u32("HeapFree", call3(rt_HeapFree, heap, 0, blk), 1);
        check_u32("HeapFree NULL", call3(rt_HeapFree, heap, 0, 0), 1);
    }
}

static void test_system_info(void)
{
    uint32_t vi = gbuf(156), ms = gbuf(32);
    wr32(vi, 148);
    check_u32("GetVersionExA", call1(rt_GetVersionExA, vi), 1);
    check_u32("major", rd32(vi + 4), 5);
    check_u32("minor", rd32(vi + 8), 1);
    check_u32("build", rd32(vi + 12), 2600);
    check_u32("platform", rd32(vi + 16), 2);
    check_str("service pack", vi + 20, "Service Pack 3");
    wr32(vi, 156);
    call1(rt_GetVersionExA, vi);
    check_u32("service pack major", rd16(vi + 148), 3);
    check_u32("product type", rd8(vi + 154), 1);
    wr32(vi, 100);
    check_u32("GetVersionExA bad size", call1(rt_GetVersionExA, vi), 0);
    check_u32("GetVersionExA bad size error", last_error(), 122);

    call1(rt_GlobalMemoryStatus, ms);
    check_u32("MEMORYSTATUS length", rd32(ms), 32);
    check_u32("total physical memory limit", rd32(ms + 8) <= 0x7FFFFFFFu && rd32(ms + 8) > 0, 1);
    check_u32("available memory at most total", rd32(ms + 12) <= rd32(ms + 8), 1);
    check_u32("total virtual", rd32(ms + 24), 0x7FFE0000u);

    check_u32("lstrlenA", call1(rt_lstrlenA, gs("abcd")), 4);
    check_u32("lstrlenA NULL", call1(rt_lstrlenA, 0), 0);
    check_u32("lstrlenW", call1(rt_lstrlenW, gws("abc")), 3);
}

static void test_locale(void)
{
    uint32_t buf = gbuf(64);
    check_u32("GetLocaleInfoA size", call4(rt_GetLocaleInfoA, 0x400, 0x0E, 0, 0), 2);
    check_u32("GetLocaleInfoA decimal", call4(rt_GetLocaleInfoA, 0x400, 0x0E, buf, 64), 2);
    check_str("GetLocaleInfoA decimal text", buf, ".");
    check_u32("GetLocaleInfoA language", call4(rt_GetLocaleInfoA, 0x409, 0x1001, buf, 64), 8);
    check_str("GetLocaleInfoA language text", buf, "English");
    check_u32("GetLocaleInfoA small buffer", call4(rt_GetLocaleInfoA, 0x409, 0x1001, buf, 3), 0);
    check_u32("GetLocaleInfoA small buffer error", last_error(), 122);
    check_u32("GetLocaleInfoA number", call4(rt_GetLocaleInfoA, 0x409, 0x20000000u | 0x1004, buf, 2), 2);
    check_u32("GetLocaleInfoA number value", rd32(buf), 1252);
}

static void test_code_pages(void)
{
    uint32_t w = gbuf(64);
    /* Windows-1252: 0x80 is the euro sign. */
    check_u32("cp1252 count", mb2wc(0, 0, gs("a\x80"), -1, w, 32), 3);
    check_u32("cp1252 a", rd16(w), 'a');
    check_u32("cp1252 euro", rd16(w + 2), 0x20AC);
    check_u32("cp1252 terminator", rd16(w + 4), 0);
    check_u32("size query", mb2wc(1252, 0, gs("abc"), 3, 0, 0), 3);
    check_u32("small buffer", mb2wc(1252, 0, gs("abc"), 3, w, 2), 0);
    check_u32("small buffer error", last_error(), 122);
    /* UTF-8: two- and four-byte sequences. */
    check_u32("utf8 count", mb2wc(65001, 0, gs("\xC3\xA9\xF0\x9F\x98\x80"), 6, w, 32), 3);
    check_u32("utf8 e-acute", rd16(w), 0xE9);
    check_u32("utf8 high surrogate", rd16(w + 2), 0xD83D);
    check_u32("utf8 low surrogate", rd16(w + 4), 0xDE00);
    /* Invalid UTF-8 is removed, or fails with MB_ERR_INVALID_CHARS. */
    check_u32("utf8 invalid removed", mb2wc(65001, 0, gs("a\xC0\x80" "b"), 4, w, 32), 2);
    check_u32("utf8 invalid removed 2", rd16(w + 2), 'b');
    check_u32("utf8 invalid fails", mb2wc(65001, 8, gs("a\xFF"), 2, w, 32), 0);
    check_u32("utf8 invalid error", last_error(), 1113);
    check_u32("utf8 invalid flags", mb2wc(65001, 1, gs("a"), 1, w, 32), 0);
    check_u32("utf8 invalid flags error", last_error(), 1004);
}

static uint32_t fmt_msg(uint32_t flags, uint32_t source, uint32_t id, uint32_t buf, uint32_t size, uint32_t args)
{
    argv_t a = { .n = 0 };
    a32(&a, flags);
    a32(&a, source);
    a32(&a, id);
    a32(&a, 0);
    a32(&a, buf);
    a32(&a, size);
    a32(&a, args);
    return callv(rt_FormatMessageA, &a);
}

static void test_format_message(void)
{
    uint32_t buf = gbuf(256), pbuf = gbuf(4), arr = gbuf(8);
    check_u32("system message", fmt_msg(0x1000 | 0x200, 0, 2, buf, 256, 0), 44);
    check_str("system message text", buf, "The system cannot find the file specified.\r\n");
    check_u32("unknown system message", fmt_msg(0x1000, 0, 99999, buf, 256, 0), 0);
    check_u32("unknown system message error", last_error(), 317);
    check_u32("allocate buffer", fmt_msg(0x1000 | 0x100 | 0x200, 0, 5, pbuf, 0, 0), 19);
    check_str("allocate buffer text", rd32(pbuf), "Access is denied.\r\n");
    wr32(arr, gs("one"));
    wr32(arr + 4, gs("two"));
    check_u32("inserts", fmt_msg(0x400 | 0x2000, gs("%2 and %1!s!%n%%"), 0, buf, 256, arr), 14);
    check_str("inserts text", buf, "two and one\r\n%");
    check_u32("ignore inserts", fmt_msg(0x400 | 0x200, gs("a %1 b"), 0, buf, 256, 0), 6);
    check_str("ignore inserts text", buf, "a %1 b");
    check_u32("small buffer", fmt_msg(0x1000 | 0x200, 0, 2, buf, 10, 0), 0);
    check_u32("small buffer error", last_error(), 122);
}

int main(void)
{
    harness_init();
    test_virtual_memory();
    test_system_info();
    test_locale();
    test_code_pages();
    test_format_message();
    return harness_report();
}
